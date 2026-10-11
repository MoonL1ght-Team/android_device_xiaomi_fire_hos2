/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AIDL effect library that hosts legacy effect libraries.
 *
 * Devices with an AIDL audio stack only load effect libraries that export
 * createEffect/queryEffect/destroyEffect. Proprietary effects such as
 * Dolby DAP only export the legacy AELI interface
 * (audio_effect_library_t). This library is listed in
 * audio_effects_config.xml for those effects and forwards every call to
 * the legacy library that knows the requested implementation UUID.
 *
 * Parameters arrive as VendorExtension holding the raw effect_param_t
 * the application sent, so they can be passed to EFFECT_CMD_SET_PARAM /
 * EFFECT_CMD_GET_PARAM unchanged.
 */

#define LOG_TAG "AHAL_AeliBridge"

#include <dlfcn.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <audio_utils/primitives.h>
#include <hardware/audio_effect.h>
#include <media/AidlConversionCppNdk.h>
#include <media/AidlConversionEffect.h>
#include <system/audio_aidl_utils.h>
#include <system/audio_effect.h>

#include <aidl/android/hardware/audio/effect/DefaultExtension.h>

#include "effect-impl/EffectImpl.h"

using aidl::android::hardware::audio::effect::DefaultExtension;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::EffectContext;
using aidl::android::hardware::audio::effect::EffectImpl;
using aidl::android::hardware::audio::effect::Flags;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::Parameter;
using aidl::android::hardware::audio::effect::RetCode;
using aidl::android::hardware::audio::effect::VendorExtension;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioMode;
using aidl::android::media::audio::common::AudioSource;
using aidl::android::media::audio::common::AudioUuid;

namespace {

constexpr char kConfigPath[] = "/vendor/etc/aeli_bridge.conf";

struct LegacyLib {
    std::string path;
    void* handle;
    audio_effect_library_t* aeli;
};

std::mutex gLibMutex;
std::vector<LegacyLib> gLibs;
bool gLibsLoaded = false;

void loadLibsLocked() {
    if (gLibsLoaded) return;
    gLibsLoaded = true;

    std::ifstream in(kConfigPath);
    if (!in) {
        LOG(ERROR) << "cannot read " << kConfigPath;
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = android::base::Trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (access(line.c_str(), R_OK) != 0) {
            LOG(INFO) << "not installed: " << line;
            continue;
        }
        void* handle = dlopen(line.c_str(), RTLD_NOW);
        if (!handle) {
            LOG(ERROR) << "dlopen " << line << ": " << dlerror();
            continue;
        }
        auto* aeli = static_cast<audio_effect_library_t*>(
                dlsym(handle, AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR));
        if (!aeli || aeli->tag != AUDIO_EFFECT_LIBRARY_TAG || !aeli->create_effect ||
            !aeli->release_effect || !aeli->get_descriptor) {
            LOG(ERROR) << line << " is not a legacy effect library";
            dlclose(handle);
            continue;
        }
        LOG(INFO) << "loaded " << line << " (" << (aeli->name ? aeli->name : "?") << ")";
        gLibs.push_back({line, handle, aeli});
    }
}

effect_uuid_t toLegacy(const AudioUuid& u) {
    effect_uuid_t r = {};
    r.timeLow = static_cast<uint32_t>(u.timeLow);
    r.timeMid = static_cast<uint16_t>(u.timeMid);
    r.timeHiAndVersion = static_cast<uint16_t>(u.timeHiAndVersion);
    r.clockSeq = static_cast<uint16_t>(u.clockSeq);
    for (size_t i = 0; i < sizeof(r.node) && i < u.node.size(); i++) r.node[i] = u.node[i];
    return r;
}

AudioUuid toAidl(const effect_uuid_t& u) {
    AudioUuid r;
    r.timeLow = static_cast<int32_t>(u.timeLow);
    r.timeMid = u.timeMid;
    r.timeHiAndVersion = u.timeHiAndVersion;
    r.clockSeq = u.clockSeq;
    r.node.assign(u.node, u.node + sizeof(u.node));
    return r;
}

// Finds the legacy library implementing |uuid|.
audio_effect_library_t* findLib(const AudioUuid& uuid, effect_descriptor_t* desc) {
    std::lock_guard lg(gLibMutex);
    loadLibsLocked();
    const effect_uuid_t legacy = toLegacy(uuid);
    for (const auto& lib : gLibs) {
        if (lib.aeli->get_descriptor(&legacy, desc) == 0) return lib.aeli;
    }
    return nullptr;
}

Descriptor toAidlDescriptor(const effect_descriptor_t& legacy) {
    auto converted = aidl::android::legacy2aidl_effect_descriptor_Descriptor(legacy);
    if (converted.has_value()) return converted.value();

    LOG(WARNING) << "descriptor conversion failed for " << legacy.name << ", using defaults";
    Descriptor d;
    d.common.id.type = toAidl(legacy.type);
    d.common.id.uuid = toAidl(legacy.uuid);
    d.common.flags = {.type = Flags::Type::INSERT, .insert = Flags::Insert::LAST};
    d.common.name = legacy.name;
    d.common.implementor = legacy.implementor;
    return d;
}

uint32_t toQ824(float v) {
    if (v <= 0.f) return 0;
    if (v >= 1.f) return 1u << 24;
    return static_cast<uint32_t>(v * (1u << 24));
}

}  // namespace

namespace aidl::android::hardware::audio::effect {

class AeliContext final : public EffectContext {
  public:
    AeliContext(const Parameter::Common& common, audio_effect_library_t* lib,
                const effect_descriptor_t& desc)
        : EffectContext(1 /* statusDepth */, common), mLib(lib), mDesc(desc) {}

    ~AeliContext() override {
        if (mHandle) {
            mLib->release_effect(mHandle);
            mHandle = nullptr;
        }
    }

    bool init() {
        int ret = mLib->create_effect(&mDesc.uuid, mCommon.session, mCommon.ioHandle, &mHandle);
        if (ret != 0 || !mHandle) {
            LOG(ERROR) << mDesc.name << ": create_effect failed " << ret;
            mHandle = nullptr;
            return false;
        }
        if (int st = command(EFFECT_CMD_INIT, 0, nullptr); st != 0) {
            LOG(ERROR) << mDesc.name << ": EFFECT_CMD_INIT failed " << st;
            return false;
        }
        return configure() == RetCode::SUCCESS;
    }

    RetCode setCommon(const Parameter::Common& common) override {
        RetCode ret = EffectContext::setCommon(common);
        if (ret != RetCode::SUCCESS || !mHandle) return ret;
        return configure();
    }

    RetCode setOutputDevice(const std::vector<AudioDeviceDescription>& devices) override {
        EffectContext::setOutputDevice(devices);
        if (!mHandle || devices.empty() ||
            (mDesc.flags & EFFECT_FLAG_DEVICE_MASK) == 0) {
            return RetCode::SUCCESS;
        }
        auto legacy = ::aidl::android::aidl2legacy_AudioDeviceDescription_audio_devices_t(
                devices[0]);
        if (!legacy.has_value()) return RetCode::SUCCESS;
        uint32_t device = legacy.value();
        command(EFFECT_CMD_SET_DEVICE, sizeof(device), &device, false /* hasReply */);
        return RetCode::SUCCESS;
    }

    RetCode setAudioMode(const AudioMode& mode) override {
        EffectContext::setAudioMode(mode);
        if (!mHandle || (mDesc.flags & EFFECT_FLAG_AUDIO_MODE_MASK) == 0) {
            return RetCode::SUCCESS;
        }
        auto legacy = ::aidl::android::aidl2legacy_AudioMode_audio_mode_t(mode);
        if (!legacy.has_value()) return RetCode::SUCCESS;
        uint32_t m = legacy.value();
        command(EFFECT_CMD_SET_AUDIO_MODE, sizeof(m), &m, false /* hasReply */);
        return RetCode::SUCCESS;
    }

    RetCode setAudioSource(const AudioSource& source) override {
        EffectContext::setAudioSource(source);
        if (!mHandle || (mDesc.flags & EFFECT_FLAG_AUDIO_SOURCE_MASK) == 0) {
            return RetCode::SUCCESS;
        }
        auto legacy = ::aidl::android::aidl2legacy_AudioSource_audio_source_t(source);
        if (!legacy.has_value()) return RetCode::SUCCESS;
        uint32_t s = legacy.value();
        command(EFFECT_CMD_SET_AUDIO_SOURCE, sizeof(s), &s, false /* hasReply */);
        return RetCode::SUCCESS;
    }

    RetCode setVolumeStereo(const Parameter::VolumeStereo& volume) override {
        EffectContext::setVolumeStereo(volume);
        const uint32_t volFlag = mDesc.flags & EFFECT_FLAG_VOLUME_MASK;
        if (!mHandle || (volFlag != EFFECT_FLAG_VOLUME_CTRL && volFlag != EFFECT_FLAG_VOLUME_IND)) {
            return RetCode::SUCCESS;
        }
        uint32_t vol[2] = {toQ824(volume.left), toQ824(volume.right)};
        uint32_t reply[2] = {};
        uint32_t replySize = sizeof(reply);
        (*mHandle)->command(mHandle, EFFECT_CMD_SET_VOLUME, sizeof(vol), vol,
                            volFlag == EFFECT_FLAG_VOLUME_CTRL ? &replySize : nullptr,
                            volFlag == EFFECT_FLAG_VOLUME_CTRL ? reply : nullptr);
        return RetCode::SUCCESS;
    }

    RetCode enable() override {
        if (!mHandle) return RetCode::ERROR_EFFECT_LIB_ERROR;
        int st = command(EFFECT_CMD_ENABLE, 0, nullptr);
        if (st != 0) LOG(WARNING) << mDesc.name << ": EFFECT_CMD_ENABLE " << st;
        mEnabled = true;
        return RetCode::SUCCESS;
    }

    RetCode disable() override {
        if (!mHandle) return RetCode::SUCCESS;
        command(EFFECT_CMD_DISABLE, 0, nullptr);
        mEnabled = false;
        return RetCode::SUCCESS;
    }

    RetCode reset() override {
        if (mHandle) command(EFFECT_CMD_RESET, 0, nullptr, false /* hasReply */);
        return RetCode::SUCCESS;
    }

    IEffect::Status process(float* in, float* out, int samples) {
        IEffect::Status status = {STATUS_OK, samples, samples};
        if (!mHandle || mInputChannelCount == 0) return status;

        const size_t frames = samples / mInputChannelCount;
        status.fmqProduced = frames * mOutputChannelCount;
        if (in != out) std::memcpy(out, in, samples * sizeof(float));
        if (!mEnabled || frames == 0) return status;

        audio_buffer_t inBuf = {}, outBuf = {};
        inBuf.frameCount = outBuf.frameCount = frames;
        int ret;
        if (mFloat) {
            inBuf.f32 = in;
            outBuf.f32 = out;
            ret = (*mHandle)->process(mHandle, &inBuf, &outBuf);
        } else {
            const size_t n = std::max(frames * mInputChannelCount, frames * mOutputChannelCount);
            if (mI16.size() < n) mI16.resize(n);
            memcpy_to_i16_from_float(mI16.data(), in, frames * mInputChannelCount);
            inBuf.s16 = outBuf.s16 = mI16.data();
            ret = (*mHandle)->process(mHandle, &inBuf, &outBuf);
            if (ret == 0) memcpy_to_float_from_i16(out, mI16.data(), frames * mOutputChannelCount);
        }
        if (ret != 0 && ret != -ENODATA && !mProcessErrorLogged) {
            LOG(ERROR) << mDesc.name << ": process failed " << ret;
            mProcessErrorLogged = true;
        }
        return status;
    }

    ndk::ScopedAStatus setParam(const std::vector<uint8_t>& bytes) {
        if (!mHandle) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        uint32_t size;
        if (!paramSize(bytes, &size)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        std::vector<uint8_t> copy(bytes.begin(), bytes.begin() + size);
        int32_t reply = 0;
        uint32_t replySize = sizeof(reply);
        int ret = (*mHandle)->command(mHandle, EFFECT_CMD_SET_PARAM, size, copy.data(),
                                      &replySize, &reply);
        if (ret != 0 || reply != 0) {
            return ndk::ScopedAStatus::fromServiceSpecificError(ret != 0 ? ret : reply);
        }
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getParam(std::vector<uint8_t>& bytes) {
        if (!mHandle) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        uint32_t size;
        if (!paramSize(bytes, &size)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        const auto* p = reinterpret_cast<const effect_param_t*>(bytes.data());
        const uint32_t cmdSize = sizeof(effect_param_t) + p->psize;
        std::vector<uint8_t> cmd(bytes.begin(), bytes.begin() + cmdSize);
        std::vector<uint8_t> reply(size);
        uint32_t replySize = size;
        int ret = (*mHandle)->command(mHandle, EFFECT_CMD_GET_PARAM, cmdSize, cmd.data(),
                                      &replySize, reply.data());
        if (ret != 0) return ndk::ScopedAStatus::fromServiceSpecificError(ret);
        reply.resize(std::max<uint32_t>(replySize, sizeof(effect_param_t)));
        bytes = std::move(reply);
        return ndk::ScopedAStatus::ok();
    }

  private:
    // Sends a command whose reply, if any, is a single int32 status.
    int command(uint32_t code, uint32_t size, void* data, bool hasReply = true) {
        int32_t reply = 0;
        uint32_t replySize = sizeof(reply);
        int ret = (*mHandle)->command(mHandle, code, size, data, hasReply ? &replySize : nullptr,
                                      hasReply ? &reply : nullptr);
        return ret != 0 ? ret : reply;
    }

    static bool paramSize(const std::vector<uint8_t>& bytes, uint32_t* size) {
        if (bytes.size() < sizeof(effect_param_t)) return false;
        const auto* p = reinterpret_cast<const effect_param_t*>(bytes.data());
        const uint64_t padded = ((static_cast<uint64_t>(p->psize) - 1) / sizeof(int) + 1) *
                                sizeof(int);
        const uint64_t total = sizeof(effect_param_t) + (p->psize ? padded : 0) + p->vsize;
        if (total > bytes.size() || total > UINT32_MAX) return false;
        *size = static_cast<uint32_t>(total);
        return true;
    }

    void fillConfig(buffer_config_t& cfg, const AudioChannelLayout& layout, bool input,
                    audio_format_t format) {
        cfg = {};
        cfg.samplingRate = mCommon.input.base.sampleRate;
        auto mask = ::aidl::android::aidl2legacy_AudioChannelLayout_audio_channel_mask_t(
                layout, false /* isInput */);
        cfg.channels = mask.has_value() ? mask.value() : AUDIO_CHANNEL_OUT_STEREO;
        cfg.format = format;
        cfg.accessMode = input ? EFFECT_BUFFER_ACCESS_READ : EFFECT_BUFFER_ACCESS_WRITE;
        cfg.mask = EFFECT_CONFIG_ALL;
    }

    RetCode configure() {
        for (audio_format_t format : {AUDIO_FORMAT_PCM_FLOAT, AUDIO_FORMAT_PCM_16_BIT}) {
            effect_config_t cfg = {};
            fillConfig(cfg.inputCfg, mCommon.input.base.channelMask, true, format);
            fillConfig(cfg.outputCfg, mCommon.output.base.channelMask, false, format);
            int st = command(EFFECT_CMD_SET_CONFIG, sizeof(cfg), &cfg);
            if (st == 0) {
                mFloat = format == AUDIO_FORMAT_PCM_FLOAT;
                LOG(INFO) << mDesc.name << ": configured " << cfg.inputCfg.samplingRate
                          << " Hz, channels 0x" << std::hex << cfg.inputCfg.channels
                          << (mFloat ? ", float" : ", int16");
                return RetCode::SUCCESS;
            }
            LOG(WARNING) << mDesc.name << ": SET_CONFIG format 0x" << std::hex << format
                         << " rejected: " << std::dec << st;
        }
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }

    audio_effect_library_t* mLib;
    effect_descriptor_t mDesc;
    effect_handle_t mHandle = nullptr;
    bool mFloat = true;
    bool mEnabled = false;
    bool mProcessErrorLogged = false;
    std::vector<int16_t> mI16;
};

class AeliBridgeImpl final : public EffectImpl {
  public:
    AeliBridgeImpl(audio_effect_library_t* lib, const effect_descriptor_t& desc)
        : mLib(lib), mLegacyDesc(desc), mDesc(toAidlDescriptor(desc)) {}
    ~AeliBridgeImpl() { cleanUp(); }

    ndk::ScopedAStatus getDescriptor(Descriptor* desc) override {
        RETURN_IF(!desc, EX_ILLEGAL_ARGUMENT, "Parameter:nullptr");
        *desc = mDesc;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific)
            REQUIRES(mImplMutex) override {
        RETURN_IF(specific.getTag() != Parameter::Specific::vendorEffect, EX_ILLEGAL_ARGUMENT,
                  "onlyVendorExtensionSupported");
        RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");
        std::optional<DefaultExtension> ext;
        auto& vendor = specific.get<Parameter::Specific::vendorEffect>();
        RETURN_IF(vendor.extension.getParcelable(&ext) != STATUS_OK || !ext.has_value(),
                  EX_ILLEGAL_ARGUMENT, "noDefaultExtension");
        return mContext->setParam(ext->bytes);
    }

    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id,
                                            Parameter::Specific* specific)
            REQUIRES(mImplMutex) override {
        RETURN_IF(!specific, EX_NULL_POINTER, "nullPtr");
        RETURN_IF(id.getTag() != Parameter::Id::vendorEffectTag, EX_ILLEGAL_ARGUMENT,
                  "onlyVendorExtensionSupported");
        RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");
        std::optional<DefaultExtension> ext;
        auto& vendor = id.get<Parameter::Id::vendorEffectTag>();
        RETURN_IF(vendor.extension.getParcelable(&ext) != STATUS_OK || !ext.has_value(),
                  EX_ILLEGAL_ARGUMENT, "noDefaultExtension");
        auto status = mContext->getParam(ext->bytes);
        if (!status.isOk()) return status;
        VendorExtension out;
        out.extension.setParcelable(*ext);
        specific->set<Parameter::Specific::vendorEffect>(std::move(out));
        return ndk::ScopedAStatus::ok();
    }

    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common)
            REQUIRES(mImplMutex) override {
        if (mContext) return mContext;
        auto context = std::make_shared<AeliContext>(common, mLib, mLegacyDesc);
        if (!context->init()) return nullptr;
        mContext = context;
        return mContext;
    }

    RetCode releaseContext() REQUIRES(mImplMutex) override {
        if (mContext) {
            mContext->disable();
            mContext->resetBuffer();
            mContext.reset();
        }
        return RetCode::SUCCESS;
    }

    IEffect::Status effectProcessImpl(float* in, float* out, int samples)
            REQUIRES(mImplMutex) override {
        if (!mContext) return {EX_NULL_POINTER, 0, 0};
        return mContext->process(in, out, samples);
    }

    std::string getEffectName() override { return mDesc.common.name; }

  private:
    audio_effect_library_t* mLib;
    const effect_descriptor_t mLegacyDesc;
    const Descriptor mDesc;
    std::shared_ptr<AeliContext> mContext GUARDED_BY(mImplMutex);
};

}  // namespace aidl::android::hardware::audio::effect

extern "C" binder_exception_t createEffect(const AudioUuid* uuid,
                                           std::shared_ptr<IEffect>* instance) {
    if (!uuid || !instance) return EX_ILLEGAL_ARGUMENT;
    effect_descriptor_t desc;
    audio_effect_library_t* lib = findLib(*uuid, &desc);
    if (!lib) {
        LOG(ERROR) << __func__ << ": no legacy library for "
                   << ::android::audio::utils::toString(*uuid);
        return EX_ILLEGAL_ARGUMENT;
    }
    *instance = ndk::SharedRefBase::make<
            aidl::android::hardware::audio::effect::AeliBridgeImpl>(lib, desc);
    LOG(DEBUG) << __func__ << ": " << desc.name;
    return EX_NONE;
}

extern "C" binder_exception_t queryEffect(const AudioUuid* uuid, Descriptor* out) {
    if (!uuid || !out) return EX_ILLEGAL_ARGUMENT;
    effect_descriptor_t desc;
    if (!findLib(*uuid, &desc)) {
        // The factory fails queryEffects() for every effect if a single
        // descriptor is missing, so describe it as an inert placeholder.
        LOG(ERROR) << __func__ << ": no legacy library for "
                   << ::android::audio::utils::toString(*uuid);
        Descriptor d;
        d.common.id.type = toAidl(effect_uuid_t{});
        d.common.id.uuid = *uuid;
        d.common.flags = {.type = Flags::Type::INSERT, .insert = Flags::Insert::LAST};
        d.common.name = "aelibridge: library not installed";
        d.common.implementor = "aelibridge";
        *out = d;
        return EX_NONE;
    }
    *out = toAidlDescriptor(desc);
    return EX_NONE;
}

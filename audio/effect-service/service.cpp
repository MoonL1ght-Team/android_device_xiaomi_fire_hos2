/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AIDL audio effect factory service for fire.
 *
 * Same as the AOSP example service, but it reads its own effects config.
 * The MediaTek audio HAL builds a second, internal effect factory from
 * audio_effects_config.xml, so effects that must live in a single
 * process (ViPER4Android registers viper.control when it is loaded) are
 * only listed in audio_effects_fire.xml, which only this service reads.
 */

#define LOG_TAG "AHAL_EffectService"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <unistd.h>

#include "effectFactory-impl/EffectFactory.h"

static constexpr char kConfigFile[] = "/vendor/etc/audio_effects_fire.xml";
static constexpr char kFallbackConfigFile[] = "/vendor/etc/audio_effects_config.xml";

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    const char* config = access(kConfigFile, R_OK) == 0 ? kConfigFile : kFallbackConfigFile;
    LOG(INFO) << "starting effect factory with " << config;
    auto factory = ndk::SharedRefBase::make<aidl::android::hardware::audio::effect::Factory>(config);

    const std::string name = std::string() + factory->descriptor + "/default";
    binder_status_t status = AServiceManager_addService(factory->asBinder().get(), name.c_str());
    CHECK_EQ(STATUS_OK, status);

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}

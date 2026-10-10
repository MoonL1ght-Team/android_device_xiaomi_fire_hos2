// SPDX-License-Identifier: Apache-2.0
#include "sensors.h"
#include <aidl/android/hardware/thermal/BnThermal.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

using namespace aidl::android::hardware::thermal;
using ndk::ScopedAStatus;
namespace fs = std::filesystem;

template <class C, class T> struct Subscription {
    std::shared_ptr<C> callback;
    std::optional<T> filter;
};

class Thermal final : public BnThermal {
    const fs::path root = "/sys/class/thermal";
    std::mutex lock;
    std::vector<Subscription<IThermalChangedCallback, TemperatureType>> callbacks;
    std::vector<Subscription<ICoolingDeviceChangedCallback, CoolingType>> coolingCallbacks;
    std::map<std::string, Temperature> previous;
    std::map<std::string, long long> previousCooling;
    // thermal_zone numbering is fixed once a driver has registered, so resolve
    // each sensor once instead of rescanning /sys/class/thermal on every read.
    std::mutex zoneLock;
    std::map<std::string, fs::path> zones;

    fs::path zoneFor(const fire::Sensor& sensor) {
        std::lock_guard guard(zoneLock);
        if (const auto it = zones.find(sensor.zone); it != zones.end()) return it->second;
        auto zone = fire::findZone(root, sensor.zone);
        if (!zone.empty()) zones.emplace(sensor.zone, zone);  // retry later if not there yet
        return zone;
    }

    template <class C, class T>
    ScopedAStatus add(std::vector<Subscription<C, T>>& list, const std::shared_ptr<C>& cb,
                     std::optional<T> filter) {
        if (!cb) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard guard(lock);
        for (const auto& item : list)
            if (item.callback->asBinder() == cb->asBinder())
                return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        if (list.size() >= 64) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        list.push_back({cb, filter});
        return ScopedAStatus::ok();
    }
    template <class C, class T>
    ScopedAStatus remove(std::vector<Subscription<C, T>>& list, const std::shared_ptr<C>& cb) {
        if (!cb) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard guard(lock);
        const auto old = list.size();
        std::erase_if(list, [&](const auto& item) {return item.callback->asBinder() == cb->asBinder();});
        return old != list.size() ? ScopedAStatus::ok()
                                 : ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
public:
    ScopedAStatus getTemperatures(std::vector<Temperature>* out) override {
        out->clear();
        for (const auto& sensor : fire::sensors) {
            const auto zone = zoneFor(sensor);
            if (zone.empty()) continue;
            auto value = fire::temperature(zone / "temp");
            if (!value) continue;
            Temperature t;
            t.name = sensor.name;
            t.type = static_cast<TemperatureType>(sensor.type);
            t.value = *value;
            t.throttlingStatus = static_cast<ThrottlingSeverity>(fire::severity(*value, sensor.hot));
            out->push_back(t);
        }
        return out->empty() ? ScopedAStatus::fromServiceSpecificErrorWithMessage(1, "No readable thermal zones")
                            : ScopedAStatus::ok();
    }
    ScopedAStatus getTemperaturesWithType(TemperatureType type, std::vector<Temperature>* out) override {
        auto status = getTemperatures(out);
        std::erase_if(*out, [type](const auto& t) {return t.type != type;});
        return status;
    }
    ScopedAStatus getTemperatureThresholds(std::vector<TemperatureThreshold>* out) override {
        out->clear();
        for (const auto& sensor : fire::sensors) {
            if (zoneFor(sensor).empty()) continue;
            TemperatureThreshold t;
            t.name = sensor.name;
            t.type = static_cast<TemperatureType>(sensor.type);
            t.hotThrottlingThresholds.assign(sensor.hot.begin(), sensor.hot.end());
            t.coldThrottlingThresholds.assign(7, NAN);
            out->push_back(t);
        }
        return ScopedAStatus::ok();
    }
    ScopedAStatus getTemperatureThresholdsWithType(TemperatureType type,
                                                  std::vector<TemperatureThreshold>* out) override {
        auto status = getTemperatureThresholds(out);
        std::erase_if(*out, [type](const auto& t) {return t.type != type;});
        return status;
    }
    ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* out) override {
        out->clear();
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.path().filename().string().rfind("cooling_device", 0) != 0) continue;
            auto value = fire::readInteger(entry.path() / "cur_state");
            const auto name = fire::readText(entry.path() / "type");
            if (!value || *value < 0 || name.empty()) continue;
            CoolingDevice d;
            d.name = name;
            d.value = *value;
            d.type = CoolingType::COMPONENT;
            if (name.rfind("cpu", 0) == 0) d.type = CoolingType::CPU;
            else if (name == "mtktsbattery-sysrst") d.type = CoolingType::BATTERY;
            out->push_back(d);
        }
        return ScopedAStatus::ok();
    }
    ScopedAStatus getCoolingDevicesWithType(CoolingType type, std::vector<CoolingDevice>* out) override {
        auto status = getCoolingDevices(out);
        std::erase_if(*out, [type](const auto& d) {return d.type != type;});
        return status;
    }
    ScopedAStatus registerThermalChangedCallback(const std::shared_ptr<IThermalChangedCallback>& cb) override {
        return registerCallback(cb, std::nullopt);
    }
    ScopedAStatus registerThermalChangedCallbackWithType(const std::shared_ptr<IThermalChangedCallback>& cb,
                                                        TemperatureType type) override {
        return registerCallback(cb, type);
    }
    ScopedAStatus registerCallback(const std::shared_ptr<IThermalChangedCallback>& cb,
                                  std::optional<TemperatureType> filter) {
        auto status = add(callbacks, cb, filter);
        if (!status.isOk()) return status;
        std::vector<Temperature> current;
        getTemperatures(&current);
        for (const auto& t : current) if (!filter || *filter == t.type) {
            if (!cb->notifyThrottling(t).isOk()) {remove(callbacks, cb); break;}
        }
        return status;
    }
    ScopedAStatus unregisterThermalChangedCallback(const std::shared_ptr<IThermalChangedCallback>& cb) override {
        return remove(callbacks, cb);
    }
    ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& cb, CoolingType type) override {
        return add(coolingCallbacks, cb, std::optional<CoolingType>(type));
    }
    ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& cb) override {
        return remove(coolingCallbacks, cb);
    }
    void poll() {
        decltype(callbacks) temperatureListeners;
        decltype(coolingCallbacks) coolingListeners;
        {
            std::lock_guard guard(lock);
            temperatureListeners = callbacks;
            coolingListeners = coolingCallbacks;
        }
        if (temperatureListeners.empty() && coolingListeners.empty()) return;
        std::vector<Temperature> temperatures;
        if (!getTemperatures(&temperatures).isOk())
            __android_log_print(ANDROID_LOG_ERROR, "fire-thermal", "No readable thermal zones");
        for (const auto& t : temperatures) {
            const auto old = previous.find(t.name);
            const bool changed = old == previous.end() || old->second.throttlingStatus != t.throttlingStatus;
            previous[t.name] = t;
            if (!changed) continue;
            for (const auto& s : temperatureListeners) if (!s.filter || *s.filter == t.type) {
                if (!s.callback->notifyThrottling(t).isOk()) remove(callbacks, s.callback);
            }
        }
        if (coolingListeners.empty()) return;
        std::vector<CoolingDevice> devices;
        getCoolingDevices(&devices);
        for (const auto& d : devices) {
            const auto old = previousCooling.find(d.name);
            const bool changed = old == previousCooling.end() || old->second != d.value;
            previousCooling[d.name] = d.value;
            if (!changed) continue;
            for (const auto& s : coolingListeners) if (!s.filter || *s.filter == d.type) {
                if (!s.callback->notifyCoolingDeviceChanged(d).isOk()) remove(coolingCallbacks, s.callback);
            }
        }
    }
};

int main(int argc, char** argv) {
    // --check reads sensors only; it does not register a Binder service or change the device.
    auto thermal = ndk::SharedRefBase::make<Thermal>();
    if (argc == 2 && std::string(argv[1]) == "--check") {
        std::vector<Temperature> values;
        if (!thermal->getTemperatures(&values).isOk()) return 1;
        for (const auto& t : values)
            std::printf("%s type=%d value=%.3f severity=%d\n", t.name.c_str(), static_cast<int>(t.type),
                        t.value, static_cast<int>(t.throttlingStatus));
        return values.size() == fire::sensors.size() ? 0 : 2;
    }
    if (argc != 1) {std::fprintf(stderr, "Usage: fire-thermal [--check]\n"); return 2;}
    ABinderProcess_setThreadPoolMaxThreadCount(3);
    const char* instance = "android.hardware.thermal.IThermal/default";
    if (!AServiceManager_isDeclared(instance)) {
        __android_log_print(ANDROID_LOG_ERROR, "fire-thermal", "Not declared in VINTF; refusing startup");
        return 1;
    }
    const auto result = AServiceManager_addService(thermal->asBinder().get(), instance);
    if (result != STATUS_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "fire-thermal", "addService failed: %d", result);
        return 1;
    }
    __android_log_print(ANDROID_LOG_INFO, "fire-thermal", "Registered; reporting device-tree thresholds; no cooling writes");
    ABinderProcess_startThreadPool();
    for (;;) {thermal->poll(); std::this_thread::sleep_for(std::chrono::seconds(2));}
}

/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#include <sys/sysinfo.h>
#include <android-base/properties.h>

#include "property_service.h"
#include "vendor_init.h"

using android::base::GetProperty;
using std::string;

void property_override(string prop, string value)
{
    auto pi = (prop_info*) __system_property_find(prop.c_str());

    if (pi != nullptr)
        __system_property_update(pi, value.c_str(), value.size());
    else
        __system_property_add(prop.c_str(), prop.size(), value.c_str(), value.size());
}

struct mem_tier {
    unsigned long long max_mb;
    const char *heapstartsize, *heapgrowthlimit, *heapsize,
               *heaptargetutilization, *heapminfree, *heapmaxfree;
    const char *partialstall, *completestall, *thrashlim, *thrashlimdec,
               *swapfreelow;
};

// Redmi 12 ships with 2/4/6/8 GB, totalram reports ~0.2-0.5 GB less.
// Heap values follow AOSP phone-xhdpi-*-dalvik-heap.mk.
static const mem_tier kTiers[] = {
    // 2 GB
    { 2560, "8m", "192m", "512m", "0.75", "512k", "8m",
      "200", "700", "30", "50", "10" },
    // 4 GB
    { 4608, "8m", "192m", "512m", "0.6", "8m", "16m",
      "100", "700", "60", "25", "10" },
    // 6 GB
    { 6656, "16m", "256m", "512m", "0.5", "8m", "32m",
      "70", "700", "80", "20", "15" },
    // 8 GB
    { ~0ull, "16m", "256m", "512m", "0.5", "8m", "32m",
      "70", "700", "100", "10", "20" },
};

void configure_dalvik_and_lmkd()
{
    struct sysinfo sys;
    sysinfo(&sys);

    const unsigned long long total_mb =
            (unsigned long long)sys.totalram * sys.mem_unit / (1024 * 1024);

    const mem_tier *t = &kTiers[0];
    while (total_mb > t->max_mb)
        t++;

    property_override("dalvik.vm.heapstartsize", t->heapstartsize);
    property_override("dalvik.vm.heapgrowthlimit", t->heapgrowthlimit);
    property_override("dalvik.vm.heapsize", t->heapsize);
    property_override("dalvik.vm.heaptargetutilization", t->heaptargetutilization);
    property_override("dalvik.vm.heapminfree", t->heapminfree);
    property_override("dalvik.vm.heapmaxfree", t->heapmaxfree);

    property_override("ro.lmk.psi_partial_stall_ms", t->partialstall);
    property_override("ro.lmk.psi_complete_stall_ms", t->completestall);
    property_override("ro.lmk.thrashing_limit", t->thrashlim);
    property_override("ro.lmk.thrashing_limit_decay", t->thrashlimdec);
    property_override("ro.lmk.swap_free_low_percentage", t->swapfreelow);
}

// heat is the NFC variant of fire, both are sold as Xiaomi Redmi 12 4G
void set_marketname()
{
    string device = GetProperty("ro.product.vendor.device", "");
    if (device.empty())
        device = GetProperty("ro.product.odm.device", "");
    if (device.empty())
        device = GetProperty("ro.product.device", "");

    const string name = device == "heat" ? "Xiaomi Redmi 12 4G NFC" : "Xiaomi Redmi 12 4G";

    property_override("ro.product.marketname", name);
    property_override("ro.product.product.marketname", name);
    // Name used by the Bluetooth stack until the user renames the device
    property_override("bluetooth.device.default_name", name);
}

void vendor_load_properties()
{
    set_marketname();
    configure_dalvik_and_lmkd();
}

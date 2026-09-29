/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdlib>
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

void configure_dalvik_and_lmkd()
{
    string heapstartsize, heapgrowthlimit, heapsize, heapminfree,
           heapmaxfree, heaptargetutilization;

    string partialstall, completestall, thrashlim, thrashlimdec,
           swapfreelow, upressure;

    struct sysinfo sys;
    sysinfo(&sys);

    // 8GB RAM tier (~7.4-7.6 GB reported)
    if (sys.totalram > 6144ull * 1024 * 1024) {
        heapstartsize = "16m";
        heapgrowthlimit = "256m";
        heapsize = "512m";
        heaptargetutilization = "0.5";
        heapminfree = "8m";
        heapmaxfree = "32m";

        partialstall = "70";
        completestall = "140";
        thrashlim = "90";
        thrashlimdec = "15";
        swapfreelow = "20";
        upressure = "50";
    }
    // 4GB RAM tier (~3.6-3.7 GB reported) - tuned for eMMC 5.1 latency
    else {
        heapstartsize = "8m";
        heapgrowthlimit = "192m";
        heapsize = "512m";
        heaptargetutilization = "0.65";
        heapminfree = "4m";
        heapmaxfree = "16m";

        // Looser PSI thresholds to avoid I/O blocking on eMMC
        partialstall = "90";
        completestall = "260";
        thrashlim = "65";
        thrashlimdec = "25";
        swapfreelow = "15";
        upressure = "65";
    }

    property_override("dalvik.vm.heapstartsize", heapstartsize);
    property_override("dalvik.vm.heapgrowthlimit", heapgrowthlimit);
    property_override("dalvik.vm.heapsize", heapsize);
    property_override("dalvik.vm.heaptargetutilization", heaptargetutilization);
    property_override("dalvik.vm.heapminfree", heapminfree);
    property_override("dalvik.vm.heapmaxfree", heapmaxfree);

    property_override("ro.lmk.psi_partial_stall_ms", partialstall);
    property_override("ro.lmk.psi_complete_stall_ms", completestall);
    property_override("ro.lmk.thrashing_limit", thrashlim);
    property_override("ro.lmk.thrashing_limit_decay", thrashlimdec);
    property_override("ro.lmk.swap_free_low_percentage", swapfreelow);
    property_override("ro.lmk.upgrade_pressure", upressure);
}

void vendor_load_properties()
{
    configure_dalvik_and_lmkd();
}

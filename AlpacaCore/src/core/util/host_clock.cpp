// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/util/host_clock.h>
#include <dirent.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

namespace alpacacore::util {

namespace {

// The device the kernel set system time from at boot is the one whose hctosys
// attribute reads 1 (the kernel gates that on the read having succeeded).
// Nothing else counts: a present-but-unread RTC, a kernel without
// CONFIG_RTC_HCTOSYS, or a userspace `hwclock --hctosys` all read as no RTC.
// Distinguishes "no usable answer yet" -- no device, or one that did not
// answer this time -- from "answered, and its time is wrong". Only the second
// is permanent (a dead RTC free-runs from its own wrong value); a device can
// register late, and an I2C read can fail transiently, so the first is
// re-probed.
enum class Probe : std::uint8_t { NoDevice, Implausible, Ok };

Probe probe_boot_rtc() {
    const std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir("/sys/class/rtc"), &::closedir);
    if (dir == nullptr) {
        return Probe::NoDevice;
    }
    std::string device;
    while (const dirent* entry = ::readdir(dir.get())) {
        const std::string name = entry->d_name;
        if (name.rfind("rtc", 0) != 0) {
            continue;
        }
        std::ifstream hctosys("/sys/class/rtc/" + name + "/hctosys");
        int used = 0;
        if (hctosys.is_open() && (hctosys >> used) && used == 1) {
            device = "/sys/class/rtc/" + name;
            break;
        }
    }
    if (device.empty()) {
        return Probe::NoDevice;
    }
    std::ifstream since_epoch(device + "/since_epoch");
    long long epoch = 0;
    if (!since_epoch.is_open() || !(since_epoch >> epoch)) {
        // A read that FAILS is not a value that is WRONG: an I2C RTC can
        // return -EIO transiently. Treat it as "not answered yet" and re-probe,
        // rather than pinning the host to "none" for the life of the process on
        // one unlucky read at startup.
        return Probe::NoDevice;
    }
    return epoch > HostClock::kMinPlausibleEpoch ? Probe::Ok : Probe::Implausible;
}

}  // namespace

bool HostClock::host_booted_from_rtc() {
    static std::mutex mutex;
    static bool settled = false;  // Ok or Implausible: neither can change after boot
    static bool result = false;
    static bool probed = false;  // not a time_point sentinel: CLOCK_MONOTONIC's zero is boot
    static std::chrono::steady_clock::time_point last_probe{};
    std::lock_guard<std::mutex> lock(mutex);
    if (settled) {
        return result;
    }
    // Only "no device" is re-probed, and not on every /management/v1/description poll.
    const auto now = std::chrono::steady_clock::now();
    if (probed && now - last_probe < kRtcProbeRateLimit) {
        return result;  // not a literal false: #307 may add a path that unsettles a true result
    }
    probed = true;
    last_probe = now;
    const Probe probe = probe_boot_rtc();
    if (probe != Probe::NoDevice) {
        settled = true;
        result = probe == Probe::Ok;
    }
    return result;
}

}  // namespace alpacacore::util

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
#include <alpacacore/util/logging.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
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

// The rtcN name of the device with hctosys = 1, or "" when there is none.
std::string boot_rtc_name() {
    const std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir("/sys/class/rtc"), &::closedir);
    if (dir == nullptr) {
        return {};
    }
    while (const dirent* entry = ::readdir(dir.get())) {
        const std::string name = entry->d_name;
        if (name.rfind("rtc", 0) != 0) {
            continue;
        }
        std::ifstream hctosys("/sys/class/rtc/" + name + "/hctosys");
        int used = 0;
        if (hctosys.is_open() && (hctosys >> used) && used == 1) {
            return name;
        }
    }
    return {};
}

Probe probe_boot_rtc() {
    const std::string name = boot_rtc_name();
    if (name.empty()) {
        return Probe::NoDevice;
    }
    const std::string device = "/sys/class/rtc/" + name;
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

std::mutex g_probe_mutex;
bool g_settled = false;  // Ok or Implausible: neither can change after boot, except by our own RTC write
bool g_result = false;
bool g_probed = false;  // not a time_point sentinel: CLOCK_MONOTONIC's zero is boot
std::chrono::steady_clock::time_point g_last_probe{};

}  // namespace

void HostClock::invalidate_rtc_probe() {
    const std::lock_guard<std::mutex> lock(g_probe_mutex);
    g_settled = false;
    g_probed = false;
}

HostClock::RtcWrite HostClock::kernel_write_rtc(std::string& device, std::string& error) {
    try {
        const std::string name = boot_rtc_name();
        if (name.empty()) {
            return RtcWrite::NoDevice;
        }
        device = "/dev/" + name;
        const int fd = ::open(device.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            error = std::string("open failed: ") + std::strerror(errno) + " (errno " + std::to_string(errno) + ")";
            return RtcWrite::Failed;
        }
        const std::time_t now = std::time(nullptr);
        struct tm utc {};
        gmtime_r(&now, &utc);
        struct rtc_time rt {};
        rt.tm_sec = utc.tm_sec;
        rt.tm_min = utc.tm_min;
        rt.tm_hour = utc.tm_hour;
        rt.tm_mday = utc.tm_mday;
        rt.tm_mon = utc.tm_mon;
        rt.tm_year = utc.tm_year;
        const int rc = ::ioctl(fd, RTC_SET_TIME, &rt);
        const int saved = errno;
        ::close(fd);
        if (rc != 0) {
            error =
                std::string("RTC_SET_TIME failed: ") + std::strerror(saved) + " (errno " + std::to_string(saved) + ")";
            return RtcWrite::Failed;
        }
        return RtcWrite::Written;
    } catch (const std::exception& e) {
        error = e.what();
        return RtcWrite::Failed;
    }
}

void HostClock::write_rtc_after_step(const Hooks& hooks, std::optional<std::chrono::milliseconds> delta) {
    std::string device;
    std::string error;
    RtcWrite result = RtcWrite::Failed;
    try {
        result = hooks.write_rtc ? hooks.write_rtc(device, error) : RtcWrite::NoDevice;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "unknown error";
    }
    if (result == RtcWrite::NoDevice) {
        return;
    }
    if (result == RtcWrite::Failed) {
        bool first = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            first = !rtc_write_warned_;
            rtc_write_warned_ = true;
        }
        if (first) {
            ALPACA_LOG_WARN("HostClock", "Could not write the corrected time to the hardware clock " + device + ": " +
                                             error +
                                             ". The system clock is correct until the next power cycle; grant "
                                             "the alpacabridge user write access to the RTC device to keep it.");
        }
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        rtc_write_warned_ = false;
    }
    ALPACA_LOG_INFO("HostClock", "Wrote the corrected system time to the hardware clock " + device +
                                     (delta ? " (step " + std::to_string(delta->count()) + " ms)" : std::string()));
    invalidate_rtc_probe();
    refresh_rtc();
}

bool HostClock::host_booted_from_rtc() {
    auto& mutex = g_probe_mutex;
    auto& settled = g_settled;
    auto& result = g_result;
    auto& probed = g_probed;
    auto& last_probe = g_last_probe;
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

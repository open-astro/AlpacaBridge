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

#pragma once

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace alpacacore::vendor::skywatcher {

// Driver for Sky-Watcher mounts spoken to directly at the motor controller
// level (Wave 100i/150i over USB serial or built-in Wi-Fi UDP). Equatorial
// mode only. Unlike the SynScan hand-controller driver, all pointing math
// (RA/Dec <-> axis counts, LST, pier side) lives in this driver; the mount
// stores no site or time information, so the site must be provided via
// configuration or the SiteLatitude/SiteLongitude setters.

namespace detail {
// True when the host clock moved underneath a client-set UTCDate offset: the
// system clock advanced by a different amount than the steady clock since the
// offset was captured (Sync Time, NTP taking over, a manual `date`). The
// offset described the old host clock, so it is dropped rather than applied
// on top of the corrected one. Pure, so the rule is unit-testable without
// stepping the test host's clock.
// open-astro#395: the driver samples the host's clock discipline through
// this probe instead of calling HostClock::kernel_is_synchronized() directly,
// so a test can drive both branches of the #301 rule on any build host. The
// default is the real adjtimex read; a test installs a lambda and restores
// the default afterwards. Process-wide, like the protocol wrapper singleton.
void set_host_synchronized_probe(std::function<bool()> probe);
bool host_synchronized_probe();
// open-astro#405: how often the pointing path re-samples discipline while a
// client offset is armed on a host that was undisciplined at the write
// (default 30 s; a test shortens it). Zero disables the re-sample.
void set_host_discipline_resample_interval(std::chrono::milliseconds interval);
std::chrono::milliseconds host_discipline_resample_interval();

bool host_clock_stepped(std::chrono::system_clock::duration system_elapsed,
                        std::chrono::steady_clock::duration steady_elapsed,
                        std::chrono::milliseconds tolerance = std::chrono::milliseconds(1000));

// Whether the POINTING math (LST, SiderealTime, DestinationSideOfPier, every
// goto) applies the client's UTCDate offset (open-astro#301).
//
// The ASCOM UTCDate readback always honours a client's write; this rule is
// only about the clock the mount is aimed by. A client's offset is applied
// when the host clock has nothing better to offer, and ignored when the host
// is NTP/PTP-disciplined, whether at the moment of the write or found so by a
// later re-sample (#405) -- there, a tablet with
// a 30-minute error would otherwise skew every goto by 7.5 degrees of RA on a
// rig whose own time is good. The router already refuses to step a
// disciplined clock and already warns when a client disagrees by more than
// 2 s; this makes the driver agree with that decision.
//
// `host_was_synchronized` is sampled when the client writes UTCDate and,
// while an offset is armed on a host that was undisciplined then, re-sampled
// at most once per interval on the pointing path (open-astro#405; one
// adjtimex read, no device I/O). Pure, so the rule is unit-testable without
// an NTP daemon.
//
// `offset_survives` folds in both "an offset was written" and "the host clock
// has not been stepped since", because the caller's
// client_offset_survives_locked() already answers exactly that and the two
// can never disagree at the call site.
bool pointing_uses_client_offset(bool offset_survives, bool host_was_synchronized);
}  // namespace detail

std::unique_ptr<TelescopeDriver> create_skywatcher_telescope(int device_number, const ConnectionInfo& connection_info,
                                                             std::optional<double> site_latitude_deg = std::nullopt,
                                                             std::optional<double> site_longitude_deg = std::nullopt,
                                                             std::optional<double> site_elevation_m = std::nullopt);

// Auto-detect: scan serial ports first, then Wi-Fi discovery (UDP 11880).
std::unique_ptr<TelescopeDriver> create_skywatcher_telescope_auto(
    int device_number, int mount_index = 0, std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt, std::optional<double> site_elevation_m = std::nullopt);

}  // namespace alpacacore::vendor::skywatcher

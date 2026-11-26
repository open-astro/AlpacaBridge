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

#include <alpacacore/async_connectable.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/device_name.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/findhome_sensor_search.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/host_clock_write_gate.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/lock_gate.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_guards.h>
#include <alpacacore/util/motion_owner.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/telescope_command_gate.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::vendor::onstep {

namespace {

// OnStep accepts a true arbitrary MoveAxis rate via :RA[n.n]#/:RE[n.n]#
// (real degrees/second, confirmed against Command.ino/Guide.ino) — this is
// a continuous range, not a small set of discrete presets. See
// OnStepProtocolWrapper::move_axis_start().
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
// Decision 0010, OnStep adoption (#1783, #2495). util::PolledState runs one poll body that samples the mount every
// kStatusPollPeriod, start to start on
// the task clock (ticks missed behind a slow link are skipped, never made up): :GU# and :GRa#/:GDe# on every tick,
// :GA#/:GZ# on every kSlowPollEvery-th, so Altitude and Azimuth are never older than the 2 s cache they replace. A
// full tick is five exchanges, about 0.13 s at 9600 baud on the HM-17 (#1242), so the line stays free for commands.
// kSnapshotMaxAge is the FRESHNESS bound: a frame older than it reads `stale` and is still served. Refusal is the
// link-loss policy of decision 0009 (the fault latch and util::kLinkStalenessBound), never staleness.
constexpr auto kStatusPollPeriod = std::chrono::milliseconds(1000);
constexpr int kSlowPollEvery = 2;
constexpr auto kSnapshotMaxAge = std::chrono::milliseconds(3000);
static_assert(kSnapshotMaxAge >= 2 * kStatusPollPeriod, "two poll periods, or a healthy link reads stale");
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");
// A pulse guide is measured once it has ended: the poll takes a tick this long after the pulse's end time, and
// IsPulseGuiding reads true until a frame sampled from then on is published (or kPulseMeasureBound after the end, on a
// link that does not answer), so a position read right after IsPulseGuiding goes false is not one taken mid-pulse.
constexpr auto kPulseGuideSettle = std::chrono::milliseconds(50);
constexpr auto kPulseMeasureBound = 2 * kStatusPollPeriod;
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
// The pier-side settings (#2179) are read at connect and again every kPierSettingsEvery-th regular tick, so a
// preference changed from a handset is followed (a pulse-end tick never lands on it).
constexpr int kPierSettingsEvery = 10;
// Decision 0022: Slewing reads true for this long after a GOTO goes out (slew_hold_), so a getter reads true before
// any :GU# is sampled. Landing never reads the window: it is a :GU# that shows no GOTO, and it clears the window.
// A stale reading cannot end the slew: a reading and the :MS# share the operation lock, and the dispatch
// write-through shows the GOTO until a later reading replaces it. Both firmwares set the GOTO state before :MS#
// replies, so the first :GU# shows it (source and rig evidence: onstep.instructions.md, "A slew while a GOTO runs
// is refused", #1887).
constexpr auto kSlewForceGrace = std::chrono::seconds(2);
// open-astro#1378: how far inside the horizon and overhead limits a MoveAxis
// re-send must stay.
constexpr double kAltitudeLimitMarginDeg = 1.0;
// open-astro#1378: how often a running MoveAxis is re-sent so OnStepX's guide
// time limit (10 s by default) does not end it. Tests advance a FakeTaskClock.
constexpr auto kMoveAxisResendInterval = std::chrono::seconds(7);
// #2406 (motion fails closed, #2396): a GOTO the mount still shows this long after its :MS# is stopped with
// AbortSlew's stops, and Slewing raises the failure. The status poll checks it for an async slew, and the
// synchronous slew's wait uses the same bound.
constexpr auto kGotoTimeout = std::chrono::seconds(120);
// #2406 (#2396 AB-4): while :GU# still shows motion after an AbortSlew, the status poll sends the stops again
// every kAbortStopRound, kAbortStopRounds rounds in all (the AbortSlew included); motion after the last one makes
// Slewing raise.
constexpr auto kAbortStopRound = std::chrono::seconds(2);
constexpr int kAbortStopRounds = 3;
// open-astro#1430: how long Park and FindHome wait for the motion they stopped
// to end, and how long they retry a park or home OnStepX refuses as "in motion"
// (an axis brakes for about 0.4 s at 0.5 deg/s on the HM-17; a home that just
// ended keeps its guide state a moment longer).
constexpr auto kStopSettleTimeout = std::chrono::seconds(6);
constexpr auto kStopSettlePoll = std::chrono::milliseconds(100);
constexpr auto kInMotionRetryWindow = std::chrono::seconds(3);
constexpr auto kInMotionRetryPoll = std::chrono::milliseconds(200);
// OnStepX :GE# codes (docs/COMMAND_REFERENCE.md, "Command Error Codes").
constexpr int kCeNone = 0;
// The longest PulseGuide the :Mg[nsew]#### command can carry (#1478).
constexpr int kMaxPulseGuideMs = 9999;
// Rate offsets (OnStepX :SXTR/:SXTD/:GXTR/:GXTD, Mount.command.cpp af1d26bf) are in arcseconds per sidereal second.
// ASCOM RightAscensionRate is seconds of RA per sidereal second (x 15) and DeclinationRate arcseconds per SI second
// (x kSiSecondsPerSiderealSecond). The firmware clamps silently at +/-1800, so a larger request is refused.
constexpr double kRaArcsecPerRaSecond = 15.0;
constexpr double kSiSecondsPerSiderealSecond = 0.9972695663;
constexpr double kRateOffsetLimitArcsecPerSiderealSecond = 1800.0;

// OnStep's limits, orders and argument texts for the telescope command gate (#2258). PulseGuide checks the fault
// before the parked state; MoveAxis and the slews check it after. The :Mg[nsew]#### duration field is 4 digits, so a
// longer pulse is refused instead of pulsing for less than asked (#1478). MoveAxis moves axes 0 and 1; a rate at or
// under 1e-6 is a stop, and a stop is sent on a faulted link (the driver checks the fault once it is sent, #1414).
constexpr util::TelescopeCommandLimits kCommandLimits = [] {
    util::TelescopeCommandLimits limits;
    limits.pulse_guide_max_ms = kMaxPulseGuideMs;
    static_assert(kMaxPulseGuideMs == 9999, "the PulseGuide duration text names the maximum");
    limits.texts.pulse_duration = "PulseGuide duration must be 0 to 9999 ms";
    limits.texts.move_axis_axis = "MoveAxis axis must be 0 or 1";
    limits.pulse_guide_order = util::ParkedFaultOrder::FaultFirst;
    limits.move_axis_axes = 2;
    limits.move_axis_max_rate = kMaxMoveAxisRateDegPerSec;
    limits.move_axis_stop_epsilon = 1e-6;
    limits.stop_checks_fault = false;
    limits.sync_order = util::ParkedFaultOrder::FaultFirst;
    limits.tracking_rate_max = 3;
    limits.texts.tracking_rate = "TrackingRate must be 0 (sidereal), 1 (lunar), 2 (solar) or 3 (King)";
    limits.rate_offset_ra_scale = kRaArcsecPerRaSecond;
    limits.rate_offset_dec_scale = kSiSecondsPerSiderealSecond;
    limits.rate_offset_max = kRateOffsetLimitArcsecPerSiderealSecond;
    static_assert(kRateOffsetLimitArcsecPerSiderealSecond == 1800.0, "the rate-offset texts name the limit");
    limits.texts.right_ascension_rate_not_finite = limits.texts.right_ascension_rate_range =
        "RightAscensionRate must be within +/-1800 arcseconds per sidereal second at the mount (the firmware clamps "
        "beyond it)";
    limits.texts.declination_rate_not_finite = limits.texts.declination_rate_range =
        "DeclinationRate must be within +/-1800 arcseconds per sidereal second at the mount (the firmware clamps "
        "beyond it)";
    return limits;
}();
constexpr int kCeSlewInMotion = 22;
constexpr int kCeTrue = 25;
constexpr int kCeNotParked = 11;
constexpr int kCeParked = 9;       // "already parked" (OnStep 4.x answers a park of a parked mount with it)
constexpr int kCeSlewInPark = 18;  // "mount parked" (OnStep 4.x answers a home of a parked mount with it)

std::string describe_onstepx_error(int code) {
    switch (code) {
        case 8:
            return "not parked or at home";
        case 9:
            return "already parked";
        case 10:
            return "park failed";
        case 11:
            return "not parked";
        case 12:
            return "no park position set";
        case 13:
            return "goto failed";
        case 15:
            return "target below the horizon limit";
        case 16:
            return "target above the overhead limit";
        case 17:
            return "controller in standby";
        case 18:
            return "mount parked";
        case 19:
            return "goto already active";
        case 20:
            return "outside configured limits";
        case 21:
            return "hardware fault";
        case 22:
            return "mount still in motion";
        case 23:
            return "slew error";
        default:
            return "error code " + std::to_string(code);
    }
}

// open-astro#1414 (decision 0009): consecutive failed poll probes that latch a link fault in util::PolledState, as on
// SynScan and Celestron; control-path reads do not count (#2495). Each failed probe costs one response timeout, so a
// silent mount latches in about three poll periods plus three timeouts.
constexpr int kLinkFaultThreshold = 3;

// Test seam: the tick number poll_status_tick_for_testing() runs on its own thread through PolledState's probe, which
// takes no argument. Empty on every poll thread.
std::optional<int>& forced_poll_tick() {
    thread_local std::optional<int> tick;
    return tick;
}
// OnStepX numbers its releases from 10 (the HM-17 answers :GVN# with "10.23a"); legacy OnStep is 4.x and older. Only
// OnStepX knows :GXEE#. is_onstepx_version() is in the wrapper, which the focuser shares (#2223).

// What a telescope with no link exchanges through: never connected, so every exchange throws NotConnected ("Not
// connected to mount"), link_alive() is false and no loss is recorded, as the process-wide wrapper answered while
// disconnected before #2223. Never connected, so never closed; never destroyed (exit order).
OnStepProtocolWrapper& no_link() {
    static auto* none = new OnStepProtocolWrapper;  // NOLINT(cppcoreguidelines-owning-memory): never destroyed
    return *none;
}

// Whether the connect asks :GVC# (the product description, decision 0024). A firmware without it sends nothing back
// (an unknown :GV command sets CE_CMD_UNKNOWN with numericReply=false, no "0"), so the read would wait out the whole
// response timeout. OnStepX history: 1b6a020f (2024-05-22) is 10.23a with no 'C' case in the :GV branch; d9de0b8d
// (2024-06-10 06:41:53) adds :GVC# and is still 10.23a, as is 675cd048 (06:44:33); 6cf2e575 (06:44:38, "Upped patch
// level") makes it 10.23b, and every later build answers. 10.24a starts at d09b11af (2024-09-18). So only two
// unreleased 10.23a commits from a three-minute window answer, and the rule is: ask for a major above 10, for 10.24
// and later, and for 10.23 with a patch letter after 'a'; do not ask for 10.23a, 10.23 with no letter, anything older,
// or a version that does not parse.
bool firmware_answers_product_description(const std::string& version) {
    std::size_t pos = 0;
    const auto number = [&](int& out) {
        std::size_t digits = 0;
        while (pos + digits < version.size() && std::isdigit(static_cast<unsigned char>(version[pos + digits]))) {
            ++digits;
        }
        if (digits == 0 || digits > 4) return false;
        out = std::stoi(version.substr(pos, digits));
        pos += digits;
        return true;
    };
    int major = 0;
    int minor = 0;
    if (!number(major) || major < 10) return false;
    if (major > 10) return true;
    if (pos >= version.size() || version[pos] != '.') return false;
    ++pos;
    if (!number(minor)) return false;
    if (minor >= 24) return true;
    if (minor != 23 || pos >= version.size()) return false;
    const char patch = version[pos];
    return patch >= 'b' && patch <= 'z';
}

// GuideRate until the mount has answered :GX90# (and for firmware that never does): half the sidereal rate, OnStep's
// default pulse-guide rate. CanSetGuideRates is false, so the driver never writes a rate; once :GX90# answers, the
// mount's own pulse-guide rate (a multiple of sidereal) replaces this on both axes.
constexpr double kInformationalGuideRateDegPerSec = (15.0411 / 3600.0) * 0.5;
// The sidereal rate in arcseconds per SI second, the unit of :GX90#'s multiple.
constexpr double kSiderealArcsecPerSecond = 15.0410686;

// #2241: while a rate offset runs, RightAscension and Declination (and so DeviceState) serve the commanded model, the
// anchor plus the offsets times the sidereal seconds since it, instead of the frame, which is up to a poll period old
// and moves in the mount's whole steps. Each poll compares its :GRa#/:GDe# with the model at the time of that read and
// re-anchors on the measurement when either axis is more than kOffsetModelGuardArcsec off (four 0.5 arcsec steps of
// the TeSeek HM-17; a mount with larger steps needs at least four of its own), so a stalled axis shows within one poll
// at 40 arcsec/s and within about 40 s at 0.05 arcsec/s, and after kOffsetModelHold without a re-anchor anyway. RA is
// compared in RA-axis arcseconds (hours x kRaAxisArcsecPerHour), the unit the offset and the mount's steps are in.
constexpr double kOffsetModelGuardArcsec = 2.0;
constexpr auto kOffsetModelHold = std::chrono::minutes(30);
constexpr double kRaAxisArcsecPerHour = 15.0 * 3600.0;

// The model's anchor: a position and the task-clock time it holds for.
struct OffsetAnchor {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
    util::TaskClock::clock::time_point at{};
};

// The anchor plus the offsets (arcseconds per sidereal second, :SXTR/:SXTD; RA in RA-axis arcseconds) times the
// sidereal seconds from the anchor to `at`: the task clock runs in SI seconds, kSiSecondsPerSiderealSecond each. RA
// wraps into [0, 24) hours and Dec is clamped to +/-90 degrees.
Position offset_model_position(const OffsetAnchor& anchor, double ra_offset_arcsec, double dec_offset_arcsec,
                               util::TaskClock::clock::time_point at) {
    const double si_seconds = std::max(0.0, std::chrono::duration<double>(at - anchor.at).count());
    const double sidereal_seconds = si_seconds / kSiSecondsPerSiderealSecond;
    return Position{util::wrap_hours(anchor.ra_hours + ra_offset_arcsec * sidereal_seconds / kRaAxisArcsecPerHour),
                    std::clamp(anchor.dec_degrees + dec_offset_arcsec * sidereal_seconds / 3600.0, -90.0, 90.0)};
}

// The guard: a measured position more than kOffsetModelGuardArcsec from the model on either axis (RA the short way
// round, in RA-axis arcseconds).
bool offset_model_disagrees(const Position& model, double ra_hours, double dec_degrees) {
    const double ra_arcsec = std::fabs(util::shortest_ra_delta_hours(ra_hours, model.ra_hours)) * kRaAxisArcsecPerHour;
    const double dec_arcsec = std::fabs(dec_degrees - model.dec_degrees) * 3600.0;
    return ra_arcsec > kOffsetModelGuardArcsec || dec_arcsec > kOffsetModelGuardArcsec;
}

// :GT# in Hz (Mount.command.cpp af1d26bf: 60.164 sidereal, 57.900 lunar, 60.000 solar, 60.136 King) as DriveRates
// 0-3, within 0.01 Hz; nullopt for a rate that is none of them (and for 0, not tracking).
std::optional<int> tracking_rate_for_hz(double hz) {
    constexpr double kRatesHz[] = {60.164, 57.900, 60.000, 60.136};
    for (int rate = 0; rate < 4; ++rate) {
        if (std::fabs(hz - kRatesHz[rate]) <= 0.01) {
            return rate;
        }
    }
    return std::nullopt;
}

// Standard spherical-trig horizontal-to-equatorial transform (azimuth
// measured from North through East). Used to implement SlewToAltAz/
// SlewToAltAzAsync via an ordinary equatorial slew — OnStep/LX200 has no
// native Alt/Az goto command — mirroring the approach other equatorial-only
// drivers in this project use for the same capability.
void alt_az_to_ra_dec(double alt_deg, double az_deg, double lat_deg, double lst_hours, double& ra_hours_out,
                      double& dec_deg_out) {
    const double lat_rad = lat_deg * std::numbers::pi / 180.0;
    const double alt_rad = alt_deg * std::numbers::pi / 180.0;
    const double az_rad = az_deg * std::numbers::pi / 180.0;

    double sin_dec = std::sin(alt_rad) * std::sin(lat_rad) + std::cos(alt_rad) * std::cos(lat_rad) * std::cos(az_rad);
    sin_dec = std::clamp(sin_dec, -1.0, 1.0);
    const double dec_rad = std::asin(sin_dec);

    double cos_ha = (std::sin(alt_rad) - std::sin(lat_rad) * sin_dec) / (std::cos(lat_rad) * std::cos(dec_rad));
    cos_ha = std::clamp(cos_ha, -1.0, 1.0);
    double ha_hours = std::acos(cos_ha) * 180.0 / std::numbers::pi / 15.0;
    if (std::sin(az_rad) > 0.0) {
        ha_hours = 24.0 - ha_hours;
    }

    ra_hours_out = util::wrap_hours(lst_hours - ha_hours);
    dec_deg_out = dec_rad * 180.0 / std::numbers::pi;
}

// One polled frame (decision 0010): what one tick measured, nothing derived. Small and copyable: the snapshot copies
// it under its mutex. `sampled_at` is the task-clock time the tick's first exchange went out; `sample_seq` numbers the
// samples in the order they were taken (each under the operation lock), so two taken at the same task-clock time
// still order.
struct PolledFrame {
    MountStatus status{};
    bool status_known = false;
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
    bool equatorial_known = false;
    // The task-clock time the :GRa#/:GDe# pair stands for (the middle of its exchange), the time the rate-offset
    // model is checked at (#2241).
    util::TaskClock::clock::time_point position_at{};
    double alt_degrees = 0.0;
    double az_degrees = 0.0;
    bool altaz_known = false;
    util::TaskClock::clock::time_point sampled_at{};
    std::uint64_t sample_seq = 0;
};

// OnStepX's pier-side settings (#2179), read from the mount: the preferred side letter (E, W, B or A), whether the
// meridian flips are on, the meridian limits in minutes past the meridian and the RA-axis limits in degrees.
struct PierSettings {
    char preference = 'B';
    bool flips_enabled = true;
    int east_limit_minutes = 0;
    int west_limit_minutes = 0;
    int axis1_min_degrees = 0;
    int axis1_max_degrees = 0;

    bool operator==(const PierSettings&) const = default;

    util::GemPierPreference model_preference() const {
        switch (preference) {
            case 'E':
                return util::GemPierPreference::East;
            case 'W':
                return util::GemPierPreference::West;
            case 'A':
                return util::GemPierPreference::Auto;
            default:
                return util::GemPierPreference::Best;
        }
    }

    util::GemPierLimits model_limits() const {
        return util::GemPierLimits{east_limit_minutes / 60.0, west_limit_minutes / 60.0,
                                   static_cast<double>(axis1_min_degrees), static_cast<double>(axis1_max_degrees)};
    }

    std::string describe() const {
        return std::string("preferred side ") + preference + (flips_enabled ? ", flips on" : ", flips off") +
               ", meridian limits " + std::to_string(east_limit_minutes) + "/" + std::to_string(west_limit_minutes) +
               " min east/west, RA axis " + std::to_string(axis1_min_degrees) + ".." +
               std::to_string(axis1_max_degrees) + " deg";
    }
};

bool same_status(const MountStatus& a, const MountStatus& b) {
    return a.raw_status == b.raw_status && a.is_tracking == b.is_tracking && a.is_slewing == b.is_slewing &&
           a.is_parked == b.is_parked && a.is_at_home == b.is_at_home && a.is_guide_active == b.is_guide_active &&
           a.is_homing == b.is_homing && a.is_parking == b.is_parking &&
           a.is_pulse_guide_active == b.is_pulse_guide_active && a.side_of_pier == b.side_of_pier;
}

// The motion a kept error or a watch belongs to (#2296): AtPark raises only a park's, AtHome only a home's.
enum class MotionKind : std::uint8_t { Slew, Park, Home };

// Why the mount stopped, for the :GU# general error codes that stop it (OnStepX Limits::errorCode() and stop()/
// stopAxis1()/stopAxis2(), which abort the GOTO; OnStep 4.x OnStep.ino stopSlewingAndTracking()); nullptr for the
// others: 0 none, 8 sync and 9 park (obsolete), 10 goto sync (4.x ends a slew that does not settle and syncs it),
// 11 unspecified, 13-15 weather, site and NV init.
const char* limit_stop_reason(int code) {
    switch (code) {
        case 1:
            return "on a motor or driver fault";
        case 2:
            return "at the horizon limit";
        case 3:
            return "at a limit switch";
        case 4:
            return "at the Dec axis limit";
        case 5:
            return "at the azimuth axis limit";
        case 6:
            return "at the RA axis limit (under the pole)";
        case 7:
            return "at the meridian limit";
        case 12:
            return "at the overhead limit";
        default:
            return nullptr;
    }
}

// The great-circle angle between two RA/Dec positions, in degrees (#2296).
double separation_degrees(double ra1_hours, double dec1_degrees, double ra2_hours, double dec2_degrees) {
    constexpr double kRad = std::numbers::pi / 180.0;
    const double d1 = dec1_degrees * kRad;
    const double d2 = dec2_degrees * kRad;
    const double c =
        (std::sin(d1) * std::sin(d2)) + (std::cos(d1) * std::cos(d2) * std::cos((ra1_hours - ra2_hours) * 15.0 * kRad));
    return std::acos(std::clamp(c, -1.0, 1.0)) / kRad;
}

// #2296: a GOTO whose end frame carries a stop code landed when the mount is this close to its target. The driver has
// no landing tolerance of its own (landing is a :GU# with no GOTO), so this is generous: 1 degree is 4 minutes of a
// stopped mount's RA drift, far beyond the arcseconds an OnStep GOTO lands within (other drivers here use 3 to 600
// arcseconds), so a landing is never read as a stop; a limit stop that ends inside it is read as a landing (logged).
// It is not tightened: a healthy GOTO can end farther from the target the driver sent than the arcseconds it lands
// within (a Tracking write during the GOTO changes whether the firmware holds the target's RA or its hour angle, and a
// tracking OnStepX GOTO follows the RA and Dec rate offsets on its final approach, Goto.cpp poll()), and a stop that
// ends inside it is still logged with the code (at WARN the first time, its repeats at INFO: log_motion_stop_locked()).
constexpr double kStopLandedToleranceDegrees = 1.0;

// Sidereal seconds per SI second: a target held at a fixed hour angle moves this fast in RA (#2296).
constexpr double kSiderealPerSolar = 1.00273790935;

// #2296: a park or home the driver started that has not ended within its bound fails. The bound is
// kParkHomeTravelSeconds divided by the mount's base slew rate in deg/s (:GX93# us/step and :GXE4# steps per degree,
// read at connect), clamped to [kParkHomeBoundFloor, kParkHomeBoundCap]. Both firmwares move a park or home at no less
// than a quarter of the base: the slowest preset halves it (OnStepX Goto.cpp, OnStep 4.x Initialize.ino and
// Command.ino clamp the current rate to half the base), and the 4.x switch search halves it again (Home.ino,
// guideRates[8]). A move is at most 180 degrees twice (a visit-home flip, or a search and the offset move), so 1440 s
// at a quarter of 1 deg/s of base, and half again for margin gives 2160 s; the quarter also covers an OnStepX
// AXIS2_SLEW_RATE_PERCENT down to 50 at the slowest preset, which no command reports. A mount whose rate does not read
// keeps the floor, which is never lowered. That is less than the 36 minutes this derivation gives a 1 deg/s base: 20
// minutes covers a 1 deg/s base at half rate with margin (1080 s), not its quarter-rate worst case. The time a
// GOTO waits at home ('w') is not counted.
constexpr auto kParkHomeBoundFloor = std::chrono::minutes(20);
constexpr double kParkHomeTravelSeconds = 2160.0;  // seconds at 1 deg/s of base slew rate
constexpr auto kParkHomeBoundCap = std::chrono::hours(24);

}  // namespace

class OnStepTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
    // Decision 0016 (#1449): getters never wait on device I/O. Two locks, in this order:
    //   transition_mutex_ -> io_mutex_ (the operation lock) -> mutex_ (the state lock, a leaf)
    // io_mutex_ keeps one command's exchanges and state writes together: every command that reaches the wire
    // holds it for the whole command and releases it wherever it released mutex_ (a sleep, a wait). Some
    // sequences must not be split: :GE# answers for the command right before it, and :Sr#/:Sd# set the target
    // the :MS# or :CM# after them uses. mutex_ is never held ACROSS an exchange: every call through Wire drops
    // it for the call and takes it back after. The protocol wrapper's own lock serialises the wire itself.
    //
    // Decision 0010 (#1783, #2495): the polled getters and DeviceState read polled_ (util::PolledState), whose poll
    // body calls probe() every second, so a getter takes mutex_ alone (StateLock::Mode::State) and never reaches the
    // wire. The probe takes the operation lock for its exchanges, like a command, and publishes the frame itself
    // under mutex_ with the operation lock released. The PolledState mutexes are leaves under mutex_.
    //
    // The gate itself (which locks this thread holds, StateLock's two modes, dropping mutex_ around an exchange)
    // is util::LockGate (lock_gate.h, #1729).

public:
    // The driver's state lock, with the operation lock above it in Mode::Io (util::StateLock). Mode::State
    // is for the getters, the setters that send nothing and the MoveAxis re-send's scheduling read: they take
    // no operation lock and never reach the wire.
    using StateLock = util::StateLock;

private:
    // Every exchange with the mount goes through this gate (the controller's shared link behind it, #2223). The
    // caller holds the operation lock; it gives up the state lock for the call, if it holds it, and takes it
    // back after (also when the call throws). A caller without the operation lock is a defect: it throws
    // DriverException instead of reaching the wire. With no link held the exchange meets no_link(), which throws
    // NotConnected.
    class Wire {
    public:
        explicit Wire(const OnStepTelescopeDriver& driver) : driver_(driver) {}

        template <typename F>
        decltype(auto) call(F&& f) const {
            const util::LockGate& gate = driver_.lock_gate_;
            if (!gate.holds_io()) {
                throw AlpacaException("OnStep: a mount exchange without the operation lock",
                                      AlpacaError::DriverException);
            }
            return gate.exchange([&]() -> decltype(auto) {
                const auto link = driver_.link_.load();  // held for the call: a Disconnect cannot close it under us
                return f(link ? *link : no_link());
            });
        }

        // No I/O, no wrapper lock held across a wait: called directly.
        bool link_alive() const { return driver_.link_alive(); }

        // A command and the :GE# that reads its code, back to back on the shared link (#2223): a sibling device's
        // exchange between them would replace the code. Taken with the state lock released.
        struct Sequence {
            std::shared_ptr<OnStepProtocolWrapper> link;  // first: outlives the lock below
            OnStepProtocolWrapper::Sequence lock;
        };
        Sequence hold_sequence() const {
            return call([this](auto&) {
                Sequence sequence{driver_.link_.load(), {}};
                if (sequence.link) {
                    sequence.lock = sequence.link->hold_sequence();
                }
                return sequence;
            });
        }

        // Lets go of the link (Disconnect, a relink, a failed connect), with the state lock released: the last holder
        // closes the port. Its loss stamps stay with this driver for the next connect (#521).
        void release_link() const {
            call([this](auto&) { driver_.release_link(); });
        }
        void declare_link_lost(const std::string& reason) const {
            call([&](auto& p) { p.declare_link_lost(reason); });
        }
        std::string get_product_name() const {
            return call([](auto& p) { return p.get_product_name(); });
        }
        std::string get_version_number() const {
            return call([](auto& p) { return p.get_version_number(); });
        }
        std::string get_product_description() const {
            return call([](auto& p) { return p.get_product_description(); });
        }
        std::optional<int> get_mount_coords_mode() const {
            return call([](auto& p) { return p.get_mount_coords_mode(); });
        }
        std::optional<char> get_preferred_pier_side() const {
            return call([](auto& p) { return p.get_preferred_pier_side(); });
        }
        std::optional<bool> get_pier_flips_enabled() const {
            return call([](auto& p) { return p.get_pier_flips_enabled(); });
        }
        std::optional<int> get_meridian_limit_minutes(bool east) const {
            return call([&](auto& p) { return p.get_meridian_limit_minutes(east); });
        }
        std::optional<int> get_axis1_limit_degrees(bool maximum) const {
            return call([&](auto& p) { return p.get_axis1_limit_degrees(maximum); });
        }
        std::optional<bool> get_tangent_arm() const {
            return call([](auto& p) { return p.get_tangent_arm(); });
        }
        std::optional<bool> get_sector_gear() const {
            return call([](auto& p) { return p.get_sector_gear(); });
        }
        std::optional<double> get_base_slew_degrees_per_second() const {
            return call([](auto& p) { return p.get_base_slew_degrees_per_second(); });
        }
        Position get_position() const {
            return call([](auto& p) { return p.get_position(); });
        }
        AltAz get_alt_az() const {
            return call([](auto& p) { return p.get_alt_az(); });
        }
        AltitudeLimits get_altitude_limits() const {
            return call([](auto& p) { return p.get_altitude_limits(); });
        }
        MountStatus get_status() const {
            return call([](auto& p) { return p.get_status(); });
        }
        void set_target_ra(double ra_hours) const {
            call([&](auto& p) { p.set_target_ra(ra_hours); });
        }
        void set_target_dec(double dec_degrees) const {
            call([&](auto& p) { p.set_target_dec(dec_degrees); });
        }
        std::string goto_current_position_on_side(char side) const {
            return call([&](auto& p) { return p.goto_current_position_on_side(side); });
        }
        std::string slew_to_target() const {
            return call([](auto& p) { return p.slew_to_target(); });
        }
        std::string sync_to_target() const {
            return call([](auto& p) { return p.sync_to_target(); });
        }
        void abort_slew() const {
            call([](auto& p) { p.abort_slew(); });
        }
        void start_tracking(bool use_enable_command) const {
            call([&](auto& p) { p.start_tracking(use_enable_command); });
        }
        bool set_tracking_checked(bool on, bool use_enable_command) const {
            return call([&](auto& p) { return p.set_tracking_checked(on, use_enable_command); });
        }
        void set_tracking_rate(int rate) const {
            call([&](auto& p) { p.set_tracking_rate(rate); });
        }
        std::optional<double> get_tracking_rate_hz() const {
            return call([](auto& p) { return p.get_tracking_rate_hz(); });
        }
        bool set_rate_offset_ra(double arcsec_per_sidereal_second) const {
            return call([&](auto& p) { return p.set_rate_offset_ra(arcsec_per_sidereal_second); });
        }
        bool set_rate_offset_dec(double arcsec_per_sidereal_second) const {
            return call([&](auto& p) { return p.set_rate_offset_dec(arcsec_per_sidereal_second); });
        }
        std::optional<double> get_rate_offset_ra() const {
            return call([](auto& p) { return p.get_rate_offset_ra(); });
        }
        std::optional<double> get_rate_offset_dec() const {
            return call([](auto& p) { return p.get_rate_offset_dec(); });
        }
        std::optional<double> get_pulse_guide_rate() const {
            return call([](auto& p) { return p.get_pulse_guide_rate(); });
        }
        bool park() const {
            return call([](auto& p) { return p.park(); });
        }
        bool set_park() const {
            return call([](auto& p) { return p.set_park(); });
        }
        bool unpark() const {
            return call([](auto& p) { return p.unpark(); });
        }
        void find_home() const {
            call([](auto& p) { p.find_home(); });
        }
        std::optional<int> get_last_command_error() const {
            return call([](auto& p) { return p.get_last_command_error(); });
        }
        void select_max_slew_rate() const {
            call([](auto& p) { p.select_max_slew_rate(); });
        }
        void move_axis_start(int direction, double rate_deg_per_sec) const {
            call([&](auto& p) { p.move_axis_start(direction, rate_deg_per_sec); });
        }
        void move_axis_stop(int direction) const {
            call([&](auto& p) { p.move_axis_stop(direction); });
        }
        void pulse_guide(int direction, int duration_ms) const {
            call([&](auto& p) { p.pulse_guide(direction, duration_ms); });
        }
        SiteInfo get_site_info() const {
            return call([](auto& p) { return p.get_site_info(); });
        }
        void set_latitude(double latitude_degrees) const {
            call([&](auto& p) { p.set_latitude(latitude_degrees); });
        }
        void set_longitude(double longitude_degrees) const {
            call([&](auto& p) { p.set_longitude(longitude_degrees); });
        }
        TimeInfo get_time() const {
            return call([](auto& p) { return p.get_time(); });
        }
        void set_time(const TimeInfo& info) const {
            call([&](auto& p) { p.set_time(info); });
        }
        std::string send_command(const std::string& command) const {
            return call([&](auto& p) { return p.send_command(command); });
        }
        void send_command_blind(const std::string& command) const {
            call([&](auto& p) { p.send_command_blind(command); });
        }

    private:
        const OnStepTelescopeDriver& driver_;
    };
    Wire wire() const { return Wire(*this); }

    // True when this thread holds the operation lock (a command, or the connect itself).
    bool holds_io_lock() const { return lock_gate_.holds_io(); }

    // A slew or sync to (ra, dec) sets both targets, except one a client wrote
    // while the command ran: the target setters take mutex_ alone (#1728
    // review), so a write can land during any exchange of the command, its
    // checks' :GU# or site reads included. Each entry point takes the snapshot
    // right after its StateLock, before any of them, so that write stands.
    struct TargetWrites {
        std::uint64_t ra;
        std::uint64_t dec;
    };
    TargetWrites target_writes_locked() const { return {target_ra_writes_, target_dec_writes_}; }
    void store_target_locked(double ra, double dec, TargetWrites before) {
        if (target_ra_writes_ == before.ra) {
            target_ra_hours_ = ra;
            target_ra_set_ = true;
        }
        if (target_dec_writes_ == before.dec) {
            target_dec_degrees_ = dec;
            target_dec_set_ = true;
        }
    }

public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    OnStepTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                          std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                          std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                          util::ConnectionResolver<ConnectionInfo> connection_resolver = {},
                          util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("OnStep"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          site_elevation_m_(site_elevation_m.value_or(std::numeric_limits<double>::quiet_NaN())),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)) {}

    ~OnStepTelescopeDriver() override {
        // MUST be first, before members the async connection task touches
        // are destroyed (AsyncConnectable base contract). The other workers,
        // the MoveAxis re-send (#1378) and the Park/FindHome tail (#1430),
        // are joined next without the driver locks (they take them to send).
        shutdown_connection();
        stop_status_poller();
        clock_resync_.cancel_all_and_join();  // takes both driver locks to write (#2419)
        move_axis_resend_.cancel_all_and_join();
        stop_move_axis_caps();
        park_home_.cancel_all_and_join();
        if (connected_) {
            try {
                // OnStepTelescopeDriver is the most-derived class (no further
                // override exists to miss), so the analyzer's "bypasses
                // virtual dispatch" warning doesn't apply here.
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall): most-derived class
            } catch (...) {            // NOLINT(bugprone-empty-catch): best-effort teardown
                // Best-effort teardown during destruction.
            }
        }
        // Last: a link a sibling device still holds must not keep a clock that may die with this driver (a test's
        // FakeTaskClock).
        release_link();
    }

    int get_device_number() const override { return device_number_; }

    // Decision 0024: OnStep is the brand, and the model is OnStepX's :GVC# product description, read at connect;
    // a firmware without :GVC# is "OnStep Telescope". Kept across a disconnect, like the Sky-Watcher model.
    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        return model_cache_.empty() ? "OnStep Telescope" : model_cache_;
    }

    DeviceType get_device_type() const override { return DeviceType::Telescope; }

    std::string fallback_unique_id() const override { return "OnStep_" + std::to_string(device_number_); }

    std::string get_description() const override { return "OnStep LX200-Protocol Telescope Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore OnStep Driver v0.1"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Product name + firmware version captured at connect; web-UI only.
    // The connect no longer holds mutex_ across its exchanges: it holds
    // transition_mutex_ and io_mutex_ for its whole multi-second run and
    // takes mutex_ only between exchanges (decision 0016), so mutex_ would
    // serve here now. firmware_mutex_ is kept anyway, held only for the copy:
    // it is the shared rule's guard (driver-lifecycle, "Device firmware / SDK
    // version"), the configureddevices listing then never queues behind the
    // state writes of a getter or a command, and the connect fills the cache
    // without firmware_mutex_ held across its :GVP#/:GVN# reads.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    // Lock-free on purpose (open-astro#910, decision 0016 rule 3): set_connected(true) holds
    // transition_mutex_ and io_mutex_ for the whole connect (port open, handshake, site and time
    // sync, the first frame, and an auto-detect scan when the port is not explicit). The
    // router checks Connecting before most Connected reads, but DeviceState
    // calls this with no such check, and a read that lands just before the
    // connect task shows as running gets past it; with mutex_ taken here those
    // once waited for the whole connect. link_alive() takes no lock a connect holds: the wrapper mutex_
    // only by try_lock, otherwise only short leaf mutexes (a stat(), a few fields).
    // It reads published_connected_, raised only at the end of a connect, so
    // Connected stays false while Connecting is true. Same rule as SynScan
    // (#130); the blocking-driver list lives in async_connectable.h.
    bool get_connected() const override {
        // A removed or re-enumerated adapter reads as not connected (#1241).
        return published_connected_.load() && link_alive();
    }

    // The held link is there and not lost, by the wrapper or by the poll's loss bound (no I/O; decision 0009).
    // Lock-free: the link pointer and the loss latch are atomic.
    bool link_alive() const {
        const auto link = link_.load();
        return link && link->link_alive() && !polled_.link_lost();
    }

    // PolledState's on_lost, on the poll thread with no driver lock held: a fault, or a frame no probe refreshed,
    // standing for util::kLinkStalenessBound. The wrapper records the loss and closes the port, as for a pulled adapter
    // (#1241), so the #521 relink stops time the outage; a sibling device on the link reads it too (#2223).
    void declare_poll_loss() const {
        const auto link = link_.load();
        if (!link) {
            return;
        }
        const std::string bound =
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) + " s";
        const std::string fault = polled_.fault();
        link->declare_link_lost(fault.empty() ? "no state frame for " + bound : "no reply for " + bound + ": " + fault);
    }

    // Why the held link was lost, or "" (no I/O).
    std::string link_lost_reason() const {
        const auto link = link_.load();
        return link ? link->link_lost_reason() : std::string();
    }

    // Drops this driver's hold on the link. Its loss stamps move into the driver first, so the next connect times
    // the #521 outage whichever link it then takes (a re-scan to another port included), as the process-wide
    // wrapper's stamps did before #2223; then its clock is handed back, since a sibling may hold the link on.
    // Under the operation lock, or in the destructor; never under mutex_ (the last holder closes the port).
    void release_link() const {
        std::shared_ptr<OnStepProtocolWrapper> link = link_.exchange(nullptr);
        if (!link) {
            return;
        }
        if (const auto lost_at = link->consume_link_lost_at()) {
            carried_lost_at_ = lost_at;
            carried_alive_at_ = link->consume_last_alive_before_loss();
        }
        link->release_task_clock(clock_);
        link.reset();
    }

    // NotConnected exactly when Connected reads false, without mutex_.
    void check_connected_lock_free() const {
        if (!get_connected()) {
            throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
        }
    }

    // The latched fault for the getters that read lock-free, so they never wait behind serial I/O under mutex_
    // (#1449): PolledState keeps it under a leaf mutex of its own. The poll's probes make a standing fault a lost link.
    void throw_if_faulted_lock_free() const {
        const std::string fault = polled_.fault();
        if (!fault.empty()) {
            throw AlpacaException("OnStep mount communications compromised: " + fault, AlpacaError::DriverException);
        }
    }

    // Why the serial link was lost, kept until the next Connect or Disconnect,
    // or else the latched fault of a silent mount (decision 0009). The wrapper's
    // reason and the poll's fault under their own narrow mutexes: never waits on mutex_.
    std::string get_link_fault() const override {
        (void)link_alive();  // notice an unplug the listing would otherwise miss
        const std::string reason = link_lost_reason();
        if (!reason.empty()) {
            return "Link to the mount lost (" + reason + "); reconnect";
        }
        // A latched fault: Connected still true, reads refused (#1414).
        const std::string fault = polled_.fault();
        return fault.empty() ? std::string{} : "Mount not answering (" + fault + ")";
    }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    void set_connected(bool connected) override {
        const Wire protocol = wire();
        // Decision 0016: the whole call runs under transition_mutex_, which no getter takes. Every exchange
        // below goes through Wire, with mutex_ free; io_mutex_ is held from the first state write to the end.
        // A getter meanwhile reads NotConnected (check_connected()) instead of waiting for the connect.
        // open-astro#2419: declared before the transition lock, so it runs after the lock is released: a host clock
        // change on_host_clock_changed() could not act on while this call held the lock (it never waits for it) is
        // acted on here, with no driver lock held. Checked while the lock was still held, a change that failed its
        // try_lock after the check would stay pending with nobody to start it. A change seen while disconnected is
        // dropped, so a disconnect's leaves nothing for the next connect.
        struct DrainPendingClockResync {
            OnStepTelescopeDriver& self;
            ~DrainPendingClockResync() { self.drain_pending_clock_resync(); }
        } drain_pending_clock_resync_on_exit{*this};
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Test seam: runs as this call returns, after everything below it, just before the transition lock is
        // released.
        struct BeforeTransitionUnlock {
            const OnStepTelescopeDriver& self;
            ~BeforeTransitionUnlock() { self.run_connected_before_transition_unlock_hook(); }
        } before_transition_unlock{*this};
        // The status poll and the pulse-end tick end before a disconnect or a relink closes the port, and are
        // joined with no driver lock held: their bodies take them (decision 0006 rule 4). A fresh connect joins
        // them too: a pulse-end body a PulseGuide started after a Disconnect's join would otherwise live on into
        // the new session. No poll body runs during a connect. The host clock re-sync is drained the same way.
        if (!connected || !connected_ || !protocol.link_alive()) {
            stop_status_poller();
            clock_resync_.cancel_all_and_join();  // every reaper holds transition_mutex_ (or is the destructor)
            stop_move_axis_caps();                // a body mid-stop takes the operation lock, so no driver lock is held
        }
        run_connect_before_state_lock_hook();  // test seam: transition_mutex_ alone held
        StateLock lock(lock_gate_);
        // #2296: the watch on a GOTO, park or home the driver started, kept through a relink that leaves the motion
        // running (stop_motion_after_link_loss_locked()), so its end is still checked. Its kept failure is not.
        std::optional<WatchedMotion> relink_watch;
        // A Connected=true against a lost link must reconnect, not return as
        // already connected (decision 0009 point 4, #1241).
        if (connected && connected_ && !protocol.link_alive()) {
            ALPACA_LOG_WARN("OnStep", "Connect requested on a lost link; reconnecting");
            if (polled_.link_lost()) {
                declare_poll_loss();  // a loss the poll latched and did not close yet: stamped for the #521 stops
            }
            published_connected_ = false;
            connected_ = false;  // before the close: a getter during it reads NotConnected (decision 0016)
            protocol.release_link();
            relink_watch = watched_motion_;
            reset_session_state_locked();
        }
        // Base gates BEFORE the idempotency check: a sync disconnect during
        // an in-flight connect looks idempotent (both sides see
        // disconnected) and would be silently dropped without the record; a
        // connect must honor a newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }

        if (connected) {
            // An auto-detected mount resolves its port here, not in the factory (#659). The scan and the
            // open go through Wire like every exchange: the state lock is released for them and taken back
            // before the outcome or its exception leaves. The connection members the scan writes are read
            // only under transition_mutex_, which this call holds; io_mutex_ stays held. The link is the
            // controller's shared one (#2223): a sibling device's open link is joined, not opened again.
            protocol.call([&](auto&) {
                util::connect_resolved(
                    connection_info_, connection_resolved_, connection_resolver_,
                    [this](const ConnectionInfo& info) {
                        std::string reason;
                        auto link = OnStepProtocolWrapper::acquire(info, &reason);
                        if (link) {
                            // The link's loss stamps read this driver's clock (decision 0005).
                            link->set_task_clock(clock_);
                            link_.store(std::move(link));
                        } else {
                            // The wrapper's reason (a port another device holds, #1202)
                            // goes to the operator.
                            const std::string message = reason.empty() ? "Failed to connect to OnStep mount"
                                                                       : "Failed to connect to OnStep mount: " + reason;
                            // Refused TCP connect, vanished serial node, or a port another connected
                            // device now holds (#1202): stale, re-scan.
                            if (info.type == ConnectionType::Network || util::serial_endpoint_stale(info.port_path)) {
                                throw util::StaleEndpoint(message);
                            }
                            throw AlpacaException(message);
                        }
                    },
                    "OnStep");
            });
            connected_ = true;
            if (stop_motion_after_link_loss_locked()) {
                relink_watch.reset();  // the driver stopped the motion: its end is the driver's own
            }
            polled_.reset();
            site_latitude_known_ = false;
            site_longitude_known_ = false;
            utc_offset_.clear();
            mount_clock_unreadable_.reset();
            client_disagreement_warned_ = false;
            firmware_reports_guide_move_ = false;
            target_ra_set_ = false;
            target_dec_set_ = false;
            clear_manual_motion_locked();
            slew_hold_.clear();
            clear_pulse_guides_locked();
            tracking_before_slew_ = false;
            status_read_since_move_ = false;
            begin_motion_command_locked();  // no stored motion failure or stop check outlives a session (#2406)
            synced_this_session_ = false;   // the first sync of a session is not bounded (#2406)
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();
            site_read_warned_ = false;

            // These reads run without firmware_mutex_: get_device_firmware()
            // takes only that lock and must not wait for a serial round trip
            // (decision 0016). Only the store is under it.
            std::string firmware_version;
            std::string firmware;
            std::string model;
            try {
                std::string product = protocol.get_product_name();
                firmware_version = protocol.get_version_number();
                firmware = product + (firmware_version.empty() ? "" : (" " + firmware_version));
            } catch (...) {  // NOLINT(bugprone-empty-catch): the firmware name stays empty
                firmware.clear();
            }
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = firmware;
            }

            // OnStepX only, and only a release that has :GVC# (firmware_answers_product_description()); the "0"
            // guard below is for a build that answers anyway.
            if (firmware_answers_product_description(firmware_version)) {
                try {
                    // The builder's free text: its qualifiers and anything after a comma leave the name, and an
                    // "OnStepX" description is not branded twice.
                    std::string description = util::without_qualifiers(protocol.get_product_description());
                    if (const auto comma = description.find(','); comma != std::string::npos) description.resize(comma);
                    if (!description.empty() && description != "0") model = util::branded_name("OnStep", description);
                } catch (...) {  // NOLINT(bugprone-empty-catch): the default name stays
                    // Unknown: the name stays "OnStep Telescope".
                }
            }
            // OnStepX applies refraction to the RA/Dec it reports unless it was
            // built for observed-place coordinates; :GXEE# says which (#1216).
            refraction_from_firmware_.reset();
            pier_settings_.reset();
            reset_rate_state_locked();
            onstepx_ = is_onstepx_version(firmware_version);
            firmware_identified_ = !firmware_version.empty();
            if (onstepx_) {
                try {
                    if (const auto mode = protocol.get_mount_coords_mode()) {
                        refraction_from_firmware_ = (*mode != 0);
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch): DoesRefraction keeps the local flag
                    // Unknown: DoesRefraction keeps the settable local flag.
                }
                // The pier-side settings (#2179): a mount that does not answer them leaves them unknown, which
                // never fails the connect; SideOfPier then follows the mount's letter and DestinationSideOfPier
                // the hour-angle rule.
                try {
                    refresh_pier_settings_locked(/*at_connect=*/true);
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Pier-side settings unknown: ") + ex.what());
                }
            }

            // Each coordinate is known only once its own write landed (or the
            // mount's pair was read): a field whose write failed stays unknown,
            // so its getter throws instead of serving a default 0.0, and the
            // pair counts as known (site_known_locked()) only with both. The
            // :Gt#/:Gg# read below, then the status poll, fetch the mount's
            // pair while it is not (ensure_site_info_cached_locked()).
            //
            // Neither firmware refuses :St#/:Sg# for the mount's state (aligned,
            // parked, slewing): OnStepX (Site.command.cpp) and OnStep 4.x
            // (Command.ino) answer 0 only for a value they cannot parse or that
            // is out of range. A failed write is therefore a bad config value or
            // a link that did not answer. The connect still succeeds, as before,
            // with the mount's own site read back below; the config values stay
            // as they are and are written again at the next connect.
            if (pending_site_latitude_.has_value()) {
                try {
                    protocol.set_latitude(pending_site_latitude_.value());
                    site_latitude_cached_ = pending_site_latitude_.value();
                    site_latitude_known_ = true;
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Site latitude from the device config not written to the "
                                                          "mount; using the mount's own site: ") +
                                                  ex.what());
                }
            }
            if (pending_site_longitude_.has_value()) {
                try {
                    protocol.set_longitude(pending_site_longitude_.value());
                    site_longitude_cached_ = pending_site_longitude_.value();
                    site_longitude_known_ = true;
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Site longitude from the device config not written to the "
                                                          "mount; using the mount's own site: ") +
                                                  ex.what());
                }
            }
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            // A failed time sync (the mount refused :SL#/:SC#/:SG#, or did not
            // answer) leaves the mount on its own clock, which the read below
            // then seeds UTCDate from. The connect still succeeds, as for the
            // site writes above (#1754).
            // open-astro#2419: this connect decides on the host clock here, so a change seen before now needs no
            // re-sync of its own; one seen from here to the end of this call re-syncs as it returns. Untrusted, the
            // gate logs the skip (#2480): a Pi with no RTC boots on the time of its last shutdown, and writing it put
            // RC51's mount 6 h 48 min out (#2419). The site writes above do not depend on it, and the read below
            // seeds UTCDate.
            host_clock_gate_.drop_owed();
            host_clock_gate_.begin_connect();
            if (sync_time_on_connect_ && host_clock_gate_.may_write()) {
                try {
                    set_utc_date_locked(std::chrono::system_clock::now());
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Time sync on connect failed; the mount keeps its own "
                                                          "clock: ") +
                                                  ex.what());
                }
            }
            // The mount clock is read here, once, so UTCDate (and so DeviceState)
            // performs no serial exchange of its own (#1478); a written UTCDate needs none.
            if (!utc_offset_.valid()) {
                seed_utc_from_mount_locked();
            }
            try {
                // Continuous-motion (:Mn#/:Ms#/:Me#/:Mw#) commands run at
                // whatever slew rate the firmware currently has selected;
                // pin it to max on connect so MoveAxis completes promptly.
                protocol.select_max_slew_rate();
            } catch (...) {  // NOLINT(bugprone-empty-catch): MoveAxis keeps the current rate
            }

            // The first frame, so the first property reads, status flags included,
            // are served from memory the moment Connect returns (ConformU's
            // fast-response targets, #1242), and the mount's site when the
            // config did not set both coordinates.
            seed_snapshot_locked();
            if (relink_watch) {
                // #2296, #2406: the relink left the watched motion running, which only a park still running gets
                // (stop_motion_after_link_loss_locked()). Its first status since the outage decides at once, by the
                // same end rules as any other, else the watch goes on. The park and home bound still runs from the
                // command.
                watched_motion_ = relink_watch;
                const auto seeded = polled_.read();
                if (seeded && seeded->value.status_known) {
                    try {
                        note_motion_end_locked(
                            seeded->value.status,
                            seeded->value.equatorial_known
                                ? std::optional<Position>(Position{seeded->value.ra_hours, seeded->value.dec_degrees})
                                : std::nullopt);
                    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the link check below decides
                        // Only a lost link escapes it (its position read): the link check below fails the connect as
                        // for any seed read that loses it (#1895).
                    }
                }
            }
            seed_rate_state_locked();
            seed_park_home_bound_locked();
            // The seed takes each failed read as a silent mount's, so a link lost during the connect (decision
            // 0009) shows only here: Connect fails instead of returning while Connected reads false (#1895).
            if (!protocol.link_alive()) {
                const std::string lost = link_lost_reason();
                fail_connect_locked("the link was lost during the connect" +
                                    (lost.empty() ? std::string() : " (" + lost + ")"));
            }
            // Published once the connect can no longer fail on the link, so a failed connect keeps the previous
            // name (decision 0016 item 3).
            if (!model.empty()) {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                model_cache_ = model;
            }
            published_connected_ = true;  // the connect is complete (#910)
            // The poll starts once the connect is complete, with no driver lock held (decision 0006 rule 4). Its
            // first probe is the seed above, so start() sends nothing and the first tick is one period away.
            lock.unlock();
            if (!start_status_poller()) {
                // Nothing would refresh the state for the whole session (#1895): undone as a Disconnect is,
                // the pulse-end body a PulseGuide may have started meanwhile joined first, with no lock held.
                stop_status_poller();
                lock.lock();
                fail_connect_locked("the status poll did not start");
            }
        } else {
            // Decision 0016, item 4: Connected goes false under mutex_ BEFORE the close, which runs through
            // Wire with mutex_ released; a getter during the close reads NotConnected, not a closing link.
            published_connected_ = false;
            connected_ = false;
            reset_session_state_locked();
            protocol.call([this](auto&) {
                run_disconnect_before_close_hook();
                release_link();
            });
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        if (!get_connected()) return {};  // listed while connected, like every telescope action
        return {std::string(util::kFindHomeSensorSearchAction)};
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        if (util::is_findhome_sensor_search_action(action_name)) {
            // FindHome sends :hC#, a slew to the home position the controller stores. OnStepX can be built with
            // home sensors, but the driver cannot read that build option, so it reports false.
            if (!get_connected()) throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
            const bool sensor_search = false;
            return util::format_findhome_sensor_search(sensor_search, util::kFindHomeGotoStoredHome, "");
        }
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view action_name) const override {
        return get_connected() && util::is_findhome_sensor_search_action(action_name);
    }

    std::string command_blind(std::string_view command, bool raw) override {
        (void)raw;
        StateLock lock(lock_gate_);
        check_connected();
        wire().send_command_blind(std::string(command));
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        StateLock lock(lock_gate_);
        check_connected();
        const Wire protocol = wire();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return response == "1";
        }
        protocol.send_command_blind(std::string(command));
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)raw;
        StateLock lock(lock_gate_);
        check_connected();
        return wire().send_command(std::string(command));
    }

    // Driver state the poll keeps current from the :GU# mount-type letter (decision 0010 OnStep row, #2214), no
    // device I/O (decision 0016). Fork is Polar;
    // alt-az and alt-alt are AltAz (ASCOM has no alt-alt value, and the decisions a client takes from it, pier side
    // and meridian flips, match alt-az). GermanPolar while disconnected, with
    // no throw (#1919 owns the disconnected getter behaviour). Connected, a latched fault, a lost link, no :GU#
    // answer since the connect and a status with no type letter throw as the other frame getters do (decisions 0009,
    // 0010 rule 8).
    AlignmentMode get_alignment_mode() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        return alignment_mode_locked();
    }

    double get_altitude() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_altaz_locked().altitude_degrees;
    }

    // The optics and SlewSettleTime are the client's values, not the mount's: lock-free and answered on a faulted
    // link, but NotConnected exactly when Connected reads false, like every other member (ASCOM contract precedence).
    // The optics setters are not ASCOM members: the catalog and updatedevice (#2057) inject the configured values
    // with them, connected or not.
    double get_aperture_diameter() const override {
        check_connected_lock_free();
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
        }
        aperture_diameter_m_ = meters;
    }

    double get_aperture_area() const override {
        check_connected_lock_free();
        // Derived from the atomic diameter so a live updatedevice cannot leave a stale area (#2057).
        const double d = aperture_diameter_m_;
        return d > 0.0 ? std::numbers::pi * (d / 2.0) * (d / 2.0) : 0.0;
    }

    bool get_at_home() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        raise_motion_error_locked(MotionKind::Home);
        return served_status_locked().is_at_home;
    }

    bool get_at_park() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        raise_motion_error_locked(MotionKind::Park);
        return served_status_locked().is_parked;
    }

    double get_azimuth() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_altaz_locked().azimuth_degrees;
    }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // commanded state, not a wire read: refused too (decision 0009 rule 1)
        const auto now = clock_.now();
        const auto reading = polled_.read();
        bool guiding = false;
        for (int axis = 0; axis < 2; ++axis) {  // ASCOM: true while either axis pulses
            if (!pulse_guiding_active_[axis]) {
                continue;
            }
            const util::HoldWindow& hold = pulse_hold_[axis];  // armed for the pulse's duration (decision 0022)
            // Past the pulse's end, until a frame sampled kPulseGuideSettle or more after it is published, the
            // position getters would serve one measured during the pulse, so the pulse still reads as running
            // (bounded by kPulseMeasureBound, for a link that does not answer); the poll takes that sample then.
            // `hold.active(t - d)` is `t < end + d`.
            const bool unmeasured = reading && hold.active(reading->value.sampled_at - kPulseGuideSettle) &&
                                    hold.active(now - kPulseMeasureBound);
            if (hold.active(now) || unmeasured) {
                guiding = true;
            } else {
                pulse_guiding_active_[axis] = false;
            }
        }
        return guiding;
    }

    // The rate offsets (:SXTR/:SXTD) exist on OnStepX only; OnStep 4.x keeps both false, and so does a driver that
    // is not connected (the firmware is not known). Answers from the driver's state, no I/O.
    bool get_can_set_declination_rate() const override { return can_set_rate_offsets(); }
    bool get_can_set_right_ascension_rate() const override { return can_set_rate_offsets(); }
    bool can_set_rate_offsets() const {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        return published_connected_.load() && onstepx_;
    }
    bool get_can_set_guide_rates() const override { return false; }
    bool get_can_set_park() const override { return true; }
    // True only on OnStepX, on a German mount, with the meridian flips on and a preferred-side letter read from
    // the mount (#2179): the flip is :MNe#/:MNw#, and its reach check needs the settings. Answers from the
    // driver's state, no I/O; false while disconnected.
    bool get_can_set_pier_side() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        return can_set_pier_side_locked();
    }
    bool get_can_set_tracking() const override { return true; }
    bool get_can_slew_alt_az() const override { return true; }
    bool get_can_slew_alt_az_async() const override { return true; }
    bool get_can_sync_alt_az() const override { return true; }
    bool get_can_slew() const override { return true; }
    bool get_can_slew_async() const override { return true; }
    bool get_can_sync() const override { return true; }
    bool get_can_unpark() const override { return true; }

    // RightAscension and Declination: the frame, or while a rate offset runs the model (#2241,
    // served_sky_position_locked()); DeviceState calls these getters, so it serves the same value.
    double get_declination() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_sky_position_locked().dec_degrees;
    }

    // The rate offsets are driver state, read from the mount at connect, after each write and when the polled
    // frame shows a change (read_rate_offsets_locked()); a getter asks the mount nothing (decision 0016). ITelescopeV4:
    // a read is 0 unless TrackingRate is driveSidereal. A stale frame is served as it is (decision 0010 rule 6); a
    // latched fault throws (decision 0009 rule 1, as Sky-Watcher's rate getters).
    double get_declination_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        if (tracking_rate_served_locked() != 0 || !onstepx_) {
            return 0.0;
        }
        return rate_offset_dec_arcsec_ / kSiSecondsPerSiderealSecond;
    }

    void set_declination_rate(double rate) override {
        set_rate_offset("DeclinationRate", rate, kSiSecondsPerSiderealSecond, /*ra=*/false);
    }

    bool get_tracking() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_status_locked().is_tracking;
    }

    void set_tracking(bool tracking) override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("Tracking");
        // The write-through below needs a status to land in (#1895). Tracking=false is a stop, so an unread status
        // never holds it up: :ST0 goes out and, if the read failed, the write-through does not land.
        ensure_status_read_locked("Tracking", /*refuse=*/tracking);
        if (tracking) {
            check_not_parked_locked("Tracking");
        }
        // The reply is read, not drained: OnStep 4.x refuses :ST while slewing
        // (#1379); the internal tracking restores stay blind.
        // OnStepX starts tracking with :Te#, which keeps the selected rate; :ST would set it back to sidereal.
        // The reply, then :GE# for its code, with no sibling device's exchange between them (#2223).
        const auto sequence = wire().hold_sequence();
        if (!wire().set_tracking_checked(tracking, onstepx_)) {
            // With no :GE# code, refusal_code_locked() has just re-read :GU#: a
            // mount that now tracks as asked took it (#1379 review).
            if (const auto code = refusal_code_locked("Tracking");
                code && !(*code < 0 && frame_status_locked().is_tracking == tracking)) {
                throw mount_refused("Tracking", *code);
            }
        }
        // Write-through (decision 0010): the next Tracking read sees it before the next poll.
        write_status_locked([tracking](MountStatus& status) { status.is_tracking = tracking; });
        drop_offset_model_locked();  // #2241: a frame measured from here anchors it again
        // The client has said what it wants; a pending GOTO restore must not
        // override it when the slew ends (#1230).
        tracking_before_slew_ = false;
    }

    double get_focal_length() const override {
        check_connected_lock_free();
        return focal_length_m_;
    }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    // The mount's pulse-guide rate (:GX90#, a multiple of sidereal), read at connect and when the pulse-guide digit
    // of :GU# changes; the same on both axes. Until it answers, and on firmware that never does, the informational
    // half-sidereal value. No I/O. A value read from the mount, so a latched fault throws (decision 0009 rule 1).
    GuideRate get_guide_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        throw_if_faulted_locked();
        const double rate = guide_rate_multiple_ ? *guide_rate_multiple_ * kSiderealArcsecPerSecond / 3600.0
                                                 : kInformationalGuideRateDegPerSec;
        return {rate, rate};
    }

    void set_guide_rate(const GuideRate& rate) override {
        (void)rate;
        check_connected_lock_free();
        throw AlpacaException("Guide rate is fixed by OnStep firmware", AlpacaError::PropertyNotImplemented);
    }

    double get_right_ascension() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_sky_position_locked().ra_hours;
    }

    double get_right_ascension_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);  // see get_declination_rate()
        check_connected();
        if (tracking_rate_served_locked() != 0 || !onstepx_) {
            return 0.0;
        }
        return rate_offset_ra_arcsec_ / kRaArcsecPerRaSecond;
    }

    void set_right_ascension_rate(double rate) override {
        set_rate_offset("RightAscensionRate", rate, kRaArcsecPerRaSecond, /*ra=*/true);
    }

    // ITelescopeV4 RightAscensionRate / DeclinationRate write (OnStepX only): `rate` times `to_firmware` is the
    // offset in arcseconds per sidereal second that :SXTR/:SXTD take. Order: the value (InvalidValue, before the
    // connection check: the firmware clamps silently at +/-1800, so a larger request is refused instead), connected,
    // the firmware (OnStep 4.x has no offset command), a latched fault, then TrackingRate not sidereal
    // (InvalidOperation, ITelescopeV4). A refusal throws with the :GE# reason (#1379).
    void set_rate_offset(const char* who, double rate, double to_firmware, bool ra) {
        util::check_rate_offset_arguments(
            kCommandLimits, ra ? util::RateOffsetAxis::RightAscension : util::RateOffsetAxis::Declination, rate);
        const double offset = rate * to_firmware;
        StateLock lock(lock_gate_);
        check_connected();
        if (!onstepx_) {
            throw AlpacaException(std::string(who) + " is not supported by this firmware",
                                  AlpacaError::PropertyNotImplemented);
        }
        refuse_while_faulted_locked(who);
        if (tracking_rate_served_locked() != 0) {
            throw AlpacaException(std::string(who) + " can only be set while TrackingRate is driveSidereal",
                                  AlpacaError::InvalidOperation);
        }
        const Wire protocol = wire();
        // #2241: the model runs on under the old offset up to the write, which the mount applies as it takes it; a
        // refused write leaves the offsets, and the model with them, as they were.
        reanchor_offset_model_locked();
        // The reply, then :GE# for its code, with no sibling device's exchange between them (#2223).
        const auto sequence = protocol.hold_sequence();
        if (!(ra ? protocol.set_rate_offset_ra(offset) : protocol.set_rate_offset_dec(offset))) {
            if (const auto code = refusal_code_locked(who)) {
                throw mount_refused(who, *code);
            }
        }
        // What the mount took, read back (it may differ by the firmware's float precision); the written value
        // stands until the read answers.
        (ra ? rate_offset_ra_arcsec_ : rate_offset_dec_arcsec_) = offset;
        try {
            read_rate_offsets_locked();
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_INFO("OnStep",
                            std::string("Rate offset read-back failed; the written value is kept: ") + ex.what());
        }
    }

    int get_side_of_pier() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        // The mount's own side: the pier-side letter of :GU# ('T' east, 'W' west), from the polled frame (decision
        // 0010), so the getter never reaches the mount. OnStepX keeps the side it is on past the meridian, up to
        // its meridian limit, while its preferred side is Best (the default; "BEST stays on current side if
        // possible", Goto.cpp), so the hour-angle sign says the wrong side there and a client skips its meridian
        // flip (#2179). The old reason for computing it from the hour angle, ConformU reading "EE" at HA -9 and
        // +9, was the #1217 misparse: 'E' is the mount-type letter (GEM), not the pier side, and is not read as one.
        // Only on an OnStepX German mount, the firmware and mount type checked on the rig; a fork or alt-az mount
        // and OnStep 4.x keep the hour-angle rule.
        const PolledFrame frame = served_frame_locked();
        if (onstepx_ && frame.status_known && frame.status.is_gem &&
            (frame.status.side_of_pier == 0 || frame.status.side_of_pier == 1)) {
            return frame.status.side_of_pier;
        }
        // 'o' (parked or at home: no side), firmware that sends no letter, or a mount outside the gate above: the
        // hour-angle rule, whose sign
        // tolerates the frame's second of RA staleness. Without a site this refuses, as the site getters do (#1893).
        const double ra = served_position_locked().ra_hours;
        throw_if_site_unknown_locked("SideOfPier");
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, ra);
        return util::pier_side_for_hour_angle(hour_angle);
    }

    // The flip (#2179): :MNe#/:MNw# is OnStepX's own GOTO to the current RA/Dec on the east or west side of the
    // pier. It writes nothing to the mount's settings and does not retarget. At home OnStepX's setTarget
    // overrides the requested side with the target's hour angle, so a flip while the mount reports no side is
    // refused here. The call returns once the GOTO is accepted; Slewing reads true until it ends (the ASCOM
    // async rule).
    void set_side_of_pier(int side) override {
        if (side != 0 && side != 1) {  // before the connection check
            throw AlpacaException("SideOfPier must be 0 (east) or 1 (west)", AlpacaError::InvalidValue);
        }
        StateLock lock(lock_gate_);
        check_connected();
        if (!can_set_pier_side_locked()) {
            throw AlpacaException("Pier side is not settable", AlpacaError::PropertyNotImplemented);
        }
        const char* const who = "SideOfPier";
        check_not_parked_locked(who);
        refuse_while_faulted_locked(who);
        if (!tracking_as_served_locked()) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
        refuse_while_goto_running_locked(who);  // reads :GU# now
        const int current = frame_status_locked().side_of_pier;
        if (current == -1) {
            throw AlpacaException(std::string(who) + " cannot be set while the mount reports no pier side (at home)",
                                  AlpacaError::InvalidOperation);
        }
        if (current == side) {
            return;  // already there: nothing to send
        }
        const Position position = wire().get_position();
        ensure_site_info_cached_locked();
        throw_if_site_unknown_locked(who);
        if (!pier_settings_) {
            throw AlpacaException("Pier side is not settable", AlpacaError::PropertyNotImplemented);
        }
        const PierSettings settings = *pier_settings_;
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, position.ra_hours);
        if (!util::gem_pier_side_reaches(settings.model_limits(), side, hour_angle)) {
            char reason[200];
            std::snprintf(reason, sizeof(reason),
                          "the %s side cannot reach the current position (hour angle %+.2f h; RA-axis and "
                          "meridian limits: %d min east, %d min west; RA axis %d to %d degrees)",
                          side == 0 ? "east" : "west", hour_angle, settings.east_limit_minutes,
                          settings.west_limit_minutes, settings.axis1_min_degrees, settings.axis1_max_degrees);
            throw AlpacaException(std::string(who) + " cannot be set: " + reason, AlpacaError::InvalidOperation);
        }
        // The flip does not retarget: the targets stay as the client set them.
        const char letter = side == 0 ? 'e' : 'w';
        dispatch_goto_locked(
            who, side == 0 ? ":MNe#" : ":MNw#", [](const Wire&) {},
            [letter](const Wire& protocol) { return protocol.goto_current_position_on_side(letter); });
        if (watched_motion_) {
            // #2296: the flip's target is the side and the position it slews back to.
            watched_motion_->target_side = side;
            watched_motion_->target = std::make_pair(position.ra_hours, position.dec_degrees);
        }
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        // Arguments first, even while disconnected (#1478).
        util::check_destination_side_of_pier_arguments(ra, dec);
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // the site and clock the connect read (decision 0009 rule 1)
        throw_if_site_unknown_locked("DestinationSideOfPier");
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, ra);
        // The side the firmware would pick, from its settings and the current side (#2179). Anything unknown, or a
        // target no side reaches (the firmware would refuse the GOTO), falls back to the hour-angle rule.
        if (const auto modelled = modelled_destination_side_locked(lst, hour_angle, dec)) {
            return *modelled;
        }
        return util::pier_side_for_hour_angle(hour_angle);
    }

    EquatorialSystem get_equatorial_system() const override { return EquatorialSystem::Topocentric; }

    // OnStepX reports its refraction mode at connect (#1216). It is fixed when
    // the firmware is built, so only the value the firmware already uses can
    // be written. Older firmware keeps the local flag.
    bool get_does_refraction() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // the connect's :GXEE# value, a mount read (decision 0009 rule 1)
        return refraction_from_firmware_.value_or(does_refraction_);
    }

    void set_does_refraction(bool does_refraction) override {
        StateLock lock(lock_gate_, StateLock::Mode::State);  // no wire I/O, see set_target_declination()
        check_connected();
        if (refraction_from_firmware_.has_value()) {
            if (does_refraction != *refraction_from_firmware_) {
                throw AlpacaException(std::string("This OnStepX firmware ") +
                                          (*refraction_from_firmware_ ? "always applies" : "never applies") +
                                          " refraction to its coordinates; it is set when the firmware is built",
                                      AlpacaError::PropertyNotImplemented);
            }
            return;
        }
        does_refraction_ = does_refraction;
    }

    int get_slew_settle_time() const override {
        check_connected_lock_free();
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        check_connected_lock_free();  // an ASCOM member: InvalidValue first, then NotConnected
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // the site and clock the connect read (decision 0009 rule 1)
        throw_if_site_unknown_locked("SiderealTime");
        return util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
    }

    // Unset (NaN) until the device config or a client sets it: 0 m is a real
    // place (decision 0019, #1478). Lock-free: the protocol has no elevation
    // command, so this never waits on the mount.
    double get_site_elevation() const override {
        const double elevation = site_elevation_m_.load();
        if (std::isnan(elevation)) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return elevation;
    }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters", AlpacaError::InvalidValue);
        }
        // Client-side only: the LX200/OnStep protocol has no elevation command.
        site_elevation_m_ = elevation;
    }

    // Unknown until the device config, a client write or a :Gt#/:Gg# read set it: InvalidOperation, never a
    // default 0.0 (#1783, decision 0019, the ASCOM time and place FAQ). The poll retries an unread site; the
    // getter never reads the mount.
    double get_site_latitude() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();
        if (!site_latitude_known_) {
            throw AlpacaException(site_unknown_message("SiteLatitude"), AlpacaError::InvalidOperation);
        }
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        util::validate_latitude(latitude);
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("SiteLatitude");  // as the other writes (#1626)
        refuse_while_moving_locked("SiteLatitude");
        // Read the mount's pair first while it is not known, so the longitude is
        // the mount's and not a default. A failed read here is logged and leaves
        // it unknown, so the poll asks again; it does not refuse the write, whose
        // own refusal throws (#1754).
        ensure_site_info_cached_locked();
        wire().set_latitude(latitude);
        site_latitude_cached_ = latitude;
        site_latitude_known_ = true;
    }

    double get_site_longitude() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();
        if (!site_longitude_known_) {
            throw AlpacaException(site_unknown_message("SiteLongitude"), AlpacaError::InvalidOperation);
        }
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        util::validate_longitude(longitude);
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("SiteLongitude");  // as the other writes (#1626)
        refuse_while_moving_locked("SiteLongitude");
        // See set_site_latitude(): the mount's latitude first, while unknown.
        ensure_site_info_cached_locked();
        wire().set_longitude(longitude);
        site_longitude_cached_ = longitude;
        site_longitude_known_ = true;
    }

    bool get_slewing() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        if (park_home_request_) {
            // This answer asks the mount nothing, so it refuses a latched fault
            // itself (decision 0009 rule 1); AbortSlew and MoveAxis(axis, 0) still
            // send their stops while faulted, only this read refuses.
            throw_if_faulted_locked();
            return true;  // stopping, settling or retrying a Park or FindHome (#1430)
        }
        raise_motion_error_locked(std::nullopt);
        if (!bound_error_.empty()) {
            throw AlpacaException(bound_error_, AlpacaError::DriverException);
        }
        return get_slewing_locked();
    }

    // #2406: the next motion command ends the stored failure and the AbortSlew stop check.
    void begin_motion_command_locked(bool keep_goto_bound = false) const {
        bound_error_.clear();
        abort_stop_rounds_ = 0;
        if (!keep_goto_bound) {
            goto_deadline_.reset();
            goto_target_.reset();
            goto_running_ = false;
            goto_seen_slewing_ = false;
        }
    }

    // #2406 (#2396 AB-10): a site or time write changes the firmware's limit and pier maths, so none goes out while
    // a motion of this driver runs: a GOTO until it lands, a MoveAxis, a Park or FindHome being started. Only the
    // driver's own state is read, so a :GU# the driver cannot parse refuses nothing; a motion from a hand
    // controller is not seen. A pulse guide does not count.
    void refuse_while_moving_locked(const char* who) const {
        const bool moving = slew_hold_.active(clock_.now()) || goto_unlanded_locked() || manual_axis_slewing_[0] ||
                            manual_axis_slewing_[1] || park_home_request_;
        if (moving) {
            throw AlpacaException(std::string(who) + " is not allowed while the mount is moving",
                                  AlpacaError::InvalidOperation);
        }
    }

    double get_target_declination() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // a commanded value is refused on a faulted link (decision 0009 rule 1)
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        util::check_target_declination_arguments(dec);
        StateLock lock(lock_gate_, StateLock::Mode::State);  // no wire I/O: never waits behind a command (#1728 review)
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
        ++target_dec_writes_;
    }

    double get_target_right_ascension() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // a commanded value is refused on a faulted link (decision 0009 rule 1)
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        util::check_target_right_ascension_arguments(ra);
        StateLock lock(lock_gate_, StateLock::Mode::State);  // see set_target_declination()
        target_ra_hours_ = ra;
        target_ra_set_ = true;
        ++target_ra_writes_;
    }

    // The rate OnStepX tracks at: driver state the poll keeps current from the :GU# letter ('(' lunar, 'O' solar,
    // 'k' King, none sidereal; decision 0010 OnStep row, #2214); no I/O. The letters are written only while tracking
    // compensation is off: with it on the driver's own writes and :GT# answer (tracking_rate_served_locked()).
    // Always sidereal on OnStep 4.x. A stale frame keeps the last letter seen (decision 0010 rule 6); a latched fault
    // throws (decision 0009 rule 1).
    int get_tracking_rate() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return tracking_rate_served_locked();
    }

    // DriveRates 0-3 (InvalidValue for anything else, before the connection check), then connected, a latched
    // fault, and a rate the firmware offers (OnStep 4.x: sidereal only). OnStepX: :TQ#/:TL#/:TS#/:TK# (no reply),
    // written through, then the rate offsets are read again. A change of rate with a non-zero offset zeroes
    // both first (:SXTR,0# :SXTD,0#; see below).
    void set_tracking_rate(int rate) override {
        util::check_tracking_rate_arguments(kCommandLimits, rate);
        StateLock lock(lock_gate_);
        util::check_tracking_rate_state(kCommandLimits, GateChecks{*this, "TrackingRate"});
        if (!onstepx_) {
            if (rate != 0) {
                throw AlpacaException("This firmware tracks at the sidereal rate only", AlpacaError::InvalidValue);
            }
            return;
        }
        // OnStepX keeps applying the rate offsets on top of a lunar, solar or King rate (Mount.cpp 476-515, af1d26bf)
        // while the getters read 0 and writes are refused, so a client could never clear them after the switch. As
        // the Sky-Watcher driver does on a rate change, zero them first (ITelescopeV4 does not require it).
        if ((rate_offset_ra_arcsec_ != 0.0 || rate_offset_dec_arcsec_ != 0.0) &&
            rate != tracking_rate_served_locked()) {
            const Wire protocol = wire();
            for (const bool ra : {true, false}) {
                reanchor_offset_model_locked();  // #2241: the position holds where the old offsets took it
                const auto sequence = protocol.hold_sequence();  // the reply and its :GE# together (#2223)
                if (!(ra ? protocol.set_rate_offset_ra(0.0) : protocol.set_rate_offset_dec(0.0))) {
                    if (const auto code = refusal_code_locked("TrackingRate")) {
                        throw mount_refused("TrackingRate", *code);
                    }
                }
                (ra ? rate_offset_ra_arcsec_ : rate_offset_dec_arcsec_) = 0.0;
            }
        }
        wire().set_tracking_rate(rate);
        tracking_rate_known_ = rate;
        // Write-through (decision 0010): the next read sees it before the next poll. The next frame says it too,
        // unless tracking compensation hides the letter.
        write_status_locked([rate](MountStatus& status) { status.tracking_rate = rate; });
        rate_watch_ = rate_watch_of(frame_status_locked());
        try {
            read_rate_offsets_locked();
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_INFO("OnStep", std::string("Rate offsets not read after the TrackingRate write: ") + ex.what());
        }
    }

    // OnStepX knows the four DriveRates (:TQ# :TL# :TS# :TK#); OnStep 4.x, and a driver that is not connected,
    // sidereal only.
    std::vector<int> get_tracking_rates() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (published_connected_.load() && onstepx_) {
            return {0, 1, 2, 3};
        }
        return {0};
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // no I/O, but a faulted read throws (decision 0009 rule 1)
        if (!utc_offset_.valid()) {
            // The host clock is not the mount's (ITelescopeV4 UTCDate, #1478). The
            // mount clock is read once at connect, so a getter never waits on it.
            throw AlpacaException("The mount clock could not be read at connect" +
                                      (mount_clock_unreadable_ ? ": " + *mount_clock_unreadable_ : std::string()) +
                                      "; write UTCDate to set it",
                                  AlpacaError::InvalidOperation);
        }
        return current_utc_time_locked();
    }

    // At connect (#1478): seeds utc_offset_ from :GL#/:GC#/:GG#, or keeps why it
    // could not, for UTCDate to report until a UTCDate write or the next connect.
    void seed_utc_from_mount_locked() {
        try {
            TimeInfo info = wire().get_time();
            using namespace std::chrono;
            int year = 2000 + info.year;
            sys_days date = sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(info.month)} /
                                     std::chrono::day{static_cast<unsigned>(info.day)}};
            auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
            auto offset = duration_cast<system_clock::duration>(
                std::chrono::duration<double, std::ratio<3600>>(info.utc_offset_hours));
            // OnStepX :GG# is the hours to add to local time to get UTC
            // (Site.command.cpp), so UTC = local + offset (#1626).
            utc_offset_.set(local_time + offset, clock_.now());
            mount_clock_unreadable_.reset();
        } catch (const std::exception& ex) {
            mount_clock_unreadable_ = ex.what();
        } catch (...) {
            mount_clock_unreadable_ = "unknown error";
        }
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        StateLock lock(lock_gate_);
        check_connected();
        // Here, not in set_utc_date_locked(): the connect's own time sync runs
        // right after polled_.reset(), where the check cannot fire (#1626).
        refuse_while_faulted_locked("UTCDate");
        refuse_while_moving_locked("UTCDate");
        set_utc_date_locked(utc);
        // A host clock re-sync decided before this write sends nothing (#2419). Bumped once the write landed, still
        // under the operation lock the re-sync's own write takes, so the two never interleave.
        utc_write_generation_.fetch_add(1);
    }

    // find_home()/park() are fire-and-forget, matching iOptron: send the
    // command and return immediately rather than blocking until the mount
    // physically finishes. ConformU classifies a Park/FindHome call that
    // blocks for the whole physical operation under the STANDARD (1.0s)
    // timing target and fails it outright; a call that returns immediately
    // and lets the client poll AtHome/AtPark afterward is classified
    // EXTENDED (600s) instead — confirmed against this project's iOptron
    // ConformU logs, where the real (tens-of-seconds) physical park/home
    // is entirely absorbed by the client's post-call AtPark/AtHome polling.
    // Not waited for: the physical park or home, nor (#1430) the end of a
    // motion the call had to stop first or a park or home the mount refuses
    // as "in motion": those finish in the park_home_ body, and a refusal it
    // meets is kept and raised by Slewing, AtPark or AtHome (the shared rule
    // "A failed async operation body keeps its error").
    void find_home() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("FindHome");
        check_not_parked_locked("FindHome");
        begin_motion_command_locked();
        refresh_status_locked();  // a control-path read, written through (decision 0010)
        if (const MountStatus status = frame_status_locked(); status.is_at_home || status.is_homing) {
            motion_error_.reset();  // a new command: an earlier failure is not this call's
            return;  // there, or on its way
        }
        read_home_geometry_locked();
        start_park_or_home_locked(lock, "FindHome", true);
    }

    void park() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("Park");
        begin_motion_command_locked();
        refresh_status_locked();  // a control-path read, written through (decision 0010)
        if (const MountStatus status = frame_status_locked(); status.is_parked || status.is_parking) {
            motion_error_.reset();  // a new command: an earlier failure is not this call's
            return;  // ASCOM: Park() on an already-parked mount is a no-op; one already parking too.
        }
        start_park_or_home_locked(lock, "Park", false);
    }

    void pulse_guide(int direction, int duration) override {
        // Arguments before the connected, fault and parked checks (#1478), and before the state lock.
        util::check_pulse_guide_arguments(kCommandLimits, direction, duration);
        StateLock lock(lock_gate_);
        util::check_pulse_guide_state(kCommandLimits, GateChecks{*this, "PulseGuide"});
        refuse_pulse_during_slew_locked();        // #2504: after the parked and fault checks, before anything is sent
        const int axis = direction >= 2 ? 0 : 1;  // East/West is axis 0, North/South axis 1
        // #2503 (the #2467 rule): a pulse on the axis a non-zero MoveAxis drives would run into the jog, so it is
        // refused and the jog runs on. The other axis is not checked. MoveAxis sets the flag under the operation lock
        // this call holds, so no jog lands between the check and the send.
        if (manual_axis_slewing_[axis]) {
            throw AlpacaException("PulseGuide is not allowed on an axis MoveAxis is moving",
                                  AlpacaError::InvalidOperation);
        }
        // #2522: the pulse claims its axis, after the jog check (motion_owner.h). The owner refuses it while the
        // driver's own slew, Park or FindHome holds its claim, with the #2504 text; nothing has been sent.
        (void)motion_owner_.claim(util::MotionCommand::PulseGuide, axis);
        // Hardware pulse guide: the mount times the pulse internally
        // (:Mgn####/:Mgs####/:Mge####/:Mgw####), so unlike a software-timed
        // implementation there is no background stop thread to manage here:
        // IsPulseGuiding reads the axis's hold window. Each axis has its own
        // (OnStepX times each axis on its own), so a shorter pulse on one axis
        // does not end the other's.
        const auto now = clock_.now();
        const auto pulse = std::chrono::milliseconds(duration);
        // The other axis's pulse outlasts this one: its pulse-end body, which measures after the later end, stays.
        const bool other_ends_later = pulse_guiding_active_[1 - axis] && pulse_hold_[1 - axis].active(now + pulse);
        // Decision 0022 rule 2: armed before the command goes out, cleared if it does not.
        pulse_guiding_active_[axis] = true;
        pulse_hold_[axis].arm(now, pulse);
        try {
            wire().pulse_guide(direction, duration);
        } catch (...) {
            pulse_guiding_active_[axis] = false;
            pulse_hold_[axis].clear();
            throw;
        }
        drop_poll_in_flight_locked();  // the position changes: a tick that sampled before the pulse is dropped
        drop_offset_model_locked();    // #2241: the next frame measured after the pulse anchors it again
        if (other_ends_later) {
            return;
        }
        // Supersede the pulse-end body of an earlier pulse here, under the locks (decision 0006 rule 3): a body
        // that wakes or takes the operation lock in the gap before start() replaces it sees the bump when its wait
        // returns or at its next stopped() check and acts on nothing (rule 6), so it sends no tracking restore and
        // publishes no frame. start() bumps again for the new body.
        pulse_measurement_generation_.bump();
        // One extra tick measures the position kPulseGuideSettle after the pulse ends (IsPulseGuiding waits for
        // that frame), in a slot of its own: the 1 s ticks run on through the pulse. Started with no driver lock
        // held (decision 0006 rule 4); a newer pulse that ends last replaces it.
        const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(pulse + kPulseGuideSettle);
        lock.unlock();
        run_pulse_guide_gap_hook();
        // A Disconnect can land between the unlock and the start: the slot's body is then harmless, because
        // poll_tick() checks the connection under the operation lock, and the next Connect (or the
        // destructor) joins the slot before a new session starts. A start during the Disconnect's join is refused.
        start_pulse_measurement(wait);
    }

    void set_park() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("SetPark");
        // OnStepX refuses :hQ# while parked, during a GOTO or a guide move, or on a
        // motor fault (Park.cpp set()); a refusal used to read as success (#1379).
        // The reply, then :GE# for its code, with no sibling device's exchange between them (#2223).
        const auto sequence = wire().hold_sequence();
        if (!wire().set_park()) {
            // Unlike Unpark and Tracking, no :GU# letter shows the park position,
            // so a 0 with no :GE# code cannot be confirmed and stays a refusal.
            if (const auto code = refusal_code_locked("SetPark")) {
                throw mount_refused("SetPark", *code);
            }
        }
    }

    void slew_to_coordinates(double ra, double dec) override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        util::check_equatorial_slew(kCommandLimits, ra, dec, "SlewToCoordinates",
                                    GateChecks{*this, "SlewToCoordinates"});
        start_slew_locked(ra, dec, "SlewToCoordinates", targets);
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        util::check_equatorial_slew(kCommandLimits, ra, dec, "SlewToCoordinatesAsync",
                                    GateChecks{*this, "SlewToCoordinatesAsync"});
        start_slew_locked(ra, dec, "SlewToCoordinatesAsync", targets);
    }

    // Under the target slew's own name, so a refusal names the member called (#1379).
    void slew_to_target() override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        const auto [ra, dec] = target_or_throw_locked();
        util::check_equatorial_slew(kCommandLimits, ra, dec, "SlewToTarget", GateChecks{*this, "SlewToTarget"});
        start_slew_locked(ra, dec, "SlewToTarget", targets);
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_target_async() override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        const auto [ra, dec] = target_or_throw_locked();
        util::check_equatorial_slew(kCommandLimits, ra, dec, "SlewToTargetAsync",
                                    GateChecks{*this, "SlewToTargetAsync"});
        start_slew_locked(ra, dec, "SlewToTargetAsync", targets);
    }

    std::pair<double, double> target_or_throw_locked() const {
        check_connected();  // before the unset target (#2075)
        if (!target_ra_set_ || !target_dec_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        return {target_ra_hours_, target_dec_degrees_};
    }

    void sync_to_coordinates(double ra, double dec) override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        sync_locked(ra, dec, "SyncToCoordinates", targets);
    }

    void sync_to_target() override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        const auto [ra, dec] = target_or_throw_locked();
        sync_locked(ra, dec, "SyncToTarget", targets);
    }

    // Under the member's own name (#1379). A sync during a GOTO is refused by the
    // mount (Goto.cpp validate(): CE_SLEW_IN_SLEW) and used to read as success, as
    // did every :CM# refusal; the driver refuses first and reads the reply.
    void sync_locked(double ra, double dec, const char* who, TargetWrites targets) {
        // Arguments first, even while disconnected (#1478); then connected, the fault (named, #1414) and parked.
        util::check_sync(kCommandLimits, ra, dec, who, GateChecks{*this, who});
        sync_to_mount_locked(ra, dec, who, targets);
    }

    // The sync itself, after the caller's checks (SyncToCoordinates, SyncToTarget, SyncToAltAz).
    void sync_to_mount_locked(double ra, double dec, const char* who, TargetWrites targets) {
        refuse_while_goto_running_locked(who);
        // The #2406 Decisions (2026-11-24): an OnStep mount has no reference at power-up, so the first sync
        // after each Connect sets it and is not bounded. Every later one may move the pointing by
        // util::kMaxSyncOffsetDegrees at most, from the position the frame holds.
        if (synced_this_session_) {
            const Position current = served_position_locked();
            util::check_sync_bound(who, {current.ra_hours, current.dec_degrees}, {ra, dec},
                                   util::kMaxSyncOffsetDegrees);
        }
        const Wire protocol = wire();
        // Use the mount's own sync command — do NOT maintain a driver-level
        // offset, which would cause coordinate divergence during subsequent
        // slews (project-wide rule; see the iOptron and Celestron files under .github/instructions/).
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        const std::string reply = protocol.sync_to_target();
        if (reply.size() >= 2 && reply[0] == 'E' && std::isdigit(static_cast<unsigned char>(reply[1]))) {
            throw digit_refused(who, RefusedCommand::Sync, reply[1], reply);
        }
        synced_this_session_ = true;
        store_target_locked(ra, dec, targets);
        // Write-through (decision 0010): the mount now reports the synced position; a tick that sampled before
        // the sync is dropped.
        polled_.write([ra, dec](PolledFrame& frame) {
            frame.ra_hours = ra;
            frame.dec_degrees = dec;
            frame.equatorial_known = true;
        });
        anchor_offset_model_at_sync_locked(ra, dec);  // #2241: a running offset goes on from the synced position
    }

    void unpark() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("Unpark");
        ensure_status_read_locked("Unpark");  // the write-through below needs a status to land in (#1895)
        cancel_motion_locked(util::MotionOwner::kBothAxes);  // a stop: also ends a Park or FindHome (#1430, #2522)
        // A refusal used to read as success (#1379): OnStepX refuses :hR# before the
        // date and time are set or on a motor fault (Park.cpp restore()). "Not
        // parked" (11) is ASCOM's no-op, not a refusal.
        // The reply, then :GE# for its code, with no sibling device's exchange between them (#2223).
        const auto sequence = wire().hold_sequence();
        if (!wire().unpark()) {
            // 11 (not parked) is ASCOM's no-op; with no :GE# code, the :GU# that
            // refusal_code_locked() has just re-read decides (#1379 review).
            if (const auto code = refusal_code_locked("Unpark");
                code && *code != kCeNotParked && !(*code < 0 && !frame_status_locked().is_parked)) {
                throw mount_refused("Unpark", *code);
            }
        }
        write_status_locked([](MountStatus& status) { status.is_parked = false; });  // decision 0010 write-through
    }

    bool get_can_move_axis(int axis) const override {
        if (axis != 0 && axis != 1 && axis != 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        // #2257: the argument checks run first, before the connected and parked checks.
        util::check_move_axis_arguments(kCommandLimits, axis, rate);
        StateLock lock(lock_gate_);
        // Only a rate at or under the epsilon is a stop, which an unread status does not hold up (#1895) and a faulted
        // link does not refuse before it is sent (#1414). A move checks the fault before it ends a waiting Park or
        // FindHome.
        const bool moving = !util::is_move_axis_stop(kCommandLimits, rate);
        util::check_move_axis_state(kCommandLimits, /*stop=*/!moving, GateChecks{*this, "MoveAxis"});
        // #2522: a move claims its axis, which supersedes a slew, Park or FindHome (they own both); a stop cancels
        // its axis. Either ends a Park or FindHome still to be sent (#1430).
        if (moving) {
            (void)claim_motion_locked(util::MotionCommand::MoveAxis, axis);
        } else {
            cancel_motion_locked(util::MotionOwner::axes_of(util::MotionCommand::MoveAxis, axis));
        }
        // A MoveAxis is not refused while a GOTO runs, so it keeps that GOTO's bound (and its site-write refusal).
        begin_motion_command_locked(/*keep_goto_bound=*/true);
        const Wire protocol = wire();

        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        const int positive_dir = (axis == 0) ? 2 : 0;
        const int negative_dir = (axis == 0) ? 3 : 1;

        if (!moving) {
            const std::string stop_error = stop_axis_locked(axis);
            if (!stop_error.empty()) {
                throw AlpacaException("MoveAxis stop failed: " + stop_error, AlpacaError::DriverException);
            }
            // #2241: a stop that ends a move or a pulse drops the rate-offset model, so a tick that sampled the
            // motion does not anchor it (the pulse's hold, which would have kept it out, is cleared below).
            if (manual_axis_slewing_[axis] || pulse_guiding_active_[axis]) {
                drop_offset_model_locked();
            }
            move_axis_cap_[axis].cancel();  // the stop went out: nothing is left for the cap to stop
            move_axis_cap_window_[axis].clear();
            // :Qe#/:Qw# and :Qn#/:Qs# end every guide on that axis, a pulse
            // guide included (OnStepX Guide.command.cpp stopAxis1/stopAxis2).
            pulse_guiding_active_[axis] = false;
            pulse_hold_[axis].clear();
        }
        std::uint64_t generation = 0;
        if (moving) {
            // Drop this axis's earlier move from the re-send first: the new
            // move sets it again only once its own command went out. This
            // only withdraws the old move, under mutex_ where no body runs;
            // decision 0006 rule 3's bump still comes before the new flags.
            manual_axis_resend_[axis] = false;
            const int dir = rate > 0.0 ? positive_dir : negative_dir;
            run_move_axis_send_hook();
            protocol.move_axis_start(dir, std::abs(rate));
            // This axis's cap sends no stop from here on (decision 0006 rules 3 and 6; #2651): its stop runs under
            // the operation lock this call holds and checks its stop reason there. A move command that threw never
            // gets here, and leaves the cap that was running.
            move_axis_cap_[axis].cancel();
            move_axis_cap_window_[axis].arm(clock_.now(), util::kMaxMoveAxisDuration);
            // Decision 0006 rule 3: supersede the running re-send under mutex_
            // before this move's flags change; start() below replaces it. Only
            // once the move went out: a failed send must leave the other
            // axis's re-send running.
            generation = move_axis_generation_.bump();
            manual_axis_dir_[axis] = dir;
            manual_axis_rate_[axis] = std::abs(rate);
            manual_axis_resend_[axis] = true;
            manual_axis_resend_due_[axis] = clock_.now() + kMoveAxisResendInterval;
            status_read_since_move_ = false;  // a :GU# from before the move must not end it (#1378)
            drop_poll_in_flight_locked();     // nor a poll tick that sampled before it
            drop_offset_model_locked();       // #2241: a frame measured after the move anchors it again
        }

        manual_axis_slewing_[axis] = moving;
        if (!moving) {
            if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
                clear_manual_motion_locked();
            }
            // Sent, but a silent mount may not have heard it (#1414).
            refuse_while_faulted_locked("MoveAxis stop");
            return;
        }
        // Always a fresh body, never "already running": a body that has just
        // found nothing to re-send may still read as running while it exits.
        // The new one supersedes it.
        lock.unlock();  // start() without mutex_: decision 0006 rule 4, async_operation.h rule 10
        try {
            start_move_axis_resend();
            run_move_axis_cap_before_start_hook();
            start_move_axis_cap(axis);
        } catch (const AlpacaException& ex) {
            lock.lock();
            stop_move_without_resend_locked(axis, generation, ex);
        }
    }

    // Stops `axis` as MoveAxis(axis, 0) does and drops its re-send first: a
    // stop asks for none whatever its outcome. Defensive dual-stop, matching
    // the project's convention for fixed-direction (rather than signed-rate)
    // motion protocols. Both stops are always tried. The driver cannot tell
    // which direction the mount needed stopped, so any failed stop may be the
    // one that mattered: the first error is returned, and the caller keeps
    // the axis moving (#742). Empty when both went out.
    std::string stop_axis_locked(int axis) {
        manual_axis_resend_[axis] = false;
        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        return send_stops_locked(false, {axis == 0 ? 2 : 0, axis == 0 ? 3 : 1});
    }

    // The stops this driver sends, from AbortSlew, MoveAxis(axis, 0) (stop_axis_locked()), the stops before
    // a Park or FindHome, a refused MoveAxis re-send and the relink after a link loss: :Q# when
    // `abort_goto`, then :Qn#/:Qs#/:Qe#/:Qw# for each direction in `directions`. Each is tried on its own,
    // so one lost command cannot skip the others (#781). They go through Wire: the operation lock keeps
    // them together, and mutex_ is free while they are on the wire (decision 0016, #1449). Returns the
    // first error, empty when every stop went out; `on_failure`, when set, also sees each failure with the
    // command it was.
    std::string send_stops_locked(
        bool abort_goto, std::initializer_list<int> directions,
        const std::function<void(const char*, const std::string&)>& on_failure = nullptr) const {
        const Wire protocol = wire();
        std::string stop_error;
        const auto try_stop = [&](const char* what, const std::function<void()>& stop) {
            std::string error;
            try {
                stop();
                return;
            } catch (const std::exception& ex) {
                error = ex.what();
            } catch (...) {
                error = "unknown exception";
            }
            if (stop_error.empty()) {
                stop_error = error;
            }
            if (on_failure) {
                on_failure(what, error);
            }
        };
        if (abort_goto) {
            try_stop(":Q#", [&protocol]() { protocol.abort_slew(); });
        }
        for (const int dir : directions) {
            try_stop("axis stop", [&protocol, dir]() { protocol.move_axis_stop(dir); });
        }
        return stop_error;
    }

    // #1458, #2651: the slot refused the re-send body or the cap body (no thread, or replaced bodies
    // still finishing) after the move went out. The bump above has already
    // superseded the running body, so the move would end at the firmware's
    // time limit while Slewing read true: stop this axis and report it, as a
    // MoveAxis that did not happen. Nothing is stopped when a newer MoveAxis
    // went out meanwhile (the generation moved on), or when AbortSlew, a GOTO,
    // Park, FindHome or a disconnect ended the move meanwhile (the axis no
    // longer reads as moving): its stops would end that newer command. After
    // a link drop the stops fail and the message says so. The other axis keeps
    // moving, without a re-send until the next MoveAxis.
    void stop_move_without_resend_locked(int axis, std::uint64_t generation, const AlpacaException& refusal) {
        if (move_axis_generation_.current() != generation || !manual_axis_slewing_[axis]) {
            throw refusal;
        }
        std::string message = std::string("MoveAxis could not keep the move going (") + refusal.what() + ")";
        const std::string stop_error = stop_axis_locked(axis);
        if (stop_error.empty()) {
            manual_axis_slewing_[axis] = false;
            move_axis_cap_[axis].cancel();
            move_axis_cap_window_[axis].clear();
            message += "; the axis was stopped";
        } else {
            message += "; stopping the axis failed too: " + stop_error;  // Slewing stays true (#742)
        }
        if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
            clear_manual_motion_locked();
        }
        ALPACA_LOG_WARN("OnStep", message);
        throw AlpacaException(message, AlpacaError::DriverException);
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 2) {
            return {0.0, 0.0};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        // OnStep's continuous-motion commands run at a single
        // firmware-selected rate (pinned to max at connect — see
        // select_max_slew_rate()), not an arbitrary requested deg/s value.
        // The advertised range therefore covers what MoveAxis will accept
        // without throwing, not a literal achievable-velocity curve.
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            return {};  // Tertiary axis unsupported.
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        StateLock lock(lock_gate_);
        check_connected();
        check_not_parked_locked("AbortSlew", /*stop=*/true);
        // #2296: a watched slew outlives the supersede until the tracking decision below, so a GOTO the mount already
        // stopped at a limit, which this call's :GU# read is the first to see, gets no restore. The abort is a new
        // motion command: on every way out it drops the watch and the failure that read kept. A failed stop drops it
        // too: the driver cannot tell whether a :Q# that failed reached the mount (send_command_blind() writes it and
        // then drains, and a hangup in the drain fails a :Q# the controller took), so a kept watch could judge a GOTO
        // the mount did abort, and on OnStep 4.x an older held stop code would report the client's own abort as a
        // limit stop. A link lost before the call fails check_connected() above, before the watch is touched, so that
        // watch is kept for the relink, which stops the motion and drops the watch unless a park is still running
        // (#2406).
        const std::optional<WatchedMotion> watched_slew =
            watched_motion_ && watched_motion_->motion == MotionKind::Slew ? watched_motion_ : std::nullopt;
        struct DropWatch {  // runs while `lock` (declared first) still holds mutex_
            const OnStepTelescopeDriver* driver;
            ~DropWatch() {
                driver->watched_motion_.reset();
                driver->motion_error_.reset();
            }
        } drop_watch{this};
        cancel_motion_locked(util::MotionOwner::kBothAxes);  // also ends a Park or FindHome still finishing (#1430)
        watched_motion_ = watched_slew;
        begin_motion_command_locked();
        // Every stop on its own, so one lost command cannot skip the others (#781).
        const std::string stop_error = send_stops_locked(true, {0, 1, 2, 3});
        if (!stop_error.empty()) {
            // The mount may still be moving: leave Slewing as it was, but
            // stop re-sending a MoveAxis the client asked to end.
            manual_axis_resend_[0] = false;
            manual_axis_resend_[1] = false;
            throw AlpacaException("AbortSlew stop failed: " + stop_error, AlpacaError::DriverException);
        }
        slew_hold_.clear();
        finish_slew_claim_locked();  // #2522: the stops went out, so PulseGuide is accepted again
        clear_manual_motion_locked();
        clear_pulse_guides_locked();    // :Q# ends every guide, a pulse guide included (Guide::stop())
        // #2406 (#2396 AB-4): a stop the mount did not hear counted as done. The status poll now checks it: while
        // :GU# still shows motion it sends the stops again (check_abort_stop_locked()).
        abort_stop_rounds_ = 1;
        abort_next_round_ = clock_.now() + kAbortStopRound;
        drop_poll_in_flight_locked();   // a tick that sampled the motion before the stops is dropped
        // #2241: and the rate-offset model with it.
        drop_offset_model_locked();
        // ASCOM: after AbortSlew, tracking returns to its state before the
        // slew. Decide now instead of at a later slew end the driver may never
        // see (#1230). Only an aborted GOTO costs the extra :GU#. A latched
        // fault gives the restore up without asking (the call refuses below).
        if (tracking_before_slew_) {
            try {
                if (link_faulted()) {
                    throw AlpacaException("faulted", AlpacaError::DriverException);
                }
                {
                    judging_abort_ = true;
                    struct EndJudgingAbort {
                        const OnStepTelescopeDriver* driver;
                        ~EndJudgingAbort() { driver->judging_abort_ = false; }
                    } end_judging_abort{this};
                    refresh_status_locked();
                }
                // #2296: OnStepX stops a GOTO at a limit through goTo.abort(), so the GOTO flag stays set while it
                // brakes (GG_READY_ABORT), and turns tracking off; a stop code in this read means no restore, GOTO
                // flag or not (the abort is the client's own stop, so nothing fails).
                if (onstepx_ && limit_stop_reason(frame_status_locked().general_error) != nullptr) {
                    tracking_before_slew_ = false;
                }
                finish_slew_tracking_from_frame_locked();
            } catch (const AlpacaException& ex) {
                tracking_before_slew_ = false;  // status unknown: never restart blind
                if (ex.error_code() == AlpacaError::NotConnected) {
                    throw;  // the link is gone: reported, not swallowed (#1783)
                }
            } catch (...) {
                tracking_before_slew_ = false;
            }
        }
        // The stops went out, but a silent mount may not have heard them
        // (#1414). Checked after the tracking decision, so a faulted abort
        // still gives up the restore instead of leaving it for a later slew end.
        refuse_while_faulted_locked("AbortSlew");
    }

    // An Alt/Az slew is the opposite of an equatorial one (open-astro#1274): ITelescopeV4
    // refuses it while Tracking is true (#1377), since a tracking mount drifts off the
    // requested Alt/Az. It converts to RA/Dec and runs the same GOTO.
    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        util::check_alt_az_slew(kCommandLimits, altitude, azimuth, "SlewToAltAz", GateChecks{*this, "SlewToAltAz"});
        alt_az_to_ra_dec_locked(altitude, azimuth, ra, dec);
        start_slew_locked(ra, dec, "SlewToAltAz", targets);
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        util::check_alt_az_slew(kCommandLimits, altitude, azimuth, "SlewToAltAzAsync",
                                GateChecks{*this, "SlewToAltAzAsync"});
        alt_az_to_ra_dec_locked(altitude, azimuth, ra, dec);
        start_slew_locked(ra, dec, "SlewToAltAzAsync", targets);
    }

    // Converts to RA/Dec as SlewToAltAz does and takes the SyncToCoordinates path. ITelescopeV4 lets a driver refuse
    // it while tracking ("may throw"); a plate-solve sync runs while the mount tracks, so Tracking is not checked.
    void sync_to_alt_az(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        util::check_alt_az_sync(kCommandLimits, altitude, azimuth, "SyncToAltAz", GateChecks{*this, "SyncToAltAz"});
        alt_az_to_ra_dec_locked(altitude, azimuth, ra, dec);
        sync_to_mount_locked(ra, dec, "SyncToAltAz", targets);
    }

    void set_park_home_before_start_for_testing(std::function<void()> hook) {
        StateLock lock(lock_gate_);
        park_home_before_start_ = std::move(hook);
    }

    void set_move_axis_resend_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        move_axis_resend_.set_spawn_for_testing(std::move(spawn));
    }

    void set_move_axis_cap_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        move_axis_cap_[0].set_spawn_for_testing(spawn);
        move_axis_cap_[1].set_spawn_for_testing(std::move(spawn));
    }

    void set_park_home_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        park_home_.set_spawn_for_testing(std::move(spawn));
    }

    void set_status_poll_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        polled_.set_spawn_for_testing(std::move(spawn));
    }

    void set_disconnect_before_close_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        disconnect_before_close_hook_ = std::move(hook);
    }

    void set_move_axis_resend_before_send_hook_for_testing(std::function<bool()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        move_axis_resend_before_send_hook_ = std::move(hook);
    }

    void set_move_axis_send_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        move_axis_send_hook_ = std::move(hook);
    }

    void set_move_axis_cap_before_start_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        move_axis_cap_before_start_hook_ = std::move(hook);
    }

    void set_connect_before_state_lock_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        connect_before_state_lock_hook_ = std::move(hook);
    }

    void set_clock_resync_before_lock_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        clock_resync_before_lock_hook_ = std::move(hook);
    }

    void set_connected_before_transition_unlock_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        connected_before_transition_unlock_hook_ = std::move(hook);
    }

    bool clock_resync_running_for_testing() const { return clock_resync_.running(); }

    // Stops (true: the poll body is joined, so it parks no waiter on a fake task clock) or resumes (false: its
    // first tick one period away) the status poll. While paused a connect does not start it. Call with no driver
    // lock held.
    void set_status_poll_paused_for_testing(bool paused) {
        status_poll_paused_for_testing_ = paused;
        if (paused) {
            stop_status_poller();
        } else if (published_connected_.load()) {
            polled_.restart_poll_for_testing();
            poll_started_ = true;
        }
    }

    // One PolledState cycle on the calling thread, paused or not (`full`: Alt/Az and the site retry too), the
    // module's fault count and loss close included. False when the connection is gone or lost.
    bool poll_status_now_for_testing(bool full) { return poll_status_tick_for_testing(full ? 0 : 1); }
    bool poll_status_tick_for_testing(int tick) {
        forced_poll_tick() = tick;
        polled_.poll_now_for_testing();
        forced_poll_tick().reset();
        return connected_.load() && published_connected_.load() && link_alive();
    }

    // False once the poll body is cancelled (a disconnect has taken it), while it may still be sampling.
    bool status_poll_running_for_testing() const { return poll_started_.load() && !poll_cancelled_.load(); }

    void set_status_poll_after_sample_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        status_poll_after_sample_hook_ = std::move(hook);
    }

    // False once the pulse-end body has returned (or none was started).
    bool pulse_measurement_running_for_testing() const { return pulse_measurement_.running(); }

    void set_pulse_guide_gap_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        pulse_guide_gap_hook_ = std::move(hook);
    }

private:
    // Test seam: runs in the re-send tick after its reads, under mutex_ and io_mutex_; true makes the tick
    // end the move there, as a cancel landing between the reads and the re-send would.
    bool run_move_axis_resend_before_send_hook() const {
        std::function<bool()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = move_axis_resend_before_send_hook_;
        }
        return hook && hook();
    }

    // Test seam (#2651): runs in move_axis() under the state and operation locks, right before a move's rate command.
    void run_move_axis_send_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = move_axis_send_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam (#2651): runs in move_axis()'s unlocked gap, before start_move_axis_cap().
    void run_move_axis_cap_before_start_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = move_axis_cap_before_start_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam: runs in a poll tick between its sample and its publish, with no driver lock held.
    void run_status_poll_after_sample_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = status_poll_after_sample_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam: runs in pulse_guide() after it unlocks and before the pulse-end body starts, with no driver lock
    // held: the gap in which an earlier pulse's body can take the operation lock.
    void run_pulse_guide_gap_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = pulse_guide_gap_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam: runs in a clean disconnect's close window, with mutex_ released, before the close.
    // set_connected()'s seam between the pre-lock link check and the StateLock: a test lands a link loss there.
    void run_connect_before_state_lock_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = connect_before_state_lock_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // set_connected()'s seam as it returns, with transition_mutex_ alone held: a test lands a host clock change
    // there. Runs in a destructor, so nothing it throws gets out.
    void run_connected_before_transition_unlock_hook() const noexcept {
        try {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
                hook = connected_before_transition_unlock_hook_;
            }
            if (hook) {
                hook();
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): a test seam must not end the process
        }
    }

    void run_disconnect_before_close_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = disconnect_before_close_hook_;
        }
        if (hook) {
            hook();
        }
    }

    void check_connected() const {
        // A connect in progress is not a connection to a client: until it has finished (published_connected_)
        // a getter reads NotConnected instead of waiting for it, as Connected does (#910). The connect's own
        // thread passes on connected_, since it holds the operation lock, which no getter holds (decision 0016).
        if (!connected_ || (!holds_io_lock() && !published_connected_.load())) {
            throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
        }
        // A removed or re-enumerated adapter is NotConnected, not a generic
        // send failure while Connected still reads true (#1241).
        if (!link_alive()) {  // no I/O
            const std::string reason = link_lost_reason();
            throw AlpacaException("OnStep link lost" + (reason.empty() ? std::string{} : " (" + reason + ")") +
                                      "; set Connected true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // Ends the driver's record of a MoveAxis and its re-send (#1378).
    // cancel() may be called with mutex_ held (async_operation.h rule 10).
    // Slews, Park, FindHome and AbortSlew end the re-send here with cancel() rather
    // than a supersede (decision 0006 rule 3): safe, because the body checks
    // ctx.stop_reason() under mutex_ before its tick starts, and those commands
    // hold io_mutex_, so none of them lands inside a tick (decision 0016). A
    // Slewing read that finds the move ended in the polled frame calls this
    // with mutex_ alone, which a tick drops for each exchange: the tick
    // re-checks the stop reason and each axis's re-send flag under mutex_
    // right before it re-sends (resend_tick_locked()).
    // Every pulse guide ended (a :Q#, a connect or a disconnect): both axes' windows cleared (decision 0022 rule 2).
    void clear_pulse_guides_locked() const {
        for (int axis = 0; axis < 2; ++axis) {
            pulse_guiding_active_[axis] = false;
            pulse_hold_[axis].clear();
        }
    }

    void clear_manual_motion_locked() const {
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        manual_axis_resend_[0] = false;
        manual_axis_resend_[1] = false;
        move_axis_resend_.cancel();
        move_axis_cap_[0].cancel();
        move_axis_cap_[1].cancel();
        move_axis_cap_window_[0].clear();
        move_axis_cap_window_[1].clear();
    }

    // :GU# says the firmware ended the running MoveAxis (#1378): no 'g',
    // from firmware that has shown 'g' since this connect. 'N' covers a GOTO
    // only. Firmware that never sends 'g' never ends it here, and a pulse
    // guide ('G') hides 'g' while it runs, so it is no evidence either. Only
    // a :GU# read after the move was sent counts (the firmware runs commands
    // in order), from the poll or a control path: the write-throughs of the
    // setters (Tracking, park, unpark, FindHome) keep its older 'g'.
    bool manual_move_ended_locked(const MountStatus& status) const {
        if (!status_read_since_move_ || status.is_pulse_guide_active) {
            return false;
        }
        if (status.is_guide_active) {
            firmware_reports_guide_move_ = true;
            return false;
        }
        return firmware_reports_guide_move_;
    }

    // open-astro#1378: OnStepX stops an equatorial mount at the horizon and
    // overhead limits only on the crossing (Limits::poll calls stop() when the
    // error turns on), and Guide::validate skips limits.isError() for a custom
    // rate, so a re-send after that crossing, or during the braking that
    // follows it, would carry the mount past the limit. Re-send only with the
    // altitude at least kAltitudeLimitMarginDeg inside both limits. The margin
    // covers the whole-degree :Gh#/:Go# replies and the pointing model. A
    // failed or out-of-range read sends nothing. The meridian, axis and switch
    // limits need no check here: OnStepX enforces them on every poll and
    // refuses a move further out (Guide::validAxis1/validAxis2).
    bool resend_within_altitude_limits_locked(const Wire& protocol) const {
        double altitude = 0.0;
        AltitudeLimits limits;
        try {
            altitude = protocol.get_alt_az().altitude_degrees;
            limits = protocol.get_altitude_limits();
        } catch (const std::exception& ex) {
            ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send skipped, altitude read failed: ") + ex.what());
            return false;
        }
        if (limits.horizon_degrees < -30.0 || limits.horizon_degrees > 30.0 || limits.overhead_degrees < 60.0 ||
            limits.overhead_degrees > 90.0) {
            ALPACA_LOG_WARN("OnStep", "MoveAxis re-send skipped, altitude limits out of range");
            return false;
        }
        const bool overhead_on = limits.overhead_degrees < 90.0;
        if (altitude < limits.horizon_degrees + kAltitudeLimitMarginDeg ||
            (overhead_on && altitude > limits.overhead_degrees - kAltitudeLimitMarginDeg)) {
            char text[160];
            std::snprintf(
                text, sizeof(text),
                "MoveAxis not re-sent: altitude %.2f deg is within %.0f deg of, or past, a limit (horizon %.0f, "
                "overhead %.0f); the firmware ends the move",
                altitude, kAltitudeLimitMarginDeg, limits.horizon_degrees, limits.overhead_degrees);
            ALPACA_LOG_INFO("OnStep", text);  // a client can cause it: decision 0007 rule 9
            return false;
        }
        return true;
    }

    // open-astro#1378: OnStepX ends a :Mn#/:Ms#/:Me#/:Mw# move after
    // GUIDE_TIME_LIMIT (default 10 s, 1-120 s or 0 for none at firmware
    // build; Guide.command.cpp), but ASCOM runs a MoveAxis until the client
    // sends rate 0. Each repeat of the move restarts that timer
    // (Guide::startAxis1), so re-send every running move while one is set,
    // every 7 s by default: inside the 10 s default, and seldom, because a
    // move command that lands while an axis brakes after a stop restarts it
    // (Axis::autoSlew; 'g' stays set for about 0.4 s of braking at 0.5 deg/s
    // on the HM-17). Each tick reads :GU# first and re-sends only while the
    // mount shows the move ('g') in that read, so a stop from a hand
    // controller or another app is undone only when the tick lands in that
    // braking. Near the horizon and overhead limits nothing is re-sent (see
    // resend_within_altitude_limits_locked()). A failed read skips the tick.
    // A lost link ends the re-send; the #1241 path handles the link and the
    // firmware limit stops the mount, as before.
    void start_move_axis_resend() {
        move_axis_resend_.start([this](util::OperationContext& ctx) {
            const auto wants_resend = [this](int axis) {
                return manual_axis_slewing_[axis] && manual_axis_resend_[axis];
            };
            for (;;) {
                // Each axis is due on its own clock, as OnStepX times each
                // axis on its own: a MoveAxis on the other axis replaces this
                // body but must not push this axis's re-send back.
                std::chrono::steady_clock::duration wait{};
                {
                    // Reads the due times only: the state lock, no operation lock.
                    StateLock lock(lock_gate_, StateLock::Mode::State);
                    if (ctx.stop_reason() != util::StopReason::None || !connected_) {
                        return;
                    }
                    if (!wants_resend(0) && !wants_resend(1)) {
                        return;  // stopped or capped, even if the stop failed: nothing left to keep going
                    }
                    auto earliest = std::chrono::steady_clock::time_point::max();
                    for (int axis = 0; axis < 2; ++axis) {
                        if (wants_resend(axis)) {
                            earliest = std::min(earliest, manual_axis_resend_due_[axis]);
                        }
                    }
                    wait = std::max(earliest - clock_.now(), std::chrono::steady_clock::duration::zero());
                }
                if (!ctx.wait_for(wait)) {
                    return;
                }
                // The tick holds the operation lock across its serial exchanges
                // (:GU#, the altitude reads, the re-send) and drops mutex_ for
                // each (decision 0016, #1449), so a getter never waits for it.
                StateLock lock(lock_gate_);
                if (ctx.stop_reason() != util::StopReason::None || !connected_) {
                    return;
                }
                const auto now = clock_.now();
                bool due[2] = {false, false};
                for (int axis = 0; axis < 2; ++axis) {
                    due[axis] = wants_resend(axis) && manual_axis_resend_due_[axis] <= now;
                }
                if (!due[0] && !due[1]) {
                    continue;  // a newer MoveAxis moved the due time
                }
                const bool keep_going = resend_tick_locked(ctx, due);
                // Due again a full interval after this tick ends, whatever it
                // did: a skipped tick never spins, and mutex_ is free between
                // ticks. An axis capped above has left `due` (its re-send
                // flag is off), so its re-send is never waited on again.
                const auto next = clock_.now() + kMoveAxisResendInterval;
                for (int axis = 0; axis < 2; ++axis) {
                    if (due[axis]) {
                        manual_axis_resend_due_[axis] = next;
                    }
                }
                if (!keep_going) {
                    return;
                }
            }
        });
    }

    // ── The MoveAxis cap (#2406, #2651) ──────────────────────────────────────────────────────────────────────────
    // A non-zero MoveAxis starts a body in its own axis's slot (move_axis_cap_[axis]) that waits through its context
    // until the 60 s window the move armed has passed, then runs util::run_motion_deadline()'s verified stop of that
    // axis and keeps the outcome for Slewing. The re-send body never sees the cap, so a firmware that sends no 'g', a
    // lapsed move or a re-send that ended does not lift it. A renewal supersedes the body, MoveAxis(axis, 0), AbortSlew
    // and every motion command that clears the manual motion cancel it, and a disconnect joins it.

    // Starts the cap body of `axis` for the window its latest move armed. Call with no driver lock held (decision 0006
    // rule 4). A start the slot refuses throws, and the caller stops the axis (fail closed). Two MoveAxis calls on one
    // axis reach here after their locks are released, in either order: each starts the body of the axis's latest
    // window, read under the axis's start lock, so an older call can never replace the newer move's cap with its own
    // earlier deadline; a window a stop, AbortSlew or a disconnect cleared starts nothing (#2651 review).
    void start_move_axis_cap(int axis) {
        const std::lock_guard<std::mutex> start_lock(move_axis_cap_start_mutex_[axis]);
        util::HoldWindow window;
        {
            StateLock lock(lock_gate_, StateLock::Mode::State);
            window = move_axis_cap_window_[axis];
            if (!connected_ || window.until() == util::TaskClock::clock::time_point::min()) {
                return;  // a stop, AbortSlew or a disconnect cleared the move this call started
            }
        }
        move_axis_cap_[axis].start(
            [this, axis, window](util::OperationContext& ctx) { run_move_axis_cap(axis, window, ctx); });
    }

    static std::string move_axis_cap_ran_text(int axis) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(util::kMaxMoveAxisDuration).count();
        return "MoveAxis on axis " + std::to_string(axis) + " ran " + std::to_string(seconds) +
               " s, the longest allowed";
    }

    void run_move_axis_cap(int axis, util::HoldWindow window, util::OperationContext& ctx) {
        const std::string expired = move_axis_cap_ran_text(axis) + "; the driver stopped it";
        try {
            util::run_motion_deadline(ctx, clock_, window, move_axis_cap_hooks(axis, ctx), expired);
        } catch (const AlpacaException& ex) {
            keep_move_axis_cap_outcome(axis, ctx, expired, ex);
        }
    }

    // True while the cap body of `axis` still owns its move: connected, not cancelled or superseded, and the axis
    // still jogging. Caller holds mutex_.
    bool move_axis_cap_owns_locked(int axis, const util::OperationContext& ctx) const {
        return connected_ && ctx.stop_reason() == util::StopReason::None && manual_axis_slewing_[axis];
    }

    // The hooks of one axis's verified stop. Each takes the operation lock for its own exchange and holds none between
    // them (decision 0016). The stop is MoveAxis(axis, 0)'s own (stop_axis_locked()): it drops the axis's re-send
    // first, so a stop the mount ignores is never undone by a re-send. The verification reads that axis alone: :GU#
    // is the whole mount's, so it reads as stopped while the other axis jogs, and the position is the axis's own
    // coordinate (azimuth and altitude on an alt-az mount, RA and Dec otherwise) with the other field pinned.
    util::StopVerifyHooks move_axis_cap_hooks(int axis, util::OperationContext& ctx) {
        util::StopVerifyHooks hooks;
        hooks.send_stop = [this, axis, &ctx] {
            StateLock lock(lock_gate_);
            // A stop that a newer command or a disconnect cancelled sends nothing: the initiator cancels the slot
            // while it holds this lock (decision 0006 rule 6), so the check inside the lock is the order on the wire.
            if (!move_axis_cap_owns_locked(axis, ctx)) {
                return;
            }
            const std::string stop_error = stop_axis_locked(axis);
            if (!stop_error.empty()) {
                throw AlpacaException(stop_error, AlpacaError::DriverException);
            }
        };
        hooks.read_status = [this, axis]() -> std::optional<util::MotionStatusReading> {
            StateLock lock(lock_gate_);
            if (!connected_) return std::nullopt;
            if (manual_axis_slewing_[1 - axis]) {
                return util::MotionStatusReading{false,
                                                 clock_.now()};  // the other axis's jog is not this axis's motion
            }
            const MountStatus status = wire().get_status();
            return util::MotionStatusReading{status.is_guide_active || status.is_slewing, clock_.now()};
        };
        hooks.read_position = [this, axis]() -> std::optional<util::PositionReading> {
            StateLock lock(lock_gate_);
            if (!connected_) return std::nullopt;
            const bool alt_az =
                mount_type_seen_ && (*mount_type_seen_ == MountType::AltAz || *mount_type_seen_ == MountType::AltAlt);
            const Wire protocol = wire();
            // The axis's coordinate: axis 0 reports it as hours (the field axis_miss_arcsec scales back by 15).
            double first = 0.0;
            double second = 0.0;
            if (alt_az) {
                // TODO: near the zenith the azimuth of a tracking alt-az mount moves faster than StillnessCheck's
                // tolerance, so an axis-0 cap stop can read as not verified; the check needs a tracking-rate allowance.
                const AltAz altaz = protocol.get_alt_az();
                first = util::wrap_degrees(altaz.azimuth_degrees) / 15.0;
                second = altaz.altitude_degrees;
            } else {
                const Position position = protocol.get_position();
                first = position.ra_hours;
                second = position.dec_degrees;
            }
            const util::MountPosition position{axis == 0 ? first : 0.0, axis == 0 ? 0.0 : second};
            return util::PositionReading{position, clock_.now()};
        };
        return hooks;
    }

    // What the cap kept, only while the body still owns its move: the stop verified, so the axis stands still and
    // Slewing raises the text until the next motion command (a client can cause it: INFO, decision 0007 rule 9); or
    // the stop did not verify, so the flag stays and Slewing raises the failure while the axis may still move (#742;
    // WARN). The axis is never re-sent afterwards: the stop dropped its re-send flag.
    void keep_move_axis_cap_outcome(int axis, const util::OperationContext& ctx, const std::string& expired,
                                    const AlpacaException& ex) {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!move_axis_cap_owns_locked(axis, ctx)) {
            return;  // a newer command, a stop, AbortSlew or a disconnect owns the axis
        }
        if (ex.what() != expired) {
            bound_error_ = move_axis_cap_ran_text(axis) + "; the stop failed: " + ex.what();
            ALPACA_LOG_WARN("OnStep", bound_error_);
            return;
        }
        bound_error_ = expired;
        manual_axis_slewing_[axis] = false;
        ALPACA_LOG_INFO("OnStep", bound_error_);
        if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
            clear_manual_motion_locked();  // cancel() from its own body is allowed
        }
    }

    // Cancel and join both cap bodies. Without the driver locks held (the bodies take them); under transition_mutex_
    // or in the destructor.
    void stop_move_axis_caps() {
        move_axis_cap_[0].cancel_all_and_join();
        move_axis_cap_[1].cancel_all_and_join();
    }

    // One re-send tick for the axes in `due` (#1378). False ends the body.
    bool resend_tick_locked(const util::OperationContext& ctx, const bool (&due)[2]) {
        const Wire protocol = wire();
        if (!protocol.link_alive()) {
            return false;
        }
        MountStatus status;
        try {
            status = protocol.get_status();
        } catch (const std::exception& ex) {
            ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send skipped, status read failed: ") + ex.what());
            return true;
        }
        note_status_read_locked(status, true);  // a control-path read, written through (decision 0010)
        if (manual_move_ended_locked(status)) {
            clear_manual_motion_locked();  // cancel() from its own body is allowed
            return false;
        }
        // Re-send only a move the mount shows now. A move the firmware let
        // lapse stays ended: its time limit, or the 5 s GUIDE_TIME_NO_LIMITS
        // cap while its soft limits are off (Guide::finalizeTimeLimit), which
        // asks for a fresh command on purpose. Firmware without 'g' gets no
        // re-send, and a pulse guide ('G') skips the tick.
        if (!status.is_guide_active) {
            // No 'g' since this connect and no pulse guide hiding it: firmware
            // without the letter, or a move that lapsed before this tick. The
            // body ends instead of reading :GU# every 7 s for nothing (#1461);
            // Slewing stays true until the client stops.
            // (After 'g' was seen, a read without it and without 'G' already
            // ended the move above.)
            return status.is_pulse_guide_active;
        }
        if (!resend_within_altitude_limits_locked(protocol)) {
            return true;  // the firmware ends the move; 'g' then ends it here
        }
        if (run_move_axis_resend_before_send_hook()) {
            clear_manual_motion_locked();  // test seam: a cancel that landed in the gap
        }
        for (int axis = 0; axis < 2; ++axis) {
            // Re-checked under mutex_ after the reads above, which dropped it (decision 0006 rule 3):
            // a cancel, or a stop that cleared this axis's re-send, may have landed meanwhile.
            if (ctx.stop_reason() != util::StopReason::None) {
                return false;
            }
            if (!due[axis] || !manual_axis_resend_[axis]) {
                continue;
            }
            try {
                protocol.move_axis_start(manual_axis_dir_[axis], manual_axis_rate_[axis]);
            } catch (const std::exception& ex) {
                ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send failed: ") + ex.what());
            }
        }
        return true;
    }

    // open-astro#1430: OnStepX refuses a park or a home while a guide move
    // (a MoveAxis, or its braking), a pulse guide, a GOTO or a home runs
    // (Park::request, Home::request: CE_SLEW_IN_MOTION). Park and FindHome
    // are asynchronous initiators (the 1 s STANDARD target), so they only do
    // what is quick here: when that motion runs they send AbortSlew's stops
    // (a stop that cannot be sent throws), and on a still mount they send the
    // park or home and read its answer at once, so a refusal a retry cannot
    // change (no park position set, outside limits) throws from the call.
    // The rest runs in park_home_ with both driver locks released between steps: wait
    // until :GU# shows the mount still (no 'g', 'G', GOTO or 'h'; up to
    // kStopSettleTimeout), then send and retry "in motion" refusals (up to
    // kInMotionRetryWindow). A refusal there is kept and raised by Slewing,
    // and by AtPark or AtHome, until the next motion command or a reconnect.
    // A newer motion command's claim or stop (claim_motion_locked(), cancel_motion_locked()) ends the body
    // without sending anything. Park and home end with tracking off, so no
    // tracking restore is kept (#1218). The stops, the :GU# polls, :hP# and
    // :GE# (up to 1 s each without a reply; OnStep 4.x can take that long) and
    // :hC# hold the operation lock, so no other command lands between :hP# and
    // the :GE# that explains it, and run with mutex_ free (decision 0016, #1449):
    // a getter answers meanwhile.
    void start_park_or_home_locked(StateLock& lock, const char* who, bool home) {
        // #2522: both axes, superseding whatever ran on them (an earlier Park or FindHome too, #1430). Given up on
        // the way out unless the mount took it or the body below was handed it.
        const util::MotionClaim claim = claim_motion_locked(util::MotionCommand::Slew);
        slew_claim_ = SlewClaim{claim, home ? MotionKind::Home : MotionKind::Park, false};
        UntakenSlewClaim give_up{this, claim};
        const bool stopped = stop_motion_before_park_or_home_locked(who);
        int code = -1;
        if (!stopped) {
            const ParkHomeTry tried = try_park_or_home_locked(who, home, code);
            if (tried == ParkHomeTry::Taken) {
                finish_park_or_home_locked(home);
                return;
            }
            if (tried == ParkHomeTry::AlreadyParked) {
                return;
            }
        }
        // Decision 0006 rule 3: claimed under mutex_ before the unlock, so a
        // newer motion command in the gap before start() still ends this one,
        // and Slewing reads true from here on. The body runs whatever request
        // is current when it starts, so a newer Park or FindHome whose start()
        // lands first is still carried out by the body started last.
        park_home_request_ = ParkHomeRequest{claim, who, home, stopped, code};
        give_up.handed_off = true;  // the body ends it, or the refusal below
        // Test seam, read under mutex_.
        const auto before_start = park_home_before_start_;
        lock.unlock();  // start() without mutex_: decision 0006 rule 4
        if (before_start) {
            before_start();  // lands a call in the gap
        }
        try {
            park_home_.start([this](util::OperationContext& ctx) { run_park_or_home(ctx); });
        } catch (const AlpacaException&) {
            lock.lock();
            if (park_home_request_ && same_claim(park_home_request_->claim, claim)) {
                park_home_request_.reset();  // no body will finish it
                release_untaken_slew_claim_locked(claim);
            }
            throw;
        }
    }

    bool moving_locked() const {
        const MountStatus status = frame_status_locked();
        return manual_axis_slewing_[0] || manual_axis_slewing_[1] || status.is_slewing || status.is_guide_active ||
               status.is_pulse_guide_active || status.is_homing;
    }

    // Sends AbortSlew's stops when the mount moves: true when it did.
    bool stop_motion_before_park_or_home_locked(const char* who) {
        if (!moving_locked()) {
            return false;
        }
        const std::string stop_error = send_stops_locked(true, {0, 1, 2, 3});
        if (!stop_error.empty()) {
            // The mount may still be moving: leave Slewing as it was, but stop
            // re-sending a MoveAxis, as AbortSlew does (#781).
            manual_axis_resend_[0] = false;
            manual_axis_resend_[1] = false;
            throw AlpacaException(std::string(who) + ": could not stop the running motion first: " + stop_error,
                                  AlpacaError::DriverException);
        }
        clear_manual_motion_locked();
        clear_pulse_guides_locked();  // AbortSlew's stops end every guide, a pulse guide included
        slew_hold_.clear();
        tracking_before_slew_ = false;
        return true;
    }

    // One park (:hP#, '1' or '0') or home (:hC#, no reply) try, with :GE#
    // read right after it, before any other command replaces the code
    // (OnStepX and OnStep 4.x have it). True when the mount took it. A
    // refusal a retry may change sets `code` (22 "in motion", or -1 for a
    // park on firmware without :GE#, whose reason is unknown) and returns
    // false; any other refusal throws InvalidOperation. Without a code a
    // home counts as started, as before. A park the mount answers "already
    // parked" (9) is ASCOM's no-op and a home it answers "mount parked" (18)
    // is InvalidWhileParked, as when the call's :GU# read shows it parked: OnStep
    // 4.x (Park.ino park(), Home.ino goHome()) gives them when it parked after that read.
    enum class ParkHomeTry : std::uint8_t { Taken, AlreadyParked, Retry };
    ParkHomeTry try_park_or_home_locked(const char* who, bool home, int& code) {
        const Wire protocol = wire();
        // :hP# or :hC#, then :GE#, with no sibling device's exchange between them: it would replace the code (#2223).
        const auto sequence = protocol.hold_sequence();
        if (home) {
            protocol.find_home();
            code = last_command_error_locked(who).value_or(kCeNone);
        } else {
            if (protocol.park()) {
                return ParkHomeTry::Taken;
            }
            code = last_command_error_locked(who).value_or(-1);
        }
        if (code == kCeNone || code == kCeTrue) {
            return ParkHomeTry::Taken;  // for a park: the reply was lost or garbled, but the mount took it
        }
        if (!home && code == kCeParked) {
            write_status_locked([](MountStatus& status) {  // the mount says so; AtPark reads it
                status.is_parked = true;
                status.is_slewing = false;
            });
            try {
                refresh_status_locked();  // best-effort: a failed read does not make the no-op a refusal
            } catch (const AlpacaException& ex) {
                // A latched fault is swallowed here too: the mount has just
                // answered "already parked", so the no-op stands (Park's call
                // refuses a fault before it gets here; its background retries
                // can reach this with one latched). Only a lost link is reported.
                if (ex.error_code() == AlpacaError::NotConnected) {
                    throw;  // the link is gone, as for any read
                }
            }
            return ParkHomeTry::AlreadyParked;
        }
        if (home && code == kCeSlewInPark) {
            throw AlpacaException(std::string(who) + " is not allowed while parked", AlpacaError::InvalidWhileParked);
        }
        if (code != kCeSlewInMotion && !(code < 0 && !home)) {
            throw mount_refused(who, code);
        }
        return ParkHomeTry::Retry;
    }

    // :GE# with no code is firmware without it, or a mount that stopped
    // answering (#1414): a :GU# read tells them apart. A silent mount fails the
    // park or home (the read feeds no fault latch, #2495)
    // instead of reading as accepted. Unlike refresh_status_locked(), the
    // :GU# read here leaves status_read_since_move_ alone on purpose: the #1378
    // 'g' check then waits for the next status read, the safe side, so do not
    // "fix" it without a test for that check.
    std::optional<int> last_command_error_locked(const char* who) {
        const Wire protocol = wire();
        if (const auto code = protocol.get_last_command_error()) {
            return code;
        }
        try {
            const MountStatus status = protocol.get_status();
            note_status_read_locked(status, false);
        } catch (const std::exception& ex) {
            if (!is_link_lost(ex)) {
                throw_if_faulted_locked();  // a control-path read feeds no latch (#2495), but reports one
            }
            if (const auto* alpaca = dynamic_cast<const AlpacaException*>(&ex);
                alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected) {
                throw;  // the link is gone: NotConnected, as for any other call
            }
            throw AlpacaException(std::string(who) + ": the mount did not answer after the command (" + ex.what() + ")",
                                  AlpacaError::DriverException);
        }
        return std::nullopt;
    }

    // A numeric command the mount answered '0' (or garbled): the reason is the :GE#
    // code. As for :hP# (#1430), a :GE# that then says 0 or 25 means it was taken.
    // Returns the refusal code (-1 when :GE# gave none), or nullopt when accepted.
    std::optional<int> refusal_code_locked(const char* who) {
        const int code = last_command_error_locked(who).value_or(-1);
        if (code == kCeNone || code == kCeTrue) {
            return std::nullopt;
        }
        return code;
    }

    static AlpacaException mount_refused(const char* who, int code) {
        return AlpacaException(std::string(who) + " refused by the mount (" +
                                   (code < 0 ? std::string("no reason given") : describe_onstepx_error(code)) + ")",
                               AlpacaError::InvalidOperation);
    }

    // Whether this mount's FindHome centres a tangent arm or a sector gear (:GXEF#, :GXEG#), read at the first FindHome
    // that is not a no-op (at home or homing), before the park-and-home start that can still refuse it, and kept for
    // the session. Neither firmware reports 'H' after such a home (OnStep 4.x
    // MoveTo.ino sets atHome only without a tangent arm; OnStepX Home::request() moves the axis with no home state, and
    // guideDone() keeps its coordinate), so that home is not watched. A '1' from either is enough; otherwise both
    // must answer '0' (a firmware identified as 4.x is not asked :GXEG#; one whose version did not read is). A read
    // that throws, or a reply that is neither digit (a bare read that times out returns what arrived, possibly
    // nothing), leaves it unknown: that home is watched, as before, and the next FindHome reads the flags again. The
    // caller holds the operation lock; the reads drop mutex_. The value is written here and by
    // reset_session_state_locked(), whose callers (a connect, a disconnect, a failed connect) all hold the operation
    // lock too, so nothing writes it while the reads run and it needs no re-check after them.
    void read_home_geometry_locked() {
        if (home_moves_one_axis_) {
            return;
        }
        std::optional<bool> arm;
        std::optional<bool> gear = false;  // OnStep 4.x has no sector gear
        try {
            arm = wire().get_tangent_arm();
            if ((onstepx_ || !firmware_identified_) && arm != true) {
                gear = wire().get_sector_gear();
            }
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_INFO("OnStep", std::string("Tangent-arm and sector-gear flags not read: ") + ex.what());
            return;
        }
        if (arm == true || gear == true) {
            home_moves_one_axis_ = true;
            ALPACA_LOG_INFO("OnStep",
                            "Tangent arm or sector gear: FindHome centres that axis and the mount does not report "
                            "AtHome after it");
        } else if (arm && gear) {
            home_moves_one_axis_ = false;
        } else {
            ALPACA_LOG_INFO("OnStep", "Tangent-arm and sector-gear flags not read: a reply was neither 1 nor 0");
        }
    }

    // The mount took the park or home: it now moves to it with tracking off.
    void finish_park_or_home_locked(bool home) {
        tracking_before_slew_ = false;  // never restart tracking afterwards (#1218)
        if (slew_claim_) {
            slew_claim_->taken = true;  // #2522: held until a status shows the park or home ended
        }
        // #2296: a limit can stop it on the way; before the write-through, from the status read. A home on a mount
        // with a tangent arm or a sector gear ends without 'H' (read_home_geometry_locked()), so it is not watched.
        if (!home || !home_moves_one_axis_.value_or(false)) {
            watch_motion_locked(home ? MotionKind::Home : MotionKind::Park, home ? "FindHome" : "Park");
        }
        // #2241: the mount moves away from the rate-offset model.
        drop_offset_model_locked();
        write_status_locked([home](MountStatus& status) {  // decision 0010 write-through
            status.is_at_home = false;
            status.is_slewing = true;
            if (!home) {
                status.is_parked = false;
            }
        });
        if (!home) {
            clear_manual_motion_locked();
        }
    }

    // The park_home_ body: settle (when the call sent stops), then send and
    // retry. Waits on the task clock with both driver locks released (decision
    // 0005); its exchanges hold the operation lock only (decision 0016).
    void run_park_or_home(util::OperationContext& ctx) {
        StateLock lock(lock_gate_);
        // A body stopped before it runs takes no request: a newer Park or
        // FindHome may have stored its own after cancelling this body and
        // before its start() marked this one superseded, and this body must
        // not end that request (#1430). Leaving it is safe because every way
        // this slot is cancelled also clears or replaces the request
        // (end_park_or_home_locked(), a newer start): keep it so, or a
        // request no body runs leaves Slewing true.
        if (!park_home_request_ || ctx.stop_reason() != util::StopReason::None) {
            return;  // superseded or cancelled, or carried out by an earlier body
        }
        const ParkHomeRequest request = *park_home_request_;
        run_park_or_home_locked(lock, ctx, request.who, request.home, request.stopped, request.code, request.claim);
        // Done, unless a newer start() superseded this body: the body it
        // started runs the request. A newer claim already replaced it.
        if (ctx.stop_reason() != util::StopReason::Superseded && park_home_request_ &&
            same_claim(park_home_request_->claim, request.claim)) {
            park_home_request_.reset();
            release_untaken_slew_claim_locked(request.claim);  // #2522: a refusal kept, or a no-op
        }
    }

    // The steps of the body; returns with `lock` held.
    void run_park_or_home_locked(StateLock& lock, util::OperationContext& ctx, const char* who, bool home, bool settle,
                                 int code, const util::MotionClaim& claim) {
        const auto still_ours = [&] {
            return ctx.stop_reason() == util::StopReason::None && connected_ &&
                   motion_owner_.stop_reason(claim) == util::StopReason::None;
        };
        const auto wait = [&](std::chrono::milliseconds d) {
            lock.unlock();
            const bool woke = ctx.wait_for(d);
            lock.lock();
            return woke && still_ours();
        };
        try {
            if (!still_ours()) {
                return;
            }
            if (settle) {
                const auto settle_deadline = clock_.now() + kStopSettleTimeout;
                for (;;) {
                    refresh_status_locked();
                    if (!moving_locked() || clock_.now() >= settle_deadline) {
                        break;  // still moving at the deadline: the refusal reports it
                    }
                    if (!wait(kStopSettlePoll)) {
                        return;
                    }
                }
            }
            const auto deadline = clock_.now() + kInMotionRetryWindow;
            bool try_now = settle;
            for (;;) {
                if (!try_now) {
                    if (clock_.now() >= deadline) {
                        throw mount_refused(who, code);
                    }
                    if (!wait(kInMotionRetryPoll)) {
                        return;
                    }
                }
                try_now = false;
                const ParkHomeTry tried = try_park_or_home_locked(who, home, code);
                if (tried == ParkHomeTry::Taken) {
                    finish_park_or_home_locked(home);
                    return;
                }
                if (tried == ParkHomeTry::AlreadyParked) {
                    return;
                }
            }
        } catch (const AlpacaException& ex) {
            if (still_ours()) {
                motion_error_ = KeptMotionError{home ? MotionKind::Home : MotionKind::Park, ex.what(), ex.error_code()};
            }
        }
    }

    // #2522 (util::MotionOwner, decision 0006 rule 3): a motion command claims the axes it moves and a stop cancels
    // them, under mutex_ before either sets its flags. Either is a new motion command: a Park or FindHome still
    // finishing ends without sending anything, and its kept failure is dropped, as is the watch on a motion the
    // driver started (#1430, #2296). Each call ends the Park or FindHome: a slew, Park or FindHome owns both axes, so
    // any claim or stop reaches it. cancel() never blocks, so mutex_ may be held (rule 10).
    util::MotionClaim claim_motion_locked(util::MotionCommand command, int axis = 0) {
        const util::MotionClaim claim = motion_owner_.claim(command, axis);
        end_park_or_home_locked();
        return claim;
    }

    void cancel_motion_locked(std::uint8_t axes) {
        motion_owner_.cancel(axes);
        end_park_or_home_locked();
    }

    void end_park_or_home_locked() {
        motion_error_.reset();
        watched_motion_.reset();
        if (park_home_request_) {
            release_untaken_slew_claim_locked(park_home_request_->claim);  // it was never sent
            park_home_request_.reset();
        }
        park_home_.cancel();
    }

    static bool same_claim(const util::MotionClaim& a, const util::MotionClaim& b) {
        return a.generation == b.generation;
    }

    // #2522: the slew, Park or FindHome claim ends, so a PulseGuide is accepted again.
    void finish_slew_claim_locked() const {
        if (slew_claim_) {
            motion_owner_.finished(slew_claim_->claim);
            slew_claim_.reset();
        }
    }

    // #2522: a slew, Park or FindHome the mount never took (refused, a no-op, given up before it was sent) moved
    // nothing, so its claim ends at once.
    void release_untaken_slew_claim_locked(const util::MotionClaim& claim) const {
        if (slew_claim_ && !slew_claim_->taken && same_claim(slew_claim_->claim, claim)) {
            finish_slew_claim_locked();
        }
    }

    // #2522: the controller runs every GOTO, park and home, so no driver body ends one. Its claim ends at the first
    // status after the mount took it that shows it no longer running, read as note_motion_end_locked() reads it: a
    // GOTO with no GOTO, a park with no GOTO and no 'I', a home with no GOTO, no 'h' and no 'g' (a 4.x switch home
    // shows only 'g'). A stop that went out ends it too (AbortSlew), as does a disconnect or relink.
    void end_slew_claim_if_ended_locked(const MountStatus& status) const {
        if (!slew_claim_ || !slew_claim_->taken) {
            return;
        }
        const MotionKind kind = slew_claim_->kind;
        const bool running = status.is_slewing || (kind == MotionKind::Park && status.is_parking) ||
                             (kind == MotionKind::Home && (status.is_homing || status.is_guide_active));
        if (!running) {
            finish_slew_claim_locked();
        }
    }

    // Raises a failure kept for a motion (a refusal the park_home_ body met, or an end note_motion_end_locked()
    // found): any for Slewing, only a park's for AtPark and a home's for AtHome. A park or home past its bound fails
    // here first, so a getter sees the bound without waiting for a poll tick (no I/O: the task clock and mutex_ only).
    void raise_motion_error_locked(std::optional<MotionKind> motion) const {
        expire_watched_motion_locked();
        if (motion_error_ && (!motion || *motion == motion_error_->motion)) {
            throw AlpacaException(motion_error_->message, motion_error_->code);
        }
    }

    // A GOTO, park or home the mount has taken: watched until a status shows it ended (#2296). Only a motion the
    // driver started is watched; a park or slew from a handset or another program is not.
    void watch_motion_locked(MotionKind motion, const char* who) const {
        const auto now = clock_.now();
        const bool fixed_hour_angle = !frame_status_locked().is_tracking;
        watched_motion_ =
            WatchedMotion{motion, who, now, std::nullopt, -1, false, false, now, fixed_hour_angle, park_home_bound_};
    }

    // Decision 0007 rule 9: a client can repeat a GOTO, park or home into a limit that stands, so a stop with a code is
    // logged at WARN once per `key` while that code stands, and every repeat of a key already logged at INFO, until a
    // status shows another code (reset in note_motion_end_locked()). `key` is what the read decided (the failure's
    // reason, or a landing), not the member that started the motion, so a client alternating SlewToCoordinatesAsync
    // and SlewToTargetAsync into the limit gets one WARN. A landing and a stop with the same code are different keys,
    // so a stop after a landing on a 4.x latched code still warns once; every key seen is kept, so a client
    // alternating a landing GOTO with a stopped one, or a park with a GOTO, gets one WARN for each and no more.
    void log_motion_stop_locked(const std::string& message, const std::string& key, int stop_code) const {
        if (stop_code == 0) {
            ALPACA_LOG_WARN("OnStep", message);
            return;
        }
        if (stop_code != warned_stop_code_) {
            warned_stop_code_ = stop_code;
            warned_stop_keys_.clear();
        }
        if (std::find(warned_stop_keys_.begin(), warned_stop_keys_.end(), key) != warned_stop_keys_.end()) {
            ALPACA_LOG_INFO("OnStep", message + " (repeated; logged at WARN when first seen)");
            return;
        }
        warned_stop_keys_.push_back(key);
        ALPACA_LOG_WARN("OnStep", message);
    }

    // Ends the watch with a kept failure: "<who> failed: <why>". `stop_code` is the :GU# general error that stopped
    // it, 0 for none.
    void fail_watched_motion_locked(const std::string& why, int stop_code = 0) const {
        if (!watched_motion_) {
            return;
        }
        const std::string message = std::string(watched_motion_->who) + " failed: " + why;
        if (judging_abort_) {
            // Inside AbortSlew the client stopped the slew itself, and a 4.x latched code cannot tell its stop from a
            // limit's: say only what the read decided. The failure is dropped with the watch.
            ALPACA_LOG_INFO("OnStep", "AbortSlew read the slew's end (" + why + "); no tracking restore");
        } else {
            // WARN once per reason and stop code (log_motion_stop_locked()); the client gets the failure from the
            // property.
            log_motion_stop_locked(message, why, stop_code);
        }
        motion_error_ = KeptMotionError{watched_motion_->motion, message, AlpacaError::DriverException};
        watched_motion_.reset();
    }

    // #2296 (items 5 and 6): a park or home the driver started that has not ended within its bound (WatchedMotion
    // bound, fixed when the command was taken) fails. It only reports: the driver sends no stop, as for its other
    // timed-out waits (the synchronous slew's, the park_home_ settle), so a mount that is still moving is left to the
    // client (AbortSlew) rather than stopped mid-park.
    // The bound relies on the status poll running for as long as the driver is connected, so a park's end frame is
    // read before the bound passes: a change that pauses the poll (while parked, say) must also pause the watch, or a
    // finished park expires as failed.
    void expire_watched_motion_locked() const {
        if (!watched_motion_ || watched_motion_->motion == MotionKind::Slew ||
            clock_.now() - watched_motion_->started_at < watched_motion_->bound) {
            return;
        }
        const bool park = watched_motion_->motion == MotionKind::Park;
        fail_watched_motion_locked(std::string("the mount did not finish the ") + (park ? "park" : "home") +
                                   " within " + std::to_string((watched_motion_->bound.count() + 59) / 60) +
                                   " minutes");
    }

    // " (:GU# general error <n>, <reason>)" for a stop code, else "".
    static std::string stop_code_suffix(int code, const char* reason) {
        return reason == nullptr ? std::string() : " (:GU# general error " + std::to_string(code) + ", " + reason + ")";
    }

    // #2296: the watched motion ends at the first status read after it that shows it ended, and that status decides,
    // the same way on OnStepX and OnStep 4.x:
    // - A slew ends with no GOTO. With no stop code (limit_stop_reason()) it landed, as before. Without a code the end
    //   position is not judged: it would be the only evidence, and a healthy GOTO can end away from the target the
    //   driver sent. A Tracking write during the GOTO (OnStepX takes :Te#/:Td# mid-GOTO) changes whether the firmware
    //   holds the target's RA or its hour angle while the watch keeps the one it had at the command; a tracking
    //   OnStepX GOTO follows the RA and Dec rate offsets on its final approach (Goto.cpp poll()), and the mount moves
    //   on at them until the end frame is read; a relink judges a GOTO the user may have moved on from with the
    //   handset during the outage; and a GOTO another program starts as ours ends extends the watch to its end. So a
    //   handset abort with no code reads as landed. With a code, where the mount is decides, since a stop code alone
    //   is not evidence: OnStepX writes the limits as they stand (a GOTO that lands close to a limit and then tracks
    //   across it shows the code in its first end frame), and OnStep 4.x latches the code until :Te#, an unpark, a
    //   home reset or a reboot (a code from an older stop). Within kStopLandedToleranceDegrees of the target (a flip:
    //   on the requested pier side) it landed, and the log names the code (log_motion_stop_locked()); away from it the
    //   mount stopped short, and
    //   the slew fails. Either way the #1230 tracking restore is not sent: the code says a limit stopped tracking or
    //   is standing, and OnStepX does not limit-check :Te#.
    // - A park ends with no GOTO and no 'I' (both firmwares go from parking to parked or failed in one step: OnStepX
    //   Goto.cpp poll() then Park::requestDone(), OnStep 4.x MoveTo.ino). Parked succeeded. Otherwise it failed: 'F',
    //   a stop code, or, once the park was seen running, an end without either (a handset abort, a park that gave up).
    //   An end with no sign that the park ran is left to the bound: both firmwares show 'I' from the :hP# answer.
    // - A home ends with no GOTO, no 'h' and no 'g'. At home ('H') succeeded. A home that ran as a GOTO and ended
    //   elsewhere failed. A home with switches ('h' or 'g' without a GOTO) is left to the bound: after the switches
    //   OnStepX moves the axes on to the sense offset (up to 180 degrees, Home.cpp guideDone(), Home.command.cpp :hC1)
    //   with nothing in :GU#, and OnStep 4.x has an idle moment between its fast and slow search (Home.ino FH_IDLE).
    //   So is a home not yet seen running.
    //   An OnStepX switch home that fails (a stop, or its 600 s search limit) re-zeroes the axes at home where it
    //   stopped (Home.cpp guideDone(false) -> reset()), so it shows 'H' and reads as reached: nothing the mount sends
    //   tells it apart.
    // Every status read reaches here: the poll's before its restore decision, and the control paths' through
    // note_status_read_locked(). `position` is where the mount was read with the status (the poll's frame); without
    // it a slew that needs it reads :GRa#/:GDe# now (every caller holds the operation lock), and if that fails the
    // watch stays for the next status.
    void note_motion_end_locked(const MountStatus& status, std::optional<Position> position = std::nullopt) const {
        end_slew_claim_if_ended_locked(status);  // #2522
        if (warned_stop_code_ != 0 && status.general_error != warned_stop_code_) {
            // Another code (or none): the next stop with a code is logged at WARN again (log_motion_stop_locked()).
            warned_stop_code_ = 0;
            warned_stop_keys_.clear();
        }
        expire_watched_motion_locked();
        if (!watched_motion_) {
            return;
        }
        WatchedMotion& watched = *watched_motion_;
        const bool park = watched.motion == MotionKind::Park;
        const bool home = watched.motion == MotionKind::Home;
        if (status.is_waiting_at_home) {
            watched.started_at = clock_.now();  // waiting for the user: the bound does not run
        }
        const bool switch_home = home && !status.is_slewing && (status.is_homing || status.is_guide_active);
        if (status.is_slewing || (park && status.is_parking) || switch_home) {
            watched.saw_motion = true;
            watched.saw_switch_home = watched.saw_switch_home || switch_home;
            return;  // still running
        }
        const char* reason = limit_stop_reason(status.general_error);
        const std::string code = stop_code_suffix(status.general_error, reason);
        const int stop_code = reason != nullptr ? status.general_error : 0;
        if (park) {
            if (status.is_parked) {
                watched_motion_.reset();
            } else if (status.is_park_failed) {
                fail_watched_motion_locked("the mount reports that the park failed (:GU# 'F')" + code, stop_code);
            } else if (reason != nullptr || watched.saw_motion) {
                fail_watched_motion_locked("the mount ended the park without parking" + code, stop_code);
            }
            return;
        }
        if (home) {
            if (status.is_at_home) {
                watched_motion_.reset();
            } else if (!watched.saw_switch_home && (reason != nullptr || watched.saw_motion)) {
                fail_watched_motion_locked("the mount ended the home without reaching home" + code, stop_code);
            }
            return;
        }
        if (reason == nullptr) {
            watched_motion_.reset();  // landed
            return;
        }
        if (onstepx_) {
            // OnStepX stops tracking at the horizon and overhead limits only on the crossing (Limits::poll(),
            // `!lastError.altitude.min && error.altitude.min`), so a restore would track on past them: none, landed
            // or not.
            tracking_before_slew_ = false;
        }
        const std::optional<bool> landed = watched_goto_landed_locked(status, position);
        if (!watched_motion_) {
            return;  // superseded while the position was read
        }
        if (landed && *landed) {
            // OnStep 4.x re-checks its limits on every pass and stops tracking again past one (OnStep.ino safety
            // checks), so its restore stays as before.
            log_motion_stop_locked(std::string(watched_motion_->who) + ": the GOTO reached its target" + code +
                                       (onstepx_ ? ", so tracking is not restarted" : ""),
                                   "the GOTO reached its target", stop_code);
            watched_motion_.reset();
            return;
        }
        // Stopped short, or a stop code whose position could not be read: a stop code is never read as a landing
        // without the position that shows it landed. No restore.
        tracking_before_slew_ = false;
        fail_watched_motion_locked(std::string("OnStep stopped the GOTO ") + reason + " (:GU# general error " +
                                       std::to_string(status.general_error) + ")" +
                                       (landed ? "" : "; its position could not be read"),
                                   stop_code);
    }

    // Whether a watched GOTO that ended with a stop code is at its target: within kStopLandedToleranceDegrees of it,
    // and a flip also on the requested pier side when the frame reports one (OnStepX derives the side from axis2
    // alone, so a flip stopped part-way can already show it). A GOTO started with tracking off holds its target's
    // hour angle, so its target RA is advanced by the sidereal time since the command (the Alt/Az slews: the mount sits
    // at the requested Alt/Az while the RA it reports moves on). nullopt when the position cannot be read now.
    // Decision 0016 rule 6: the read drops mutex_, so the watch is re-checked after it (here and in the caller). That
    // it is still there is enough, with no generation: every caller holds the operation lock across the read, as does
    // every write that starts or replaces a watch (watch_motion_locked(), the target writes after a GOTO dispatch and a
    // flip, the relink, AbortSlew, a disconnect), and the only write a getter makes, expire_watched_motion_locked(),
    // ends a park or home watch, never a slew's. So after the read the watch is the one before it, or none.
    std::optional<bool> watched_goto_landed_locked(const MountStatus& status, std::optional<Position> position) const {
        if (!watched_motion_) {
            return std::nullopt;
        }
        const int target_side = watched_motion_->target_side;
        const std::optional<std::pair<double, double>> target = watched_motion_->target;
        const bool fixed_hour_angle = watched_motion_->fixed_hour_angle;
        const auto commanded_at = watched_motion_->commanded_at;
        if (target_side >= 0 && status.side_of_pier >= 0 && status.side_of_pier != target_side) {
            return false;  // a flip that ended on the side it started from
        }
        if (!target) {
            return true;  // no target to compare with: not a stop
        }
        if (!position) {
            try {
                position = wire().get_position();
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    throw;
                }
                ALPACA_LOG_WARN("OnStep", std::string("Position read after a GOTO end failed: ") + ex.what());
                return std::nullopt;
            }
            if (!watched_motion_) {
                return std::nullopt;
            }
        }
        auto [ra, dec] = *target;
        if (fixed_hour_angle) {
            ra += std::chrono::duration<double>(clock_.now() - commanded_at).count() * kSiderealPerSolar / 3600.0;
        }
        return separation_degrees(position->ra_hours, position->dec_degrees, ra, dec) <= kStopLandedToleranceDegrees;
    }

    // AlignmentMode from the mount type last seen (mount_type_seen_), a mount setting kept in driver state by the
    // poll and the control-path reads (decision 0010 OnStep row, #2214). set_connected(false) clears it, so the
    // published_connected_ gate is defence in depth. Caller holds mutex_.
    AlignmentMode alignment_mode_locked() const {
        if (!published_connected_.load()) {
            return AlignmentMode::GermanPolar;
        }
        check_connected();
        // A latched fault throws it, and no :GU# answer since the connect throws DriverException (decisions 0009
        // rules 1 and 3, 0010 rule 8), never a guessed GermanPolar; a status with no type letter throws too.
        throw_if_faulted_locked();
        if (!mount_type_seen_) {
            throw AlpacaException("OnStep mount status is not available: the mount has not answered :GU#",
                                  AlpacaError::DriverException);
        }
        switch (*mount_type_seen_) {
            case MountType::Fork:
                return AlignmentMode::Polar;
            case MountType::AltAz:
            case MountType::AltAlt:
                return AlignmentMode::AltAz;
            case MountType::GermanEquatorial:
                return AlignmentMode::GermanPolar;
            case MountType::Unknown:
                break;
        }
        // OnStepX always writes one of E/K/A/L and OnStep 4.x one of E/K/A, so a status with no letter is not a
        // supported firmware's answer: the mount's answer lacks the field (decision 0010 rule 8).
        throw AlpacaException("OnStep mount type is not available: the :GU# status carries no mount-type letter",
                              AlpacaError::DriverException);
    }

    // CanSetPierSide: OnStepX, a German mount, the meridian flips on and a read preferred side (#2179). Caller holds
    // mutex_.
    bool can_set_pier_side_locked() const {
        if (!published_connected_.load() || !onstepx_ || !pier_settings_ || !pier_settings_->flips_enabled) {
            return false;
        }
        const auto reading = polled_.read();
        return reading && reading->value.status_known && reading->value.status.is_gem;
    }

    // The side OnStepX would pick for a GOTO to (hour_angle, dec) from the polled frame and the settings read from the
    // mount, or nullopt when any of them is unknown or no side reaches the target. Caller holds mutex_; no I/O.
    std::optional<int> modelled_destination_side_locked(double lst_hours, double target_hour_angle,
                                                        double target_dec) const {
        if (!pier_settings_) {
            return std::nullopt;
        }
        const auto reading = polled_.read();
        if (!reading || !reading->value.status_known || !reading->value.equatorial_known ||
            !reading->value.status.is_gem) {
            return std::nullopt;
        }
        const PolledFrame& frame = reading->value;
        const util::GemPierModel model{pier_settings_->model_preference(), pier_settings_->flips_enabled,
                                       pier_settings_->model_limits(), site_latitude_cached_ < 0.0};
        const util::GemPointing current{frame.status.side_of_pier, frame.status.is_at_home,
                                        util::shortest_ra_delta_hours(lst_hours, frame.ra_hours), frame.dec_degrees};
        const int side = util::gem_destination_pier_side(model, current, target_hour_angle, target_dec);
        if (side != 0 && side != 1) {
            return std::nullopt;
        }
        return side;
    }

    // The six pier-side reads (#2179), :GX96# first: a reply that is not a preferred-side letter ends the read, so
    // a firmware or fake that does not know the commands costs one exchange. Returns the settings, or nullopt with
    // `why` set. A read that gets no reply throws. Caller holds the operation lock.
    std::optional<PierSettings> read_pier_settings_locked(std::string& why) const {
        const Wire protocol = wire();
        PierSettings settings;
        const auto letter = protocol.get_preferred_pier_side();
        if (!letter) {
            why = ":GX96# did not answer E, W, B or A";
            return std::nullopt;
        }
        settings.preference = *letter;
        const auto flips = protocol.get_pier_flips_enabled();
        const auto east = protocol.get_meridian_limit_minutes(true);
        const auto west = protocol.get_meridian_limit_minutes(false);
        const auto axis_min = protocol.get_axis1_limit_degrees(false);
        const auto axis_max = protocol.get_axis1_limit_degrees(true);
        if (!flips) {
            why = ":GX94# did not answer \"<n>\" or \"<n> N\"";
            return std::nullopt;
        }
        if (!east || !west) {
            why = ":GXE9#/:GXEA# did not answer a whole number of minutes";
            return std::nullopt;
        }
        if (!axis_min || !axis_max) {
            why = ":GXEe#/:GXEw# did not answer a whole number of degrees";
            return std::nullopt;
        }
        settings.flips_enabled = *flips;
        settings.east_limit_minutes = *east;
        settings.west_limit_minutes = *west;
        settings.axis1_min_degrees = *axis_min;
        settings.axis1_max_degrees = *axis_max;
        return settings;
    }

    // Reads the settings into pier_settings_. At connect a failed or unparseable read leaves them unknown; later
    // (the poll) it keeps the previous ones, and a changed value is logged. A throw is the caller's to judge (a
    // lost link matters; anything else is "unknown"). Caller holds the operation lock.
    void refresh_pier_settings_locked(bool at_connect) {
        std::string why;
        const auto read = read_pier_settings_locked(why);
        if (!read) {
            if (at_connect) {
                ALPACA_LOG_INFO("OnStep", "Pier-side settings unknown: " + why);
            }
            return;
        }
        if (!pier_settings_ || !(*pier_settings_ == *read)) {
            ALPACA_LOG_INFO("OnStep", "Pier-side settings: " + read->describe());
        }
        pier_settings_ = read;
    }

    // The per-session state a disconnect or a lost link leaves behind.
    void reset_session_state_locked() {
        cancel_motion_locked(util::MotionOwner::kBothAxes);
        finish_slew_claim_locked();
        // open-astro#2419: a host clock re-sync the old session decided ends without sending. A relink whose loss the
        // pre-lock link check missed reaches here with the body still waiting out motion, and it must not write into
        // the new session. cancel() never blocks (rule 7); the next disconnect or connect joins the body.
        clock_resync_.cancel();
        park_home_bound_ = kParkHomeBoundFloor;
        home_moves_one_axis_.reset();
        warned_stop_code_ = 0;
        warned_stop_keys_.clear();
        polled_.reset();  // decision 0010: also drops a poll tick in flight, the fault and the loss latch
        refraction_from_firmware_.reset();
        pier_settings_.reset();
        onstepx_ = false;
        firmware_identified_ = false;
        reset_rate_state_locked();
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        target_ra_set_ = false;
        target_dec_set_ = false;
        clear_manual_motion_locked();
        slew_hold_.clear();
        clear_pulse_guides_locked();
        utc_offset_.clear();  // decision 0014: the written UTCDate lives until the next connect or disconnect
        mount_clock_unreadable_.reset();
    }

    // open-astro#521, #2406: OnStepX keeps running a GOTO while the serial link is out (on the HM-17 a GOTO pulled
    // mid-slew stopped only when this sent the stops, #1241), and a MoveAxis for up to about 10 s before the
    // firmware ends it by itself (#1098). Nobody was in control of that motion, however short the outage, so the
    // connect that follows a loss stops it with the same commands AbortSlew sends, before anything else. No recorded
    // loss (e.g. a fresh start) stops nothing: no stamp is not evidence that anything ran unattended. The outage,
    // from the last time the link was seen alive (an auto-detect re-scan counts), is only logged. Tracking is kept.
    // True when it sent the stops; false when it left the motion running (no loss, a park).
    bool stop_motion_after_link_loss_locked() {
        // The loss stamps of the link this driver last let go of (release_link()), read and cleared: no I/O.
        const auto lost_at = std::exchange(carried_lost_at_, std::nullopt);
        const auto alive_at = std::exchange(carried_alive_at_, std::nullopt);
        if (!lost_at) {
            return false;
        }
        // The wrapper stamped both times on clock_ (set at connect).
        const auto outage = clock_.now() - (alive_at ? *alive_at : *lost_at);
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1f", std::chrono::duration<double>(outage).count());
        // A park still running is left to finish (#1289 item 5): :Q# would
        // abort it and leave the mount unparked where it stopped (OnStepX
        // Park::requestAborted()). A status that cannot be read stops anyway.
        try {
            if (wire().get_status().is_parking) {
                ALPACA_LOG_INFO("OnStep", std::string("The link to the mount was last seen alive ") + seconds +
                                              " s ago; the mount is parking, so the park is left to finish");
                return false;
            }
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): an unread status stops anyway
            // unknown state: stop, as before
        }
        ALPACA_LOG_WARN("OnStep",
                        std::string("The link to the mount was last seen alive ") + seconds +
                            " s ago; stopping any slew or MoveAxis that ran unattended (open-astro#521, #2406)");
        // Each stop on its own, as AbortSlew does (#781): a failed :Q# must
        // not skip the per-axis stops that end a MoveAxis.
        (void)send_stops_locked(true, {0, 1, 2, 3}, [](const char* what, const std::string& error) {
            ALPACA_LOG_ERROR("OnStep", std::string("Stop after a link loss failed (") + what + "): " + error);
        });
        return true;
    }

    // The command gate's state checks in OnStep's words (#2258): util/telescope_command_gate.h decides the order
    // (connected; parked; the fault, named, #1414; Tracking), and these answer each step with the driver's own texts
    // and reads. One per call, built under the state lock.
    struct GateChecks {
        const OnStepTelescopeDriver& driver;
        const char* who;

        void check_connected() const { driver.check_connected(); }
        // A non-stop first needs a status read since the connect; a stop refuses only a status that reads parked.
        // #2435: a MoveAxis stop is never refused, parked or not (motion fails closed): its :Qe#/:Qw# or :Qn#/:Qs#
        // are harmless on a parked mount.
        void check_not_parked(bool stop) const {
            if (stop) return;
            driver.check_not_parked_locked(who, stop);
        }
        void check_no_fault() const { driver.refuse_while_faulted_locked(who); }
        // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation (open-astro#1274). Read as the
        // getter does, from the polled frame (a control-path :GU# read only while the frame has no status), so a
        // Tracking=false just written counts as off; before any target or GOTO command is sent.
        void check_tracking() const {
            if (!driver.tracking_as_served_locked()) {
                throw AlpacaException(std::string(who) + " requires Tracking to be true",
                                      AlpacaError::InvalidOperation);
            }
        }
        // The Alt/Az members (the gate runs it after the fault, named before the site read, #1414): the site read is
        // retried first (at most every kSiteInfoRetryDelay); a site still unknown refuses instead of converting at
        // latitude and longitude 0 (#1893).
        void check_site() const {
            driver.ensure_site_info_cached_locked();
            driver.throw_if_site_unknown_locked(who);
        }
        // An Alt/Az slew is refused while Tracking is true, read as the getter reads it, before anything is sent
        // (#1377).
        void check_not_tracking() const {
            if (driver.tracking_as_served_locked()) {
                throw AlpacaException(std::string(who) + " is not allowed while Tracking is true",
                                      AlpacaError::InvalidOperation);
            }
        }
    };

    // A GOTO sent while one runs makes OnStepX stop the running one and refuse the
    // new one (Goto.cpp request(): CE_SLEW_IN_SLEW -> abort(), :MS# reply 5), so the
    // driver refuses first and leaves it running (#1379): its own GOTO (also in the
    // grace before :GU# shows it), a park or home already moving, or a handset GOTO.
    // A home with home sensors runs as a guide move, not a GOTO (:GU# shows 'h',
    // not the missing 'N'): it is refused too, or the slew would end the waiting
    // FindHome and the mount would answer "already moving" (reply 8).
    // It reads :GU# itself, never the frame alone: a GOTO the frame has not seen
    // yet would be stopped by the :MS# (#1379).
    void refuse_while_goto_running_locked(const char* who) const {
        bool running = slew_hold_.active(clock_.now());
        if (!running) {
            refresh_status_locked();
            running = frame_status_locked().is_slewing;
        }
        if (running) {
            throw AlpacaException(std::string(who) + " is not allowed while a GOTO is running",
                                  AlpacaError::InvalidOperation);
        }
        if (frame_status_locked().is_homing) {
            throw AlpacaException(std::string(who) + " is not allowed while the mount is homing",
                                  AlpacaError::InvalidOperation);
        }
    }

    // #2504, ITelescopeV4: a guide pulse during a GOTO, a Park or a FindHome changes the motion the mount runs, so a
    // PulseGuide is refused while one runs, before anything is sent. This half reads the frame, the mount's own
    // status: a GOTO (written through at its dispatch), a park or home the mount took (written through as slewing),
    // 'I' parking and 'h' homing, so a handset motion refuses too. The frame is read, not :GU#, so a guide pulse costs
    // no extra exchange. A MoveAxis is not a slew here ('N' covers a GOTO only). The driver's own slew, Park or
    // FindHome is refused by the pulse's claim (util::MotionOwner, #2522), from the command until a status shows it
    // ended.
    void refuse_pulse_during_slew_locked() const {
        const MountStatus status = frame_status_locked();
        if (status.is_slewing || status.is_parking || status.is_homing) {
            throw AlpacaException("PulseGuide is not allowed while a slew, park or home is running",
                                  AlpacaError::InvalidOperation);
        }
    }

    // The :MS# refusal digit (OnStepX Goto.command.cpp) as the error the client sees (#1379).
    // `command`: the GOTO command that was refused (:MS#, or :MNe#/:MNw# for the SideOfPier flip; same digits).
    static AlpacaException slew_refused(const char* who, const std::string& reply, const char* command = ":MS#") {
        return digit_refused(who, RefusedCommand::Goto, reply.empty() ? '?' : reply[0], reply, command);
    }

    // The :MS# digits, also behind the E of a :CM# refusal (OnStep Command.ino).
    enum class RefusedCommand : std::uint8_t { Goto, Sync };  // :MS# and :CM#

    static AlpacaException digit_refused(const char* who, RefusedCommand refused, char code, const std::string& reply,
                                         const char* goto_command = ":MS#") {
        const bool is_goto = refused == RefusedCommand::Goto;
        const char* what = is_goto ? "GOTO" : "sync";
        const char* command = is_goto ? goto_command : ":CM#";
        const char* why = "an unknown reason";
        switch (code) {
            case '1':
                why = "the target is below the horizon limit";
                break;
            case '2':
                why = "the target is above the overhead limit";
                break;
            case '3':
                why = "the controller is in standby";
                break;
            case '4':
                why = "the mount is parked";
                break;
            case '5':
                why = is_goto ? "a GOTO was already running, and the mount stopped it" : "a GOTO is running";
                break;
            case '6':
                why = "the target is outside the limits";
                break;
            case '7':
                why = "a hardware fault";
                break;
            case '8':
                why = "the mount is already moving";
                break;
            case '9':
                why = "an unspecified error";
                break;
            default:
                break;
        }
        return AlpacaException(std::string(who) + ": the mount refused the " + what + ": " + why + " (" + command +
                                   " reply '" + reply + "')",
                               code == '4' ? AlpacaError::InvalidWhileParked : AlpacaError::InvalidOperation);
    }

    // Tracking as the getter serves it (the frame, a write-through included); a
    // frame with no status yet is filled by a control-path :GU# read first.
    bool tracking_as_served_locked() const {
        const auto reading = polled_.read();
        if (!reading || !reading->value.status_known) {
            refresh_status_locked();
        }
        return frame_status_locked().is_tracking;
    }

    // ── Rates (OnStepX tracking rate, rate offsets, pulse-guide rate) ──────────

    // TrackingRate as the getters serve it: tracking_rate_known_, driver state, no I/O; a latched fault throws.
    // OnStep 4.x is always sidereal. On OnStepX the :GU# letter updates it from every status the driver sees while
    // tracking compensation is off (none: sidereal); with it on the frame carries 'r' or 't' and no letter, so the
    // last rate the driver knew answers: its own writes, the letter while it last showed one, and the :GT# reads
    // (connect, and the slow poll tick while compensation is on and the mount tracks). On OnStepX with no status
    // seen since the connect it throws DriverException (decision 0010 rule 8), as does everything built on it (the
    // rate offset getters). Caller holds mutex_.
    int tracking_rate_served_locked() const {
        throw_if_faulted_locked();
        if (!onstepx_) {
            return 0;
        }
        // Decision 0010 rule 8: with no :GU# answer since the connect nothing device-sourced is held (the last known
        // rate is the reset default then), so this throws as the Tracking getter does instead of answering sidereal.
        if (!mount_type_seen_) {
            throw AlpacaException("OnStep mount status is not available: the mount has not answered :GU#",
                                  AlpacaError::DriverException);
        }
        return tracking_rate_known_;
    }

    // What decides whether the rate offsets must be read again (OnStepX zeroes them on every home reset: a find
    // home, the home at boot, the unpark path; Home.cpp reset(), af1d26bf): the rate, the compensation, and the
    // tracking, parked and at-home flags.
    struct RateWatch {
        int rate = 0;
        bool compensation = false;
        bool tracking = false;
        bool parked = false;
        bool at_home = false;
        bool operator==(const RateWatch&) const = default;
    };
    RateWatch rate_watch_of(const MountStatus& status) const {
        return RateWatch{tracking_rate_known_, status.tracking_compensation, status.is_tracking, status.is_parked,
                         status.is_at_home};
    }

    // :GXTR# and :GXTD# into the driver's state; a reply that does not parse keeps the previous value. Caller holds
    // the operation lock. A read that gets no reply throws.
    void read_rate_offsets_locked() {
        const Wire protocol = wire();
        const auto ra = protocol.get_rate_offset_ra();
        const auto dec = protocol.get_rate_offset_dec();
        set_rate_offsets_locked(ra.value_or(rate_offset_ra_arcsec_), dec.value_or(rate_offset_dec_arcsec_));
    }

    // Every change of the stored offsets but the write's own (set_rate_offset() re-anchors before its send): the read
    // back, an offset change the poll finds, the connect's read. The model re-anchors under the old offsets first
    // (#2241). Caller holds mutex_.
    void set_rate_offsets_locked(double ra, double dec) {
        if (ra == rate_offset_ra_arcsec_ && dec == rate_offset_dec_arcsec_) {
            return;
        }
        reanchor_offset_model_locked();
        rate_offset_ra_arcsec_ = ra;
        rate_offset_dec_arcsec_ = dec;
    }

    // :GX90# into the pulse-guide rate; no answer or a reply that is not a positive decimal keeps what was known.
    void read_pulse_guide_rate_locked() {
        if (const auto rate = wire().get_pulse_guide_rate()) {
            guide_rate_multiple_ = rate;
        }
    }

    // :GT# mapped to a DriveRate, within 0.01 Hz. "0" (not tracking) and a rate that is none of the four keep the
    // last known one.
    void read_tracking_rate_hz_locked() {
        if (const auto hz = wire().get_tracking_rate_hz()) {
            if (const auto rate = tracking_rate_for_hz(*hz)) {
                tracking_rate_known_ = *rate;
            }
        }
    }

    // The session's rate state, cleared at connect and disconnect. Under mutex_.
    void reset_rate_state_locked() {
        tracking_rate_known_ = 0;
        mount_type_seen_.reset();
        rate_offset_ra_arcsec_ = 0.0;
        rate_offset_dec_arcsec_ = 0.0;
        offset_anchor_.reset();
        offset_model_floor_ = {};
        guide_rate_multiple_.reset();
        guide_rate_digit_read_ = -1;
        rate_watch_.reset();
    }

    // At the end of a connect, after the first frame: the pulse-guide rate on any firmware (:GX90# is on OnStep 4.x
    // too, Command.ino), and on OnStepX the tracking rate (:GT#, then the frame's letter when it shows one) and
    // the rate offsets. A read that fails leaves the default; the connect still succeeds, and the poll follows the
    // frame. Caller holds the operation lock.
    void seed_rate_state_locked() {
        const auto reading = polled_.read();
        const bool have_status = reading && reading->value.status_known;
        const MountStatus status = have_status ? reading->value.status : MountStatus{};
        const auto attempt = [](const char* what, const auto& read) {
            try {
                read();
            } catch (const std::exception& ex) {
                ALPACA_LOG_INFO("OnStep", std::string(what) + " not read at connect: " + ex.what());
            }
        };
        if (have_status) {
            guide_rate_digit_read_ = status.pulse_guide_rate_select;
            note_status_seen_locked(status);
        }
        attempt("Pulse-guide rate", [&] { read_pulse_guide_rate_locked(); });
        if (!onstepx_) {
            return;
        }
        attempt("Tracking rate", [&] { read_tracking_rate_hz_locked(); });
        if (have_status) {
            note_status_seen_locked(status);  // the frame's letter wins over :GT# while it names the rate
        }
        attempt("Rate offsets", [&] { read_rate_offsets_locked(); });
        if (have_status) {
            rate_watch_ = rate_watch_of(status);
        }
    }

    // #2296: the park and home bound from the mount's base slew rate (kParkHomeTravelSeconds / deg/s, clamped to
    // [kParkHomeBoundFloor, kParkHomeBoundCap]), read at connect with the operation lock held. A rate that does not
    // read keeps the floor; a lost link is left to the connect's link check that follows, as for the other seed reads.
    void seed_park_home_bound_locked() {
        std::optional<double> base;
        try {
            base = wire().get_base_slew_degrees_per_second();
        } catch (const std::exception& ex) {
            ALPACA_LOG_INFO("OnStep", std::string("Base slew rate not read at connect: ") + ex.what());
        }
        if (!base) {
            return;
        }
        // Capped in double before the cast, so a tiny rate cannot overflow it; the floor is applied after.
        const double travel = std::min(kParkHomeTravelSeconds / *base,
                                       static_cast<double>(std::chrono::seconds(kParkHomeBoundCap).count()));
        park_home_bound_ = std::max(std::chrono::seconds(static_cast<long long>(std::ceil(travel))),
                                    std::chrono::seconds(kParkHomeBoundFloor));
        if (park_home_bound_ > kParkHomeBoundFloor) {
            char rate[32];
            std::snprintf(rate, sizeof(rate), "%.4g", *base);
            ALPACA_LOG_INFO("OnStep", "Park and home bound: " + std::to_string((park_home_bound_.count() + 59) / 60) +
                                          " minutes (base slew rate " + rate + " deg/s)");
        }
    }

    // One poll tick's rate bookkeeping, from the frame it sampled (the operation lock is held). The pulse-guide rate
    // is read again when its :GU# digit changes. On OnStepX the frame's letter keeps the last known tracking rate
    // current while it shows one; while tracking compensation hides it, the slow tick asks :GT# once the mount
    // tracks. The rate offsets are read again when the rate, the compensation or the tracking, parked or at-home
    // flags differ from the previous frame's. Any failure but a lost link keeps what was known (a read that gets
    // no reply is not tried again until the frame changes again).
    void refresh_rate_state_locked(const MountStatus& status, bool full) {
        if (status.pulse_guide_rate_select >= 0 && status.pulse_guide_rate_select != guide_rate_digit_read_) {
            guide_rate_digit_read_ = status.pulse_guide_rate_select;
            try {
                read_pulse_guide_rate_locked();
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    throw;
                }
                ALPACA_LOG_INFO("OnStep", std::string("Pulse-guide rate not read: ") + ex.what());
            }
        }
        if (!onstepx_) {
            return;
        }
        // Taken here too, before the watch below compares: this frame publishes only at the end of the tick.
        note_status_seen_locked(status);
        if (status.tracking_compensation && full && status.is_tracking) {
            try {
                read_tracking_rate_hz_locked();
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    throw;
                }
                ALPACA_LOG_INFO("OnStep", std::string("Tracking rate not read: ") + ex.what());
            }
        }
        const RateWatch watch = rate_watch_of(status);
        if (rate_watch_ && *rate_watch_ == watch) {
            return;
        }
        rate_watch_ = watch;
        try {
            read_rate_offsets_locked();
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_INFO("OnStep", std::string("Rate offsets not read: ") + ex.what());
        }
    }

    // `stop`: AbortSlew and MoveAxis(axis, 0) refuse only a status that reads parked, so an unread one never holds
    // up a stop. Every other caller first needs a status read since the connect (ensure_status_read_locked()).
    void check_not_parked_locked(const char* operation, bool stop = false) const {
        if (!stop) {
            ensure_status_read_locked(operation);
        }
        if (frame_status_locked().is_parked) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    // A frame with no status (every :GU# since the connect failed) reads as idle and not parked to the control paths,
    // and a write-through into it is not served (#1895). A command that checks or writes the status reads :GU# now
    // instead, as the Tracking check of a slew does, and refuses when that read fails too: a latched fault and a
    // lost link as they are, anything else as DriverException (a mount that does not answer :GU#).
    // `refuse` false (Tracking=false, a stop) swallows that last case and carries on; a lost link and a latched fault
    // still throw.
    void ensure_status_read_locked(const char* operation, bool refuse = true) const {
        const auto reading = polled_.read();
        if (reading && reading->value.status_known) {
            return;
        }
        refuse_while_faulted_locked(operation);  // a faulted mount is not asked: the read would wait out its timeout
        try {
            refresh_status_locked();
        } catch (const std::exception& ex) {
            if (is_link_lost(ex) || link_faulted()) {
                throw;
            }
            if (!refuse) {
                return;
            }
            throw AlpacaException(std::string(operation) +
                                      " failed: the mount has not answered :GU# since the connect (" + ex.what() + ")",
                                  AlpacaError::DriverException);
        }
    }

    // The RA/Dec an Alt/Az means, at the known site (the gate's site check ran first).
    void alt_az_to_ra_dec_locked(double altitude, double azimuth, double& ra_out, double& dec_out) const {
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        alt_az_to_ra_dec(altitude, azimuth, site_latitude_cached_, lst, ra_out, dec_out);
    }

    // The GOTO dispatch shared by the coordinate slews (:MS#) and the SideOfPier flip (:MNe#/:MNw#): refuses a
    // fault or a running GOTO, supersedes a park or home, runs `prepare` (the :Sr/:Sd target writes), arms the
    // slew hold, and sends `send`, the GOTO command named `command`. A reply other than '0' clears the hold and
    // raises the mount's refusal digit (#1379). Caller holds the operation lock.
    template <typename Prepare, typename Send>
    void dispatch_goto_locked(const char* context, const char* command, Prepare&& prepare, Send&& send) {
        refuse_while_faulted_locked(context);
        refuse_while_goto_running_locked(context);  // before anything is cancelled or sent (#1379)
        // #2522: the GOTO claims both axes, superseding whatever ran on them (a Park or FindHome too, #1430); a GOTO
        // the mount does not take gives the claim up on the way out.
        slew_claim_ = SlewClaim{claim_motion_locked(util::MotionCommand::Slew), MotionKind::Slew, false};
        const UntakenSlewClaim give_up{this, slew_claim_->claim};
        begin_motion_command_locked();
        const Wire protocol = wire();
        prepare(protocol);
        // Decision 0022 rule 2: armed before the GOTO goes out, cleared when it does not start.
        slew_hold_.arm(clock_.now(), kSlewForceGrace);
        std::string reply;
        try {
            reply = send(protocol);
        } catch (...) {
            slew_hold_.clear();
            throw;
        }
        if (reply.empty() || reply[0] != '0') {
            slew_hold_.clear();
            if (reply.empty()) {
                stop_unconfirmed_goto_locked(context);  // no digit: the mount may have started it
            }
            throw slew_refused(context, reply, command);
        }
        goto_deadline_ = clock_.now() + kGotoTimeout;
        clear_manual_motion_locked();
        tracking_before_slew_ = frame_status_locked().is_tracking;
        watch_motion_locked(MotionKind::Slew, context);  // #2296: before the write-through, from the status read
        slew_claim_->taken = true;                       // #2522: held until a status shows the GOTO ended
        // Write-through (decision 0010): Slewing reads true past the grace until a poll sampled after the GOTO
        // says otherwise; a tick that sampled before it is dropped.
        write_status_locked([](MountStatus& status) { status.is_slewing = true; });
        drop_offset_model_locked();  // #2241: a frame measured after the GOTO anchors it again
    }

    void start_slew_locked(double ra, double dec, const char* context, TargetWrites targets) {
        util::validate_ra_dec(ra, dec, context);
        // The targets are written after the refusals and the supersede, before the hold is armed and the :MS#.
        dispatch_goto_locked(
            context, ":MS#",
            [&](const Wire& protocol) {
                protocol.set_target_ra(ra);
                protocol.set_target_dec(dec);
            },
            [](const Wire& protocol) { return protocol.slew_to_target(); });
        goto_target_ = util::MountPosition{ra, dec};  // #2406: its landing is judged against this
        goto_running_ = true;
        goto_seen_slewing_ = false;
        if (watched_motion_) {
            watched_motion_->target = std::make_pair(ra, dec);  // #2296: a stop code at the end is judged against it
        }
        store_target_locked(ra, dec, targets);
    }

    // Slewing as the getter serves it, from the polled frame (decision 0010); the synchronous slew's wait reads
    // it after its own :GU# read, which is written through.
    bool get_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            // Slewing follows the mount once it has shown the move (#1378).
            throw_if_faulted_locked();  // a latched fault is reported, as by every read (#1414)
            const auto reading = polled_.read();
            if (!reading || !reading->value.status_known || !manual_move_ended_locked(reading->value.status)) {
                return true;  // keep reporting the move; an unread status is not a stop
            }
            clear_manual_motion_locked();
        }
        if (slew_hold_.active(clock_.now())) {
            throw_if_faulted_locked();  // the grace asks the mount nothing (decision 0009 rule 1)
            return true;
        }
        // Past the window the frame decides. Landing (a reading after the :MS# answer that shows no GOTO) has
        // cleared the window and written the status; a reading from before the answer is never published over
        // the dispatch's write-through (decision 0010 token).
        return served_status_locked().is_slewing;
    }

    // Uses and clears the restore flag that start_slew_locked() set: restore
    // tracking once if that slew started with tracking on and `status`, read
    // after its GOTO ended, reports it off. Every path that ends a coordinate
    // slew calls this (the status poll when it sees the GOTO end,
    // wait_for_slew_complete_locked(), abort_slew()), so the flag cannot
    // outlive its slew and fire at an unrelated one (#1230). Per the SynScan
    // tracking-restoration lesson, some LX200-family mounts stop tracking
    // during a GOTO; OnStepX keeps tracking through one (seen on the TeSeek
    // HM-17), so there this normally sends nothing. Park and FindHome clear the
    // flag: they end with tracking off on purpose, and restarting it moved the
    // mount off home (#1218). Every caller holds the operation lock, so no
    // command lands between the status read and the restore.
    //
    // A restore that fails is logged and given up, except a lost link, which
    // is reported (NotConnected): it is no longer swallowed (#1783).
    // `tracking_now` is what that read says; true is returned when the restore went out, and the caller writes
    // it through (the poll into the frame it is about to publish, which a write would drop).
    bool finish_slew_tracking_locked(bool tracking_now) const {
        const bool restore_tracking = tracking_before_slew_ && !tracking_now;
        tracking_before_slew_ = false;
        if (!restore_tracking) {
            return false;
        }
        try {
            wire().start_tracking(onstepx_);
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_WARN("OnStep", std::string("Tracking restore after the GOTO failed: ") + ex.what());
            return false;
        }
        return true;
    }

    // finish_slew_tracking_locked() on the frame's status, written through.
    void finish_slew_tracking_from_frame_locked() const {
        if (finish_slew_tracking_locked(frame_status_locked().is_tracking)) {
            write_status_locked([](MountStatus& status) { status.is_tracking = true; });
        }
    }

    // Poll for a status condition, RELEASING the driver mutex around every
    // sleep so a synchronous Park/FindHome/SlewToCoordinates doesn't block
    // all GETs and Disconnect for the whole operation (Bisque/SynScan
    // unlock-sleep-relock pattern). `lock` must be held on entry and is held
    // again on return/throw.
    template <typename Predicate>
    void wait_for_condition_locked(StateLock& lock, Predicate done) const {
        const auto start = clock_.now();
        for (;;) {
            // Before done(): a GOTO the status poll stopped at its bound reads as ended on the next :GU#, and must
            // fail the slew, not land it (#2406).
            if (!bound_error_.empty()) {
                throw AlpacaException(bound_error_, AlpacaError::DriverException);
            }
            if (done()) {
                return;
            }
            if (clock_.now() - start > kGotoTimeout) {
                // #2406: a timeout that sends no stop left the GOTO running. Stop it first; Slewing then raises.
                stop_overdue_goto_locked();
                throw AlpacaException(bound_error_, AlpacaError::DriverException);
            }
            lock.unlock();
            clock_.sleep_for(std::chrono::milliseconds(250));
            lock.lock();
            check_connected();
            refresh_status_locked();
        }
    }

    void wait_for_slew_complete_locked(StateLock& lock) const {
        wait_for_condition_locked(lock, [this] { return !get_slewing_locked(); });
        goto_deadline_.reset();  // landed on the wait's own :GU#, before the next poll tick sees it (#2406)
        goto_running_ = false;   // the wait's own :GU# showed no GOTO
        // The control path ends the motion: publish where the mount stopped, so the position read right after
        // Slewing goes false is not one the poll took during the GOTO (decision 0010).
        const auto ended_at = clock_.now();
        publish_measurement_locked();
        raise_motion_error_locked(MotionKind::Slew);  // #2296: the read that saw the end, or a poll tick, kept it
        // #2406: that frame judges the landing, before the next poll tick sees it. A failed read leaves it to the
        // next tick's frame. A stop the mount made itself (above) is the reason, not a far miss.
        const auto measured = polled_.read();
        if (measured && measured->value.sampled_at >= ended_at) {
            judge_landing_locked(measured->value);
        }
        if (!bound_error_.empty()) {
            throw AlpacaException(bound_error_, AlpacaError::DriverException);
        }
        // Decide the restore here; the status was just read (#1230). A slew the mount stopped owes none (#2296), and
        // one the driver stopped for a far miss gave it up (#2406).
        finish_slew_tracking_from_frame_locked();
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
    }

    // open-astro#1414, decision 0009: a mount that stops answering while its adapter stays plugged in (mount
    // unpowered, cable pulled at the mount) latches a fault in util::PolledState after kLinkFaultThreshold failed
    // poll probes in a row: Connected stays true, and reads throw DriverException with the fault instead of serving
    // the frame. A fault, or a frame no probe refreshed, standing for util::kLinkStalenessBound is a lost link:
    // declare_poll_loss() has the wrapper record it and close the port as for a pulled adapter (#1241), so Connected
    // reads false and calls throw NotConnected. Only the client's Connect relinks. The poll keeps asking while
    // faulted, so one good probe clears the fault. Control-path reads (a command's :GU#, the synchronous slew's end
    // read, the connect's seed) feed no latch (#2495): only the probe does. Time is the task clock.
    bool link_faulted() const { return !polled_.fault().empty(); }

    // A read that answers from the frame or the driver's own state (the
    // position and status getters, the site, UTCDate, SiderealTime,
    // DestinationSideOfPier, DoesRefraction, IsPulseGuiding and Slewing's
    // pending Park/FindHome and grace answers) throws the latched fault
    // (decision 0009 rule 1), one text for all of them (#1658 review).
    void throw_if_faulted_locked() const {
        const std::string fault = polled_.fault();
        if (!fault.empty()) {
            throw AlpacaException("OnStep mount communications compromised: " + fault, AlpacaError::DriverException);
        }
    }

    // A read that found the link gone.
    static bool is_link_lost(const std::exception& ex) {
        const auto* alpaca = dynamic_cast<const AlpacaException*>(&ex);
        return alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected;
    }

    // Decision 0009 rule 1: while the fault is latched, writes throw as reads
    // do. Blind commands cannot fail on a healthy fd, so without this a
    // Tracking, Unpark or a slew would report success to a mount that is
    // not listening. AbortSlew and MoveAxis(axis, 0) send their stops first
    // and call this after, so the stop is still tried (#1414). The poll's
    // probes, not a write, find the loss bound (#2495).
    void refuse_while_faulted_locked(const char* what) const {
        const std::string fault = polled_.fault();
        if (!fault.empty()) {
            throw AlpacaException(std::string(what) + ": OnStep mount communications compromised: " + fault,
                                  AlpacaError::DriverException);
        }
    }

    // ── Snapshot-served state (decision 0010, OnStep adoption, #1783) ──────────

    // The frame's status, for the control paths: default (idle, not parked) before any :GU# answered. Takes only
    // the snapshot's own mutex.
    MountStatus frame_status_locked() const {
        const auto reading = polled_.read();
        return reading && reading->value.status_known ? reading->value.status : MountStatus{};
    }

    // Write-through of a status the driver now knows (a command's effect). The write drops a poll tick in flight
    // (decision 0010 point 4) and leaves measured_at alone. With no frame held it is not kept: the next tick
    // measures the mount.
    template <typename Apply>
    void write_status_locked(Apply&& apply) const {
        polled_.write([&apply](PolledFrame& frame) { apply(frame.status); });
    }

    // A write that changed what the mount's position means (a pulse, a MoveAxis start, the stops): the tick in
    // flight sampled before it and is dropped; the next tick measures.
    void drop_poll_in_flight_locked() const {
        polled_.write([](PolledFrame&) {});
    }

    // A :GU# a control path read (decision 0010: control paths keep their direct reads and write through). A
    // status equal to the frame's is not written, so a control loop that re-reads :GU# (the synchronous slew,
    // the Park/FindHome settle) does not drop every poll tick and freeze the position. `counts_for_move`: a read
    // after the last MoveAxis start, which may end it (#1378).
    void note_status_read_locked(const MountStatus& status, bool counts_for_move) const {
        note_status_seen_locked(status);
        note_motion_end_locked(status);  // #2296: a status equal to the frame's can still be the first after a start
        if (counts_for_move) {
            note_status_measured_locked(status);
        }
        const auto held = polled_.read();
        if (held && held->value.status_known && same_status(held->value.status, status)) {
            return;
        }
        polled_.write([&status](PolledFrame& frame) {
            frame.status = status;
            frame.status_known = true;
        });
    }

    // Every status the driver learns, from the poll or a control-path read, updates the mount settings it keeps
    // (decision 0010 OnStep row, #2214): the mount type, and on OnStepX the rate letter while compensation shows it.
    void note_status_seen_locked(const MountStatus& status) const {
        mount_type_seen_ = status.mount_type;
        if (onstepx_ && !status.tracking_compensation) {
            tracking_rate_known_ = status.tracking_rate;
        }
    }

    // A :GU# measured after the last MoveAxis start reached the frame (#1378).
    void note_status_measured_locked(const MountStatus& status) const {
        note_status_seen_locked(status);
        status_read_since_move_ = true;
        if (status.is_guide_active && !status.is_pulse_guide_active) {
            firmware_reports_guide_move_ = true;
        }
    }

    // Control path: :GU# now, written through. A failed read feeds no fault latch (#2495); it throws when the link
    // went, a fault is latched or the frame knows no status, and is otherwise not reported, as the status cache was
    // served before #1783 (#1414). Caller holds the operation lock.
    void refresh_status_locked() const {
        try {
            const MountStatus status = wire().get_status();
            if (!status.is_slewing) {
                slew_hold_.clear();  // landed: a reading with no GOTO (decision 0022 rules 2, 3)
            } else if (goto_target_) {
                goto_seen_slewing_ = true;  // the GOTO ran: its end is judged (#2406)
            }
            note_status_read_locked(status, true);
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            throw_if_faulted_locked();
            const auto reading = polled_.read();
            if (!reading || !reading->value.status_known) {
                throw;
            }
        }
    }

    // The frame a getter serves, or the refusal that applies: NotConnected (the caller checked), a latched fault
    // (DriverException, Connected stays true), or no frame yet. A frame past kSnapshotMaxAge is served as it
    // is: stale is not lost (decision 0010 rule 6).
    PolledFrame served_frame_locked() const {
        throw_if_faulted_locked();
        if (!polled_.read()) {
            throw AlpacaException("OnStep mount state is not available yet: the first status poll has not answered",
                                  AlpacaError::DriverException);
        }
        return polled_.served().value;  // NotConnected once the frame is util::kLinkStalenessBound old
    }

    MountStatus served_status_locked() const {
        const PolledFrame frame = served_frame_locked();
        if (!frame.status_known) {
            throw AlpacaException("OnStep mount status is not available: the mount has not answered :GU#",
                                  AlpacaError::DriverException);
        }
        return frame.status;
    }

    Position served_position_locked() const {
        const PolledFrame frame = served_frame_locked();
        if (!frame.equatorial_known) {
            throw AlpacaException("OnStep mount position is not available: the mount has not answered :GRa#/:GDe#",
                                  AlpacaError::DriverException);
        }
        return Position{frame.ra_hours, frame.dec_degrees};
    }

    AltAz served_altaz_locked() const {
        const PolledFrame frame = served_frame_locked();
        if (!frame.altaz_known) {
            throw AlpacaException("OnStep mount Alt/Az is not available: the mount has not answered :GA#/:GZ#",
                                  AlpacaError::DriverException);
        }
        return AltAz{frame.alt_degrees, frame.az_degrees};
    }

    // ── The rate-offset position model (#2241) ─────────────────────────────────────────────────────────────────

    bool rate_offsets_set_locked() const { return rate_offset_ra_arcsec_ != 0.0 || rate_offset_dec_arcsec_ != 0.0; }

    // Whether the model may run for `frame` at `now`: OnStepX only; a frame that says the mount tracks and is not
    // slewing, pulse guiding, moving an axis ('g'), homing, parking or parked; none of the driver's own motion the
    // frame may not show yet (a MoveAxis, the slew grace, a pending Park or FindHome, a pulse); and, while an offset is
    // set, TrackingRate sidereal (ITelescopeV4: the offsets read 0 on another rate, and the model has no lunar, solar
    // or King term). Caller holds mutex_.
    bool offset_model_allowed_locked(const PolledFrame& frame, util::TaskClock::clock::time_point now) const {
        if (!onstepx_ || !frame.status_known || !frame.equatorial_known) {
            return false;
        }
        const MountStatus& status = frame.status;
        if (!status.is_tracking || status.is_slewing || status.is_pulse_guide_active || status.is_guide_active ||
            status.is_homing || status.is_parking || status.is_parked) {
            return false;
        }
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1] || slew_hold_.active(now) || park_home_request_ ||
            pulse_hold_[0].active(now) || pulse_hold_[1].active(now)) {
            return false;
        }
        return !rate_offsets_set_locked() || tracking_rate_known_ == 0;
    }

    // The model's position now, when it is served: anchored, inside kOffsetModelHold, allowed for the held frame, and
    // that frame no older than kSnapshotMaxAge. Past that bound the link has stalled and the frame is served as it is
    // (decision 0010 rule 6), never a position extrapolated from a dead link. Snapshot data and driver state only, no
    // I/O (decision 0010 rule 1, decision 0016). Caller holds mutex_.
    std::optional<Position> offset_model_position_locked() const {
        if (!offset_anchor_) {
            return std::nullopt;
        }
        const auto reading = polled_.read();
        const auto now = clock_.now();
        if (!reading || reading->stale || now - offset_anchor_->at >= kOffsetModelHold ||
            !offset_model_allowed_locked(reading->value, now)) {
            return std::nullopt;
        }
        return offset_model_position(*offset_anchor_, rate_offset_ra_arcsec_, rate_offset_dec_arcsec_, now);
    }

    // What RightAscension and Declination serve: the frame's refusals first (a latched fault, no frame, no position
    // read), then the model while it runs, else the frame as read.
    Position served_sky_position_locked() const {
        const Position frame = served_position_locked();
        return offset_model_position_locked().value_or(frame);
    }

    // Before a stored offset changes: the model re-anchors at now on the position under the old offsets, the model
    // when it is served, else the held frame's measurement carried forward from its read (a frame no older than
    // kSnapshotMaxAge), so a new rate never applies to the time before it. In a state the model does not run in, the
    // anchor is dropped and the next poll's measurement starts it. Caller holds mutex_.
    void reanchor_offset_model_locked() const {
        const auto now = clock_.now();
        offset_model_floor_ = now;  // a tick that sampled before this change never anchors, keeps or drops the model
        if (const auto model = offset_model_position_locked()) {
            offset_anchor_ = OffsetAnchor{model->ra_hours, model->dec_degrees, now};
            return;
        }
        const auto reading = polled_.read();
        if (!reading || reading->stale || !offset_model_allowed_locked(reading->value, now)) {
            offset_anchor_.reset();
            return;
        }
        const PolledFrame& frame = reading->value;
        const Position position =
            offset_model_position(OffsetAnchor{frame.ra_hours, frame.dec_degrees, frame.position_at},
                                  rate_offset_ra_arcsec_, rate_offset_dec_arcsec_, now);
        offset_anchor_ = OffsetAnchor{position.ra_hours, position.dec_degrees, now};
    }

    // A sync: a running offset goes on from the synced position. Caller holds mutex_.
    void anchor_offset_model_at_sync_locked(double ra, double dec) const {
        const auto reading = polled_.read();
        const auto now = clock_.now();
        offset_model_floor_ = now;
        if (rate_offsets_set_locked() && reading && offset_model_allowed_locked(reading->value, now)) {
            offset_anchor_ = OffsetAnchor{ra, dec, now};
        } else {
            offset_anchor_.reset();
        }
    }

    // A command that moves the mount on its own (a GOTO, a park or home, a MoveAxis start or stop, a pulse, the
    // stops) or turns Tracking: the model stops, and the next frame measured after the command that it may run for
    // anchors it again (a tick that sampled before it does not). Caller holds mutex_.
    void drop_offset_model_locked() const {
        offset_anchor_.reset();
        offset_model_floor_ = clock_.now();
    }

    // A frame the poll (or the synchronous slew's end read) has just published: the guard. A frame whose :GRa#/:GDe#
    // read is older than the model's last driver-side change (offset_model_floor_: a re-anchor before an offset
    // change, a sync, a drop) changes nothing: it predates what the driver knows, so it neither ends the hold of a
    // position whose offsets went to zero nor anchors under offsets the mount did not run then. Otherwise, with no
    // offset set, or in a state the model does not run in, or a frame sampled before a pulse was over, the model
    // stops; the first frame in a state it runs in anchors it; one kOffsetModelHold or more after the anchor
    // re-anchors on the measurement; and one that disagrees with the model at its read by more than
    // kOffsetModelGuardArcsec on either axis re-anchors on the measurement. Caller holds mutex_.
    void adopt_offset_model_frame_locked(const PolledFrame& frame) const {
        if (frame.position_at < offset_model_floor_) {
            return;
        }
        const bool after_pulses = !pulse_hold_[0].active(frame.sampled_at - kPulseGuideSettle) &&
                                  !pulse_hold_[1].active(frame.sampled_at - kPulseGuideSettle);
        if (!rate_offsets_set_locked() || !after_pulses || !offset_model_allowed_locked(frame, clock_.now())) {
            offset_anchor_.reset();
            return;
        }
        const OffsetAnchor measured{frame.ra_hours, frame.dec_degrees, frame.position_at};
        if (!offset_anchor_) {
            offset_anchor_ = measured;
            return;
        }
        if (frame.position_at - offset_anchor_->at >= kOffsetModelHold) {
            offset_anchor_ = measured;
            return;
        }
        const Position model =
            offset_model_position(*offset_anchor_, rate_offset_ra_arcsec_, rate_offset_dec_arcsec_, frame.position_at);
        if (offset_model_disagrees(model, frame.ra_hours, frame.dec_degrees)) {
            ALPACA_LOG_DEBUG("OnStep", "Rate-offset model re-anchored: the mount is more than 2 arcsec from it");
            offset_anchor_ = measured;
        }
    }

    // Decision 0010 rule 5: DeviceState's TimeStamp is when the poll measured. Task-clock age; takes only the
    // snapshot's own mutex.
    std::optional<std::chrono::nanoseconds> device_state_age() const override { return polled_.age(); }

    // One mount sample: :GU# first, so a frame that says "not slewing" carries a position read after the stop,
    // then :GRa#/:GDe#, and with `full` :GA#/:GZ#. Throws at the first failed exchange of the mandatory part
    // (a tick on a dead link waits out one timeout); a failed Alt/Az read keeps the previous value. Caller holds
    // the operation lock.
    PolledFrame sample_locked(const PolledFrame& previous, bool full) const {
        const Wire protocol = wire();
        PolledFrame frame = previous;
        frame.sampled_at = clock_.now();
        frame.sample_seq = ++sample_seq_;
        frame.status = protocol.get_status();
        frame.status_known = true;
        const auto position_asked = clock_.now();
        const Position position = protocol.get_position();
        frame.position_at = position_asked + (clock_.now() - position_asked) / 2;
        frame.ra_hours = position.ra_hours;
        frame.dec_degrees = position.dec_degrees;
        frame.equatorial_known = true;
        if (full) {
            try {
                const AltAz altaz = protocol.get_alt_az();
                frame.alt_degrees = altaz.altitude_degrees;
                frame.az_degrees = altaz.azimuth_degrees;
                frame.altaz_known = true;
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    throw;
                }
                ALPACA_LOG_WARN("OnStep", std::string("Alt/Az read failed; the last value is kept: ") + ex.what());
            }
        }
        return frame;
    }

    // Where a control path ends a motion (the synchronous slew): reads the mount now and publishes a full frame
    // through the module (PolledState::begin_poll() and publish()), so Slewing false is never served beside a
    // position from mid-motion. A failed read keeps the last values served and feeds no fault latch (#2495): the
    // poll's probes judge the link. Only a lost link is reported. Caller holds the operation lock.
    void publish_measurement_locked() const {
        const auto token = polled_.begin_poll();
        const auto previous = polled_.read();
        try {
            const PolledFrame frame = sample_locked(previous ? previous->value : PolledFrame{}, true);
            if (publish_sample_locked(token, frame)) {
                note_status_measured_locked(frame.status);
                adopt_offset_model_frame_locked(frame);
            }
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_WARN("OnStep",
                            std::string("End-of-slew read failed; the last values stay served: ") + ex.what());
        }
    }

    // The first frame, at the end of a connect, so the getters have an answer, status flags included, the
    // moment Connect returns; PolledState::start() then sends nothing for its first probe. Each read on its own, as
    // the cache warm-up did: a mount that answers none leaves a frame with nothing known (the getters refuse it until
    // a tick answers), and a failure feeds no fault latch (#2495): the poll's probes do. The mount's site is read
    // here too while the config did not set it. A link lost meanwhile is not one of these: the connect checks for it
    // after the seed and fails (#1895).
    void seed_snapshot_locked() {
        const Wire protocol = wire();
        const auto token = polled_.begin_poll();
        PolledFrame frame;
        frame.sampled_at = clock_.now();
        frame.sample_seq = ++sample_seq_;
        const auto read = [](const std::function<void()>& exchange) {
            try {
                exchange();
            } catch (const std::exception& ex) {
                ALPACA_LOG_WARN("OnStep", std::string("Connect read failed; the poll asks again: ") + ex.what());
            }
        };
        read([&] {
            frame.status = protocol.get_status();
            frame.status_known = true;
        });
        read([&] {
            const auto position_asked = clock_.now();
            const Position position = protocol.get_position();
            frame.position_at = position_asked + (clock_.now() - position_asked) / 2;
            frame.ra_hours = position.ra_hours;
            frame.dec_degrees = position.dec_degrees;
            frame.equatorial_known = true;
        });
        try {
            ensure_site_info_cached_locked();
        } catch (...) {  // NOLINT(bugprone-empty-catch): the connect still succeeds; the poll retries it
        }
        read([&] {
            const AltAz altaz = protocol.get_alt_az();
            frame.alt_degrees = altaz.altitude_degrees;
            frame.az_degrees = altaz.azimuth_degrees;
            frame.altaz_known = true;
        });
        polled_.publish(token, frame);
        if (frame.status_known) {
            note_status_measured_locked(frame.status);
        }
    }

    // PolledState's probe (decision 0010 rule 7, #2495), on the poll thread with no driver lock held, or on a test
    // seam's thread. Each tick publishes its own frame (poll_tick()) and returns nullopt, so the module counts the
    // reply and publishes nothing over it; a failed sample throws, which the module counts toward the fault. The
    // first call after a connect is the seed's: the connect already published that frame.
    std::optional<PolledFrame> probe() {
        if (skip_next_probe_.exchange(false)) {
            return std::nullopt;
        }
        const std::optional<int> forced = forced_poll_tick();
        const int tick = forced.value_or(next_poll_tick_);
        if (!forced) {
            ++next_poll_tick_;
        }
        poll_tick([this, seam = forced.has_value()] { return !seam && poll_cancelled_.load(); }, tick);
        return std::nullopt;
    }

    // One poll tick, the probe's or the pulse-end measurement's. Its exchanges hold the operation lock, like a
    // command's, so they never land inside a command's sequence (:hC# then :GE#), and mutex_ is dropped for each one
    // (decision 0016); the frame is published under mutex_ alone through PolledState::publish(), so a client write
    // between the sample and the publish drops it (decision 0010 point 4), and the status note and the rate-offset
    // model take only a frame whose publish took. The tick ends at its first failed exchange, which it throws; a lost
    // link is reported to the module first (PolledState::lose_link(), decision 0009 rule 2a). It returns with nothing
    // done when the connection is gone or `stopped()` (a cancel, decision 0006 rule 6).
    template <typename Stopped>
    void poll_tick(const Stopped& stopped, int tick) {
        const bool full = tick % kSlowPollEvery == 0;
        util::PolledState<PolledFrame>::Token token{};
        PolledFrame frame;
        {
            StateLock lock(lock_gate_);
            if (!connected_.load() || !published_connected_.load() || stopped()) {
                return;
            }
            if (!link_alive()) {
                polled_.lose_link();
                throw AlpacaException("OnStep link lost", AlpacaError::NotConnected);
            }
            const auto previous = polled_.read();
            token = polled_.begin_poll();
            try {
                frame = sample_locked(previous ? previous->value : PolledFrame{}, full);
            } catch (const std::exception& ex) {
                if (stopped()) {
                    return;  // superseded or cancelled mid-sample: no verdict, restore or site read
                }
                if (is_link_lost(ex)) {
                    polled_.lose_link();
                } else {
                    check_goto_bound_unread_locked();  // #2406: a failed :GU# still enforces the GOTO's bound
                }
                throw;
            }
            if (stopped()) {
                return;
            }
            try {
                finish_slew_if_ended_locked(frame);
                check_motion_bounds_locked(frame.status);
                if (full && !site_known_locked()) {
                    ensure_site_info_cached_locked(/*probe_answered=*/true);  // one the connect could not read (#1783)
                }
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    polled_.lose_link();
                    throw;
                }
            }
            if (stopped()) {
                return;  // cancelled: no rate reads
            }
            try {
                refresh_rate_state_locked(frame.status, full);
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    polled_.lose_link();
                    throw;
                }
            }
            // The pier-side settings follow a change made at a handset (#2179). Any failure but a lost link keeps
            // the previous ones.
            if (stopped()) {
                return;
            }
            if (onstepx_ && tick > 0 && tick % kPierSettingsEvery == 0) {
                try {
                    refresh_pier_settings_locked(/*at_connect=*/false);
                } catch (const std::exception& ex) {
                    if (is_link_lost(ex)) {
                        polled_.lose_link();
                        throw;
                    }
                }
            }
        }
        run_status_poll_after_sample_hook();
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!connected_.load() || stopped()) {
            return;
        }
        if (publish_sample_locked(token, frame)) {
            note_status_measured_locked(frame.status);
            adopt_offset_model_frame_locked(frame);
        }
    }

    // The regular tick, the pulse-end tick and the synchronous slew's end read all publish under mutex_, the ticks
    // after the operation lock is released, so they can publish out of the order they sampled in. A sample older
    // than the held frame's is skipped; a newer one in flight is left to publish (decision 0010: a client write
    // after its begin_poll() still drops it).
    bool publish_sample_locked(util::PolledState<PolledFrame>::Token token, const PolledFrame& frame) const {
        const auto held = polled_.read();
        if (held && frame.sample_seq < held->value.sample_seq) {
            return false;
        }
        return polled_.publish(token, frame);
    }

    // The tracking restore a coordinate slew owes (#1230), on the first tick frame that shows no GOTO (inside the
    // hold window too, decision 0022 rule 3): before #1783 a Slewing read sent it, as a single read that waited for the
    // operation lock. It is not decided during a MoveAxis (a 4.x mount refuses :ST mid-move), nor while a Park or
    // FindHome is pending: one refused "in motion" and retried in the park_home_ body ends with tracking off, so a
    // restore sent between its tries would only be undone (#1743 review). The tick holds the operation lock, so no
    // command lands between its :GU# and the restore. A frame that shows no GOTO ends the slew; the hold window is not
    // read (decision 0022 rule 3). A frame cannot predate the :MS# answer: the tick holds the operation lock that :MS#
    // is sent under, and the dispatch write-through shows the GOTO until a later frame replaces it.
    void finish_slew_if_ended_locked(PolledFrame& frame) const {
        // #2296: first, so a slew the mount stopped owes no restore; the frame's position judges a stop code.
        note_motion_end_locked(frame.status, frame.equatorial_known
                                                 ? std::optional<Position>(Position{frame.ra_hours, frame.dec_degrees})
                                                 : std::nullopt);
        if (frame.status.is_slewing) {
            goto_seen_slewing_ = goto_seen_slewing_ || goto_target_.has_value();  // its end is judged (#2406)
            return;
        }
        slew_hold_.clear();  // landed: a frame with no GOTO (decision 0022 rules 2, 3)
        goto_running_ = false;
        judge_landing_locked(frame);  // a far miss stops the mount and gives up the restore (#2406)
        if (!tracking_before_slew_ || manual_axis_slewing_[0] || manual_axis_slewing_[1] || park_home_request_) {
            return;
        }
        if (finish_slew_tracking_locked(frame.status.is_tracking)) {
            frame.status.is_tracking = true;
        }
    }

    // #2406: the bounds the status poll enforces on each frame, under the operation lock its :GU# took: a GOTO past
    // kGotoTimeout is stopped, and an AbortSlew the mount did not obey is sent again. The bound outlives a frame
    // that shows no GOTO: the mount may start the GOTO only after that frame. It ends at its deadline, or with the
    // next motion command.
    void check_motion_bounds_locked(const MountStatus& status) const {
        if (goto_deadline_ && clock_.now() >= *goto_deadline_) {
            if (status.is_slewing) {
                stop_overdue_goto_locked();
            } else {
                goto_deadline_.reset();
                goto_target_.reset();  // a GOTO never seen running is no longer judged
            }
        }
        check_abort_stop_locked(status);
    }

    // #2406: a GOTO of this driver has not landed: no frame has shown it end yet, or a :GU# showed it running and
    // its end is not judged yet.
    bool goto_unlanded_locked() const { return goto_running_ || goto_seen_slewing_; }

    // #2406: a tick whose :GU# failed still enforces the bound of a GOTO not judged landed (one no frame has seen
    // run may start late): the mount may still be running it, so the stops go out.
    void check_goto_bound_unread_locked() const {
        if ((goto_target_ || goto_unlanded_locked()) && goto_deadline_ && clock_.now() >= *goto_deadline_) {
            stop_overdue_goto_locked();
        }
    }

    // #2406 (#2396 AB-7): `frame` shows no GOTO, and its position was read after that :GU#. A GOTO that ended more
    // than util::kLandingFarMissArcsec from its target on either axis did not land: it is stopped, and Slewing
    // raises it. One frame is taken as still, without util::LandingGate's stillness pair.
    // Only a GOTO a :GU# has shown running is judged: a frame from before the mount started it holds the position
    // before the slew. A GOTO never seen running is left to the kGotoTimeout bound.
    // TODO(#2406): check on a rig that :GU# shows no GOTO only once both axes have stopped.
    void judge_landing_locked(const PolledFrame& frame) const {
        if (!goto_target_ || !goto_seen_slewing_ || frame.status.is_slewing || !frame.equatorial_known) {
            return;
        }
        const util::MountPosition target = *goto_target_;
        goto_target_.reset();
        goto_seen_slewing_ = false;
        const double miss = util::axis_miss_arcsec({frame.ra_hours, frame.dec_degrees}, target);
        if (miss <= util::kLandingFarMissArcsec) {
            return;
        }
        char text[160];
        std::snprintf(text, sizeof(text), "The GOTO ended %.0f arcsec from its target; the driver stopped it", miss);
        stop_goto_locked(text);
    }

    // #2406 (#2396 AB-4): `status` from a :GU# after the AbortSlew. No motion ends the check. Motion when a round is
    // due sends the stops again, up to kAbortStopRounds in all; motion after the last round is a stored failure.
    void check_abort_stop_locked(const MountStatus& status) const {
        if (abort_stop_rounds_ == 0) {
            return;
        }
        const bool moving =
            status.is_slewing || status.is_homing || (status.is_guide_active && !status.is_pulse_guide_active);
        if (!moving) {
            abort_stop_rounds_ = 0;
            return;
        }
        if (clock_.now() < abort_next_round_) {
            return;
        }
        if (abort_stop_rounds_ >= kAbortStopRounds) {
            abort_stop_rounds_ = 0;
            // Appended, so a GOTO the 120 s bound stopped still says so.
            const std::string text =
                "AbortSlew: the mount still reports motion after " + std::to_string(kAbortStopRounds) + " stops";
            bound_error_ = bound_error_.empty() ? text : bound_error_ + "; " + text;
            ALPACA_LOG_ERROR("OnStep", bound_error_);
            return;
        }
        ++abort_stop_rounds_;
        abort_next_round_ = clock_.now() + kAbortStopRound;
        ALPACA_LOG_WARN("OnStep", "AbortSlew: the mount still reports motion; sending the stops again (round " +
                                      std::to_string(abort_stop_rounds_) + ")");
        (void)send_stops_locked(true, {0, 1, 2, 3}, [](const char* what, const std::string& error) {
            ALPACA_LOG_ERROR("OnStep", std::string("AbortSlew re-send failed (") + what + "): " + error);
        });
    }

    // #2406: a GOTO past kGotoTimeout. AbortSlew's stops go out, the stop is checked as after an AbortSlew, and
    // Slewing raises the failure.
    void stop_overdue_goto_locked() const {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(kGotoTimeout).count();
        stop_goto_locked("The GOTO did not end within " + std::to_string(seconds) + " s; the driver stopped it");
    }

    // #2406: a GOTO the driver stops itself (its bound, a far miss): `text` is stored for Slewing.
    void stop_goto_locked(std::string text) const {
        goto_deadline_.reset();
        goto_target_.reset();
        goto_running_ = false;
        goto_seen_slewing_ = false;
        slew_hold_.clear();
        tracking_before_slew_ = false;
        bound_error_ = std::move(text);
        ALPACA_LOG_ERROR("OnStep", bound_error_);
        (void)send_stops_locked(true, {0, 1, 2, 3}, [](const char* what, const std::string& error) {
            ALPACA_LOG_ERROR("OnStep", std::string("Stop of a GOTO failed (") + what + "): " + error);
        });
        abort_stop_rounds_ = 1;
        abort_next_round_ = clock_.now() + kAbortStopRound;
    }

    // #2406: a :MS# with no digit before its reply timeout may still have started the GOTO, so :Q# goes out (best
    // effort, logged) before the refusal is reported. (A thrown exchange gets none: it throws when :MS# could not be
    // written or the link is lost.)
    void stop_unconfirmed_goto_locked(const char* context) const {
        ALPACA_LOG_WARN("OnStep", std::string(context) + ": no :MS# reply; sending :Q# in case the GOTO started");
        (void)send_stops_locked(true, {}, [context](const char* what, const std::string& error) {
            ALPACA_LOG_ERROR("OnStep",
                             std::string(context) + ": stop after an unanswered :MS# failed (" + what + "): " + error);
        });
    }

    // Call with no driver lock held (decision 0006 rule 4): the poll and pulse-end bodies take them.
    // poll_cancelled_ stands for the cancel while the bodies are joined, so a probe in its sample gives no verdict.
    void stop_status_poller() {
        poll_cancelled_ = true;
        polled_.stop_and_join();
        pulse_measurement_.cancel_all_and_join();
        poll_started_ = false;
        poll_cancelled_ = false;
    }

    // One tick `wait` from now, the pulse's end plus kPulseGuideSettle, beside the regular cadence: a one-shot
    // measurement slot that publishes through the module (decision 0010 rule 7). It feeds no fault latch: the 1 s
    // probes judge the link. Call with no driver lock held (decision 0006 rule 4).
    void start_pulse_measurement(std::chrono::nanoseconds wait) {
        if (status_poll_paused_for_testing_.load()) {
            return;
        }
        try {
            pulse_measurement_.start([this, wait](util::OperationContext& ctx) {
                if (!ctx.wait_for(wait)) {
                    return;
                }
                try {
                    poll_tick([&ctx] { return ctx.stop_reason() != util::StopReason::None; }, 1);  // :GU#, position
                } catch (const std::exception& ex) {
                    // IsPulseGuiding still ends at kPulseMeasureBound.
                    ALPACA_LOG_DEBUG("OnStep", std::string("Pulse-end measurement failed: ") + ex.what());
                }
            });
        } catch (const std::exception& e) {
            // A burst of PulseGuide calls can cause this (decision 0007 rule 9: INFO); IsPulseGuiding still ends at
            // kPulseMeasureBound and the 1 s ticks go on.
            ALPACA_LOG_INFO("OnStep", std::string("Pulse-end measurement did not start: ") + e.what());
        }
    }

    // Call with no driver lock held (decision 0006 rule 4), at the end of a connect, after the seed. False when the
    // poll did not start (no thread): the connect fails rather than serve its first frame for the whole session
    // (#1895). While the test seam pauses the poll it is not started.
    bool start_status_poller() {
        if (status_poll_paused_for_testing_.load()) {
            return true;
        }
        next_poll_tick_ = 0;
        skip_next_probe_ = true;  // start()'s first probe is the seed the connect published
        const bool started = polled_.start();
        skip_next_probe_ = false;
        poll_started_ = started;
        return started;
    }

    // A connect that cannot finish (#1895) is undone as a Disconnect is: Connected false and the session state
    // cleared under mutex_ before the close, which goes through Wire with mutex_ released (decision 0016 item 4).
    // The caller holds the driver locks and no poll body runs (they were joined with no lock held, decision 0006
    // rule 4). The link's loss stamp stays with the driver (release_link()), so the next Connect still times
    // the outage (#521).
    [[noreturn]] void fail_connect_locked(const std::string& reason) {
        published_connected_ = false;
        connected_ = false;
        reset_session_state_locked();
        wire().release_link();
        throw AlpacaException("Failed to connect to OnStep mount: " + reason);
    }

    // ── Site ───────────────────────────────────────────────────────────────────

    bool site_known_locked() const { return site_latitude_known_ && site_longitude_known_; }

    // `needs_site`: the member is not a site getter but uses the site (#1893).
    std::string site_unknown_message(const char* member, bool needs_site = false) const {
        return std::string(member) + (needs_site ? " needs the site, which" : "") +
               " is not known: the mount's site could not be read" +
               (site_read_error_.empty() ? std::string() : " (" + site_read_error_ + ")") +
               (needs_site ? "; write SiteLatitude and SiteLongitude" : "; write it") +
               ", or set the site in the device config";
    }

    // A member whose answer or motion uses the site (SideOfPier, DestinationSideOfPier, SiderealTime, the Alt/Az
    // slews) refuses while the pair is not known, as the site getters do, instead of using latitude and longitude 0
    // (#1893; decision 0019 rule 2, which this follows for consistency: rule 3 covers controllers that store no site).
    void throw_if_site_unknown_locked(const char* member) const {
        if (!site_known_locked()) {
            throw AlpacaException(site_unknown_message(member, true), AlpacaError::InvalidOperation);
        }
    }

    // Control path (a connect, a site write, the poll): reads :Gt#/:Gg# while
    // the pair is not known, at most every kSiteInfoRetryDelay. Once known it
    // is never read again: the site writes set it themselves. Caller holds the
    // operation lock. `probe_answered`: the poll's probe, whose sample has just
    // answered; PolledState clears a latched fault only once the probe returns,
    // so that fault does not refuse this read (#2495).
    void ensure_site_info_cached_locked(bool probe_answered = false) const {
        if (!connected_) {
            return;
        }
        // While the link fault is latched every read throws it (decision 0009
        // rule 1), and an unread site is not fetched: that fetch would wait out
        // the reply timeout and swallow the failure (#1659).
        if (!probe_answered) {
            throw_if_faulted_locked();
        }
        if (site_known_locked()) {
            return;
        }
        auto now = clock_.now();
        if (last_site_info_attempt_ != std::chrono::steady_clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSiteInfoRetryDelay) {
            return;
        }
        last_site_info_attempt_ = now;
        try {
            SiteInfo info = wire().get_site_info();
            site_latitude_cached_ = info.latitude_degrees;
            site_longitude_cached_ = info.longitude_degrees;
            site_latitude_known_ = true;
            site_longitude_known_ = true;
            site_read_error_.clear();
        } catch (const AlpacaException& ex) {
            if (ex.error_code() == AlpacaError::NotConnected) {
                throw;  // the link is lost: reported, and the state is left alone
            }
            note_site_read_failed_locked(ex.what());
        } catch (const std::exception& ex) {
            note_site_read_failed_locked(ex.what());
        } catch (...) {
            note_site_read_failed_locked("unknown error");
        }
    }

    // A failed :Gt#/:Gg# read is retried by the poll (after
    // kSiteInfoRetryDelay); it is logged once per connect (#1754). Its reason
    // is kept for the site getters' refusal.
    void note_site_read_failed_locked(const char* reason) const {
        site_read_error_ = reason;
        if (!site_read_warned_) {
            site_read_warned_ = true;
            ALPACA_LOG_WARN("OnStep", std::string("Could not read the site from the mount: ") + reason);
        }
    }

    // Body of set_utc_date with the driver locks already held — called from the
    // connect path (sync_time_on_connect_), which holds them; calling the
    // public locking method there would self-deadlock (non-recursive mutex).
    void set_utc_date_locked(std::chrono::system_clock::time_point utc) {
        check_connected();
        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        send_mount_time_locked(utc);
        utc_offset_.set(utc, clock_.now());
        mount_clock_unreadable_.reset();
        // The mount now runs on the client's clock and so does the LST above;
        // on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("OnStep", client_minus_host, client_disagreement_warned_);
    }

    // The :SL#/:SC#/:SG# write of `utc`, with the operation lock held. Publishes nothing.
    void send_mount_time_locked(std::chrono::system_clock::time_point utc) const {
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);

        // Send UTC itself as the mount's "local" time with a 0 offset,
        // rather than converting to the host's local time/DST — confirmed
        // against real hardware: with a non-zero offset, the mount's
        // internal sidereal-time (:GS#) calculation came out ~85 minutes
        // wrong (not a clean ±1h double-application of the offset either,
        // so this is a genuine firmware quirk, not just a sign error like
        // the longitude one above). Skipping the offset entirely — long
        // established practice for LX200-family mounts precisely to dodge
        // per-firmware timezone/DST bugs like this — made :GS# match the
        // correct GMST-based value to within a second.
        std::tm utc_tm{};
#ifdef _WIN32
        gmtime_s(&utc_tm, &utc_time_t);
#else
        gmtime_r(&utc_time_t, &utc_tm);  // reentrant: the re-sync body and a client's UTCDate write both land here
#endif
        TimeInfo info;
        info.hour = utc_tm.tm_hour;
        info.minute = utc_tm.tm_min;
        info.second = utc_tm.tm_sec;
        info.month = utc_tm.tm_mon + 1;
        info.day = utc_tm.tm_mday;
        info.year = (utc_tm.tm_year + 1900) % 100;
        info.utc_offset_hours = 0.0;
        wire().set_time(info);
    }

    // ── Host clock re-sync (open-astro#2419) ────────────────────────────────────────────────────────────────────

public:
    // The server's timer saw the host clock step or become trusted. With syncTimeOnConnect on, re-syncs the mount
    // clock on the clock_resync_ slot, as the connect would have. The timer thread calls this with no lock held: it
    // does no I/O, joins nothing and never waits for transition_mutex_. It records the change, then starts the
    // re-sync when it can take transition_mutex_ at once; otherwise its holder (set_connected()) starts it after
    // letting go (drain_pending_clock_resync()). AsyncOperation::start() itself can wait up to kStaleReapTimeout
    // (2 s) when three replaced bodies are still running (a blocked link, #2432).
    void on_host_clock_changed(const util::HostClockEvent& event) noexcept override {
        if (!sync_time_on_connect_) {  // fixed at construction
            return;
        }
        host_clock_gate_.owe(event);  // before the try_lock: a holder that lets go after it sees the flag
        drain_pending_clock_resync();
    }

private:
    // Starts the re-sync a host clock change left pending, unless another thread holds transition_mutex_: its one
    // holder, set_connected(), calls this after letting go, so a change recorded while the lock was held is never
    // lost. Call with no driver lock held.
    void drain_pending_clock_resync() noexcept {
        try {
            while (host_clock_gate_.owed()) {
                std::unique_lock<std::mutex> transition(transition_mutex_, std::try_to_lock);
                if (!transition.owns_lock()) {
                    return;  // its holder drains as it lets go
                }
                if (!host_clock_gate_.take_owed()) {
                    return;
                }
                start_clock_resync_transition_locked();
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): a timer-thread hook must not throw
        }
    }

    // Under transition_mutex_ and no other driver lock: starts a re-sync while the mount is connected (a change seen
    // while disconnected is dropped). The UTCDate write generation is taken here, where the re-sync is decided. A
    // refused start() (a slot still draining, a refused thread, bodies stuck on a blocked link) is logged; the next
    // host clock change tries again.
    void start_clock_resync_transition_locked() noexcept {
        try {
            if (!published_connected_.load()) {
                return;
            }
            std::string why = host_clock_gate_.owed_why();
            const std::uint64_t decided_at = utc_write_generation_.load();
            clock_resync_.start([this, why = std::move(why), decided_at](util::OperationContext& ctx) {
                run_clock_resync(ctx, why, decided_at);
            });
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("OnStep", std::string("Could not start the mount clock re-sync: ") + e.what());
        } catch (...) {
            ALPACA_LOG_WARN("OnStep", "Could not start the mount clock re-sync");
        }
    }

    // Under the operation lock: motion the driver tracks, during which the mount clock is left alone (OnStepX aims a
    // GOTO by it): a GOTO (its grace, or the frame), a MoveAxis, a guide move or pulse guide, a home, a park, a
    // Park/FindHome still to finish, or a GOTO, park or home still watched for its end (#2296: from its dispatch, and
    // through a relink that leaves it running, until a status shows it ended, at a limit stop or not).
    bool clock_resync_blocked_by_motion_locked() const {
        const auto now = clock_.now();
        const bool pulsing = (pulse_guiding_active_[0] && pulse_hold_[0].active(now)) ||
                             (pulse_guiding_active_[1] && pulse_hold_[1].active(now));
        return moving_locked() || slew_hold_.active(now) || park_home_request_.has_value() ||
               watched_motion_.has_value() || frame_status_locked().is_parking || pulsing;
    }

    // The re-sync body. Each attempt holds the operation lock from the motion check through the write and its
    // publication, and every motion command takes that lock, so no motion the driver starts can begin between the
    // check and the write. Under motion it WAITS (util::kHostClockResyncMotionPoll steps on the task clock,
    // cancellable by a disconnect or a newer re-sync) for up to util::kHostClockResyncMotionBound, then gives up and
    // leaves the mount clock to the next host clock change or a client UTCDate write. A client UTCDate write since the
    // re-sync was decided (`decided_at`) wins: nothing is sent. A failed write is logged and leaves the mount clock; it
    // counts toward nothing but what the wire itself records.
    void run_clock_resync(util::OperationContext& ctx, const std::string& why, std::uint64_t decided_at) {
        {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
                hook = clock_resync_before_lock_hook_;
            }
            if (hook) {
                hook();  // test seam: no driver lock held
            }
        }
        // Asked once, with no driver lock held (the kernel probe); the latch never clears. A step the router did not
        // trust leaves the mount clock as it is.
        if (!util::HostClockTrust::trusted()) {
            return;
        }
        const auto give_up_at = clock_.now() + util::kHostClockResyncMotionBound;
        bool waited = false;
        while (true) {
            {
                StateLock lock(lock_gate_);
                if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                    return;
                }
                if (utc_write_generation_.load() != decided_at) {
                    ALPACA_LOG_INFO("OnStep", why + "; a client wrote UTCDate since, so the mount keeps that time");
                    return;
                }
                if (link_faulted()) {
                    ALPACA_LOG_WARN("OnStep", why + "; the mount is not answering, so its clock is left alone");
                    return;
                }
                if (!clock_resync_blocked_by_motion_locked()) {
                    const auto host_utc = std::chrono::system_clock::now();
                    const auto sent_at = clock_.now();
                    try {
                        send_mount_time_locked(host_utc);
                    } catch (const std::exception& ex) {
                        ALPACA_LOG_WARN("OnStep", why +
                                                      "; the mount clock re-sync failed, the mount keeps its own "
                                                      "clock: " +
                                                      ex.what());
                        return;
                    }
                    // Checked again where the re-sync publishes: the write above held the operation lock, which a
                    // client's UTCDate write takes too, so it cannot have landed in between.
                    if (utc_write_generation_.load() == decided_at) {
                        utc_offset_.set(host_utc, sent_at);
                        mount_clock_unreadable_.reset();
                    }
                    ALPACA_LOG_INFO("OnStep", why + "; mount clock re-synced to the host clock");
                    return;
                }
            }
            if (clock_.now() >= give_up_at) {
                ALPACA_LOG_WARN("OnStep", why +
                                              "; the mount kept moving, so its clock is left alone: the next host "
                                              "clock change or a client UTCDate write sets the mount clock");
                return;
            }
            if (!waited) {
                ALPACA_LOG_INFO("OnStep", why +
                                              "; the mount is moving, the mount clock re-sync waits for the "
                                              "motion to end");
                waited = true;
            }
            if (!ctx.wait_for(util::kHostClockResyncMotionPoll)) {
                return;
            }
        }
    }

    // The UTC the LST computations use: a written or mount-read UTCDate, else
    // the host clock. The host clock is not stored in utc_offset_, so UTCDate
    // never reports it as the mount's (#1478).
    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (const auto utc = utc_offset_.current(clock_.now())) {
            return *utc;
        }
        return std::chrono::system_clock::now();
    }

    // Decision 0005: every wait, sleep and deadline of this driver (#1378),
    // and the wrapper's link-loss stamps (set_task_clock() at connect).
    util::TaskClock& clock_;
    int device_number_;
    ConnectionInfo connection_info_;
    // Set by the auto-detect factory; empty for an explicit port or host.
    // connection_resolved_ is true once a connect has run the resolver, so a
    // later connect retries that endpoint before scanning again (#659).
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    // Decision 0016: see the lock-order comment at the top of the class. Both are mutable: a const helper
    // that reaches the wire (Wire::call) needs the operation lock already held and drops and retakes mutex_.
    // set_connected() against set_connected(); no getter takes it. Under it, set_connected() joins the
    // status-poll and pulse-end slots (stop_status_poller(), on a connect, a disconnect and a relink) and only cancels
    // the others (cancel() never blocks). No slot body takes it or calls set_connected(), so the join ends.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    // Which of the two this thread holds, and the exchanges that drop mutex_ (#1729).
    util::LockGate lock_gate_{io_mutex_, mutex_, "OnStep"};
    // The controller's shared link (#2223), taken at connect and let go at a Disconnect, a relink or a failed
    // connect; a lost one is kept until then, with its reason (decision 0009 rule 5). Atomic, so Connected and the
    // getters read it lock-free; replaced only under transition_mutex_ and io_mutex_.
    mutable std::atomic<std::shared_ptr<OnStepProtocolWrapper>> link_;
    // The loss and last-alive stamps of the link last let go of, on clock_ (#521, #1241): the next connect's outage
    // check reads and clears them. Under io_mutex_ (the connect paths), or the destructor.
    mutable std::optional<std::chrono::steady_clock::time_point> carried_lost_at_;
    mutable std::optional<std::chrono::steady_clock::time_point> carried_alive_at_;
    // Read lock-free by check_connected(); written under io_mutex_ and mutex_.
    std::atomic<bool> connected_;
    // What get_connected() reports, read lock-free (#910). Raised only once a
    // connect has finished, so Connected never reads true while Connecting
    // does; dropped before any disconnect. Written under io_mutex_ and mutex_.
    std::atomic<bool> published_connected_{false};
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    // open-astro#346 (the shape #304 fixed on the Sky-Watcher driver): ASCOM
    // treats the two target properties as independent, so each must throw
    // ValueNotSet until that property itself has been written. One shared flag
    // let a write to either unlock both, and a client reading the one it did
    // not set got a default 0 instead of an error. The paths that legitimately
    // define both coordinates at once -- the slew and sync coordinate forms,
    // the position-override and arrival reads, and the connect/disconnect
    // resets -- still set or clear both.
    mutable bool target_ra_set_ = false;
    mutable bool target_dec_set_ = false;
    // Bumped by each target write (under mutex_); see store_target_locked().
    std::uint64_t target_ra_writes_ = 0;
    std::uint64_t target_dec_writes_ = 0;

    std::atomic<double> aperture_diameter_m_ = 0.0;
    std::atomic<double> focal_length_m_ = 0.0;

    // The site, each coordinate known once the config, a client write or a :Gt#/:Gg# read set it (#1783); the
    // getters throw until then. Under mutex_.
    mutable double site_latitude_cached_ = 0.0;
    mutable double site_longitude_cached_ = 0.0;
    mutable bool site_latitude_known_ = false;
    mutable bool site_longitude_known_ = false;
    mutable std::string site_read_error_;    // why the last :Gt#/:Gg# read failed, for the getters' refusal
    mutable bool site_read_warned_ = false;  // a failed site read was logged (#1754); under mutex_
    mutable std::chrono::steady_clock::time_point last_site_info_attempt_;
    std::atomic<double> site_elevation_m_;  // NaN until set or configured

    // Decision 0014: the UTCDate a client wrote, or the mount clock the connect
    // read (seed_utc_from_mount_locked()), and the task-clock time since.
    // Cleared on connect and disconnect. Under mutex_.
    mutable util::UtcOffset utc_offset_;
    // Why the connect could not read the mount clock (#1478); under mutex_,
    // cleared on connect, disconnect and a UTCDate write.
    std::optional<std::string> mount_clock_unreadable_;

    // Decision 0022: Slewing reads true while it is active, kSlewForceGrace from the :MS#. Under mutex_.
    mutable util::HoldWindow slew_hold_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    // The running MoveAxis per axis, for the re-send (#1378). Under mutex_.
    int manual_axis_dir_[2] = {0, 0};
    double manual_axis_rate_[2] = {0.0, 0.0};
    mutable bool manual_axis_resend_[2] = {false, false};
    std::chrono::steady_clock::time_point manual_axis_resend_due_[2]{};
    // :GU# has shown 'g' since this connect, so its absence ends a MoveAxis (#1378).
    mutable bool firmware_reports_guide_move_ = false;
    // A real :GU# read has landed since the last MoveAxis start (#1378).
    mutable bool status_read_since_move_ = false;
    // Set only by a coordinate slew; consumed when that slew completes (#1218).
    mutable bool tracking_before_slew_ = false;
    // #2406: kGotoTimeout from the last GOTO's :MS#; kept after it lands, since the mount may start a GOTO only after
    // a frame that shows none, and cleared at the deadline, by the next motion command, an AbortSlew and a connect. A
    // fail-safe timeout, not a hold window: Slewing's hold stays slew_hold_ (decision 0022 rule 1). Under mutex_.
    mutable std::optional<std::chrono::steady_clock::time_point> goto_deadline_;
    // #2406: the last GOTO's target, until a frame with no GOTO after one that showed it running judges its landing
    // (judge_landing_locked()); cleared like goto_deadline_ otherwise. Under mutex_.
    mutable std::optional<util::MountPosition> goto_target_;
    // #2406: no frame has shown the GOTO end since its :MS#. Under mutex_.
    mutable bool goto_running_ = false;
    // #2406: a :GU# showed the GOTO running since its :MS# (not the write-through), so its end is judged. Under
    // mutex_.
    mutable bool goto_seen_slewing_ = false;
    // #2406: a sync went out since the connect, so the next one is bounded (sync_locked()). Under mutex_.
    bool synced_this_session_ = false;
    // #2406: a motion the driver had to stop itself (a GOTO past kGotoTimeout, a MoveAxis past its cap, a mount
    // still moving after AbortSlew's last round). Slewing raises it as DriverException until the next motion
    // command, AbortSlew or connect (an async failure keeps its error, ascom-contract). Under mutex_.
    mutable std::string bound_error_;
    // #2406: the AbortSlew stop rounds sent while :GU# still shows motion; 0 when no stop is being checked.
    mutable int abort_stop_rounds_ = 0;
    mutable std::chrono::steady_clock::time_point abort_next_round_{};

    bool does_refraction_ = false;
    // From :GXEE# at connect on OnStepX; empty when unknown or disconnected.
    std::optional<bool> refraction_from_firmware_;
    // OnStepX's pier-side settings, read at connect and by the poll (#2179); nullopt while unknown. `onstepx_` is
    // whether :GVN# numbered the firmware 10 or later. Both under mutex_, cleared with the session state.
    std::optional<PierSettings> pier_settings_;
    bool onstepx_ = false;
    // Whether :GVN# gave a version at connect, so a false onstepx_ means OnStep 4.x rather than unknown firmware.
    bool firmware_identified_ = false;
    // The rate state (see tracking_rate_served_locked()), under mutex_, cleared with the session state: the last
    // known DriveRate, the rate offsets as the mount holds them (arcseconds per sidereal second), the pulse-guide
    // rate as a multiple of sidereal (:GX90#; empty while unknown), the :GU# pulse-guide digit it was read for, and
    // the frame fields the offsets were last read for.
    // mount_type_seen_: the :GU# mount type last seen since the connect (empty until a status has been), with
    // tracking_rate_known_ the mount settings the getters serve (decision 0010 OnStep row, #2214).
    mutable int tracking_rate_known_ = 0;
    mutable std::optional<MountType> mount_type_seen_;
    double rate_offset_ra_arcsec_ = 0.0;
    double rate_offset_dec_arcsec_ = 0.0;
    // The rate-offset model's anchor (#2241, offset_model_position_locked()); empty while no model runs. Under mutex_,
    // cleared with the rate state.
    mutable std::optional<OffsetAnchor> offset_anchor_;
    // The task-clock time of the model's last driver-side change (a re-anchor, a sync, a drop): a frame whose
    // :GRa#/:GDe# read is older never changes the model (adopt_offset_model_frame_locked()). Under mutex_, cleared
    // with the rate state.
    mutable util::TaskClock::clock::time_point offset_model_floor_{};
    std::optional<double> guide_rate_multiple_;
    int guide_rate_digit_read_ = -1;
    std::optional<RateWatch> rate_watch_;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;

    // Per axis (0 East/West, 1 North/South): a pulse is pending, and its hold window for the pulse's duration
    // (decision 0022). IsPulseGuiding is true while either axis's pulse runs or is not yet measured. Under mutex_.
    mutable bool pulse_guiding_active_[2] = {false, false};
    mutable std::array<util::HoldWindow, 2> pulse_hold_;
    // Numbers each sample (PolledFrame::sample_seq); under mutex_, taken with the operation lock held.
    mutable std::uint64_t sample_seq_ = 0;

    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so
    // the get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;
    // The name read at connect ("OnStep " + the :GVC# description), under firmware_mutex_; kept across a disconnect.
    std::string model_cache_;

    // Last, so it is destroyed (and its body joined) before every member the
    // body touches. The destructor joins it explicitly as well.
    util::OperationGeneration move_axis_generation_;
    mutable util::AsyncOperation move_axis_resend_{"OnStep MoveAxis re-send", move_axis_generation_, clock_};
    // Issue #2651: the 60 s MoveAxis cap of each axis (run_move_axis_cap()); a generation of its own per axis, so a
    // renewal on one axis never supersedes the other's body.
    std::array<util::OperationGeneration, 2> move_axis_cap_generation_;
    // The window of each axis's latest move (under mutex_; cleared with the manual motion), and the lock that holds
    // start_move_axis_cap()'s read and start together. Taken with no driver lock held.
    mutable util::HoldWindow move_axis_cap_window_[2];
    std::mutex move_axis_cap_start_mutex_[2];
    mutable std::array<util::AsyncOperation, 2> move_axis_cap_{
        {{"OnStep MoveAxis cap 0", move_axis_cap_generation_[0], clock_},
         {"OnStep MoveAxis cap 1", move_axis_cap_generation_[1], clock_}}};

    // A motion's failure: a refusal the park_home_ body met (#1430), or a GOTO, park or home the mount stopped
    // (#2296, note_motion_end_locked()). Raised by Slewing and by AtPark (a park's) or AtHome (a home's) until the
    // next motion command or a reconnect. Under mutex_.
    struct KeptMotionError {
        MotionKind motion;
        std::string message;
        int code;
    };
    mutable std::optional<KeptMotionError> motion_error_;
    // #2296: the GOTO, park or home the driver started that no status has yet shown ended (note_motion_end_locked()).
    // `who` names the member (a string literal); `started_at` starts the park and home bound (`bound`; a 'w'
    // status restarts it); `target` is a coordinate GOTO's RA (hours) and Dec, `target_side` a flip's pier side (-1
    // none); `saw_motion` and `saw_switch_home`: a status has shown it running, and running as a home with switches;
    // `commanded_at` and `fixed_hour_angle`: when the GOTO was sent, and that it started with tracking off, so both
    // firmwares hold its target's hour angle, not its RA (OnStepX Goto.cpp moves the target only while tracking; OnStep
    // 4.x Astro.ino/OnStep.ino likewise); `bound`: the park and home bound, park_home_bound_ when it was taken. Under
    // mutex_.
    struct WatchedMotion {
        MotionKind motion;
        const char* who;
        util::TaskClock::clock::time_point started_at;
        std::optional<std::pair<double, double>> target;
        int target_side;
        bool saw_motion;
        bool saw_switch_home;
        util::TaskClock::clock::time_point commanded_at;
        bool fixed_hour_angle;
        std::chrono::seconds bound;
    };
    mutable std::optional<WatchedMotion> watched_motion_;
    // #2296: the park and home bound a new watch takes (seed_park_home_bound_locked(), from the base slew rate read at
    // connect; kParkHomeBoundFloor until then). A watch keeps its own, so a relink that resets this one before it
    // judges the carried watch does not shorten it. Under mutex_.
    std::chrono::seconds park_home_bound_{kParkHomeBoundFloor};
    // #2296: whether this mount's FindHome centres a tangent arm or a sector gear (read_home_geometry_locked()), so a
    // home is not watched; unknown until a FindHome that sends a home reads both flags. Under mutex_.
    std::optional<bool> home_moves_one_axis_;
    // The standing stop code and the keys already logged at WARN with it (log_motion_stop_locked(), decision 0007 rule
    // 9): a repeat of one of those keys while the code stands is logged at INFO. Under mutex_.
    mutable int warned_stop_code_ = 0;
    mutable std::vector<std::string> warned_stop_keys_;
    // True while AbortSlew's own :GU# read judges the watched slew: a failure there is logged at INFO (#2296).
    // Under mutex_.
    mutable bool judging_abort_ = false;
    // #2522: who owns the axes (util::MotionOwner, decision 0006 rule 3), under mutex_. A slew, Park or FindHome
    // claims both axes, a non-zero MoveAxis and a PulseGuide their own; AbortSlew, Unpark, a disconnect or relink and
    // MoveAxis(axis, 0) cancel. The controller runs every GOTO, park and home, so the slew, Park or FindHome claim is
    // kept in slew_claim_ and finished by the driver (end_slew_claim_if_ended_locked() and the paths above it).
    // `taken`: the mount took the command, so a status that shows no motion ends the claim.
    struct SlewClaim {
        util::MotionClaim claim;
        MotionKind kind;
        bool taken;
    };
    mutable util::MotionOwner motion_owner_;
    mutable std::optional<SlewClaim> slew_claim_;
    // Gives up the slew claim `claim` on the way out of a dispatch unless the mount took it or the Park/FindHome body
    // was handed it (handed_off). Destroyed with mutex_ held.
    struct UntakenSlewClaim {
        const OnStepTelescopeDriver* driver = nullptr;
        util::MotionClaim claim;
        bool handed_off = false;
        ~UntakenSlewClaim() {
            if (!handed_off) {
                driver->release_untaken_slew_claim_locked(claim);
            }
        }
    };
    // The current Park/FindHome's request: its claim and what its background part is still to finish. Under mutex_.
    struct ParkHomeRequest {
        util::MotionClaim claim;
        const char* who;
        bool home;
        bool stopped;
        int code;
    };
    std::optional<ParkHomeRequest> park_home_request_;
    // Test seam: runs between the unlock and park_home_.start(). Set only
    // while no Park or FindHome is in flight.
    std::function<void()> park_home_before_start_;
    // Test seams: run before a clean disconnect's close, and in a poll tick between its sample and its
    // publish (a leaf lock of their own guards both).
    mutable std::mutex disconnect_hook_mutex_;
    std::function<void()> disconnect_before_close_hook_;
    std::function<void()> status_poll_after_sample_hook_;
    std::function<void()> pulse_guide_gap_hook_;
    std::function<bool()> move_axis_resend_before_send_hook_;
    std::function<void()> move_axis_send_hook_;
    std::function<void()> move_axis_cap_before_start_hook_;
    // Test seam: runs at the start of the host clock re-sync body, with no driver lock held (#2419).
    std::function<void()> clock_resync_before_lock_hook_;
    std::function<void()> connect_before_state_lock_hook_;
    std::function<void()> connected_before_transition_unlock_hook_;
    // The tail of a Park or FindHome that must first wait for motion to end
    // (#1430). Declared last with the re-send slot, for the same reason.
    util::OperationGeneration park_home_generation_;
    mutable util::AsyncOperation park_home_{"OnStep Park/FindHome", park_home_generation_, clock_};
    // Decision 0010 (#1783, #2495): the frame the getters and DeviceState serve, with the status poll, its fault
    // latch and its loss bound, in util::PolledState. One poll body per connection, started at the end of a connect
    // (and by the test pause seam, never after a pulse guide) and joined before a disconnect or relink closes the
    // port. The probe, the pulse-end tick and the synchronous slew's end publish through it, ordered by sample_seq.
    // Its mutexes are leaves under mutex_. Declared after every member the probe touches.
    std::atomic<bool> status_poll_paused_for_testing_{false};
    // The probe's tick number: every second one reads Alt/Az, every tenth the pier settings. Reset before each start
    // and touched by the probe only, on the poll thread.
    int next_poll_tick_ = 0;
    // The seed is start()'s first probe (start_status_poller()).
    std::atomic<bool> skip_next_probe_{false};
    // True while stop_status_poller() joins the bodies: a probe in its sample gives no verdict and publishes nothing.
    std::atomic<bool> poll_cancelled_{false};
    std::atomic<bool> poll_started_{false};  // for status_poll_running_for_testing()
    util::OperationGeneration status_poll_generation_;
    mutable util::PolledState<PolledFrame> polled_{
        clock_,
        status_poll_generation_,
        {kStatusPollPeriod, kLinkFaultThreshold, kSnapshotMaxAge, "OnStep mount"},
        [this] { return probe(); },
        [this] { declare_poll_loss(); }};
    // The extra tick at the end of a pulse guide (#1783); a newer pulse replaces it.
    util::OperationGeneration pulse_measurement_generation_;
    mutable util::AsyncOperation pulse_measurement_{"OnStep pulse-end measurement", pulse_measurement_generation_,
                                                    clock_};
    // open-astro#2419: a host clock change on_host_clock_changed() has not acted on yet (owed), and what it was.
    // Dropped where a connect decides on the host clock; drained by the hook itself, or as set_connected() lets go.
    // Also the connect's untrusted-clock warning (#2480).
    util::HostClockWriteGate host_clock_gate_{"OnStep"};
    // Bumped by each client UTCDate write that landed (under the operation lock); a re-sync decided before it sends
    // nothing. Read lock-free where the re-sync is decided.
    std::atomic<std::uint64_t> utc_write_generation_{0};
    // The host clock re-sync body (#2419), started under transition_mutex_ and joined under it by a disconnect, a
    // relink and every connect, with no other driver lock held. Last: its body touches the members above.
    util::OperationGeneration clock_resync_generation_;
    util::AsyncOperation clock_resync_{"OnStep host clock re-sync", clock_resync_generation_, clock_};
};

std::unique_ptr<TelescopeDriver> create_onstep_telescope(int device_number, const ConnectionInfo& connection_info,
                                                         util::TaskClock& clock) {
    return create_onstep_telescope_with_site(device_number, connection_info, std::nullopt, std::nullopt, std::nullopt,
                                             std::nullopt, clock);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock) {
    return std::make_unique<OnStepTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                   util::ConnectionResolver<ConnectionInfo>{}, clock);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect) {
    if (!connection_resolver) {
        throw AlpacaException("OnStep telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<OnStepTelescopeDriver>(device_number, ConnectionInfo{}, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                   std::move(connection_resolver));
}

ConnectionInfo resolve_onstep_serial_auto(int mount_index) {
    auto ports = enumerate_onstep_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("OnStep mount"));
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                              std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("OnStep", "Auto-detected mount on " + port.port_path + " (fw " + port.version_string + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;
    return conn;
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_auto(int device_number, int mount_index,
                                                              std::optional<double> site_latitude_deg,
                                                              std::optional<double> site_longitude_deg,
                                                              std::optional<double> site_elevation_m,
                                                              std::optional<bool> sync_time_on_connect) {
    // The serial scan runs at connect time (#659), not here.
    return create_onstep_telescope_deferred(
        device_number, [mount_index] { return resolve_onstep_serial_auto(mount_index); }, site_latitude_deg,
        site_longitude_deg, site_elevation_m, sync_time_on_connect);
}

void set_park_home_before_start_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_park_home_before_start_for_testing(std::move(hook));
    }
}

void set_disconnect_before_close_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_disconnect_before_close_hook_for_testing(std::move(hook));
    }
}

void set_move_axis_send_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_send_hook_for_testing(std::move(hook));
    }
}

void set_move_axis_cap_before_start_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_cap_before_start_hook_for_testing(std::move(hook));
    }
}

void set_move_axis_resend_before_send_hook_for_testing(TelescopeDriver& driver, std::function<bool()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_resend_before_send_hook_for_testing(std::move(hook));
    }
}

void set_clock_resync_before_lock_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_clock_resync_before_lock_hook_for_testing(std::move(hook));
    }
}

void set_connect_before_state_lock_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_connect_before_state_lock_hook_for_testing(std::move(hook));
    }
}

void set_connected_before_transition_unlock_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_connected_before_transition_unlock_hook_for_testing(std::move(hook));
    }
}

bool clock_resync_running_for_testing(const TelescopeDriver& driver) {
    const auto* onstep = dynamic_cast<const OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->clock_resync_running_for_testing();
}

void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_status_poll_paused_for_testing(paused);
    }
}

bool poll_status_now_for_testing(TelescopeDriver& driver, bool full) {
    auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->poll_status_now_for_testing(full);
}

bool poll_status_tick_for_testing(TelescopeDriver& driver, int tick) {
    auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->poll_status_tick_for_testing(tick);
}

bool status_poll_running_for_testing(const TelescopeDriver& driver) {
    const auto* onstep = dynamic_cast<const OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->status_poll_running_for_testing();
}

void set_status_poll_after_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_status_poll_after_sample_hook_for_testing(std::move(hook));
    }
}

bool pulse_measurement_running_for_testing(const TelescopeDriver& driver) {
    const auto* onstep = dynamic_cast<const OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->pulse_measurement_running_for_testing();
}

void set_pulse_guide_gap_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_pulse_guide_gap_hook_for_testing(std::move(hook));
    }
}

void set_move_axis_resend_spawn_for_testing(TelescopeDriver& driver,
                                            std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_resend_spawn_for_testing(std::move(spawn));
    }
}

void set_move_axis_cap_spawn_for_testing(TelescopeDriver& driver,
                                         std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_cap_spawn_for_testing(std::move(spawn));
    }
}

void set_park_home_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_park_home_spawn_for_testing(std::move(spawn));
    }
}

void set_status_poll_spawn_for_testing(TelescopeDriver& driver,
                                       std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_status_poll_spawn_for_testing(std::move(spawn));
    }
}

}  // namespace alpacacore::vendor::onstep

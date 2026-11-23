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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/pointing.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// The telescope command gate (#2258): the checks a mount runs before a command, in one order for every mount
// driver. A member refuses in this order: argument (InvalidValue), connected, parked, link fault, tracking (#2257).
//
// Families: PulseGuide, MoveAxis, the equatorial slews and the guide-rate setters (the members #2257 probes),
// TrackingRate, DestinationSideOfPier, the target setters, the rate offsets and the Alt/Az slews with SyncToAltAz
// (#2418). SyncToCoordinates joins next.
//
// Arguments are data. Each family's `_arguments` check takes the arguments and the driver's TelescopeCommandLimits
// only: the ranges and the InvalidValue texts are fields of the limits (each driver keeps its own words), so a driver
// replaces its argument helpers with a limits value. It reads no state, so a driver may run it before it takes any
// lock. The RA and Dec of a slew go through util::validate_ra_dec(), so their text is the same on every mount.
//
// State reads stay the driver's. Connected, parked, link fault and Tracking each read the mount or its link (OnStep's
// parked check reads :GU# when no status has been read since the connect; a fault check closes a link stale past
// util::kLinkStalenessBound; a Tracking check may ask the handset), so the `_state` check calls the driver's own
// checks in the order, through a checks object of the driver's (no base class, no virtual member: any type with
// these members, each throwing the driver's own AlpacaException or returning):
//
//   void check_connected();          // NotConnected
//   void check_not_parked(bool stop);  // InvalidWhileParked
//   void check_no_fault();           // DriverException (decision 0009 rule 1)
//   void check_tracking();           // InvalidOperation (equatorial slews only)
//   void check_sidereal_rate();      // InvalidOperation unless TrackingRate is driveSidereal (rate offsets only)
//   void check_site();               // the driver's refusal while the site is unknown (Alt/Az members only)
//   void check_not_tracking();       // InvalidOperation while Tracking is true (Alt/Az slews only)
//
// The gate itself does no I/O, takes no lock and keeps no state (decision 0016 rules 1 and 2): a driver calls it
// where it ran its checks before, holding the lock its check callbacks expect. That is its state lock when the
// callbacks read state under a lock the caller holds, and no lock when each callback takes the state lock itself.
// Only the parked-against-fault order (and the Alt/Az site check's place) and the protocol limits differ per driver;
// both are data in TelescopeCommandLimits, never a vendor name in here.

namespace alpacacore::util {

/// The axis of a guide-rate write.
enum class GuideRateAxis : std::uint8_t { RightAscension, Declination };

/// The axis of a rate-offset write: RightAscensionRate or DeclinationRate.
enum class RateOffsetAxis : std::uint8_t { RightAscension, Declination };

/// Which of the parked and link-fault checks a member family runs first. #2257 leaves this order as each driver has
/// it, so it is per driver and member family. NoFaultCheck: the family runs the parked check only, because the
/// driver refuses that command on a faulted link through the wire, not through its latched fault.
enum class ParkedFaultOrder : std::uint8_t { ParkedFirst, FaultFirst, NoFaultCheck };

/// The InvalidValue texts of the argument checks, in the driver's own words. Two fields may hold one text. Each
/// field points at a string literal (static storage): the gate keeps the pointer, never a copy.
struct TelescopeArgumentTexts {
    const char* pulse_direction = "Invalid PulseGuide direction";
    const char* pulse_duration = "PulseGuide duration is out of range";
    const char* move_axis_axis = "Invalid MoveAxis axis";
    const char* move_axis_rate_not_finite = "MoveAxis rate must be finite";
    const char* move_axis_rate_range = "MoveAxis rate exceeds supported range";
    const char* guide_rate_not_finite = "GuideRate must be a finite number";
    const char* guide_rate_range = "GuideRate is out of range";
    const char* tracking_rate = "TrackingRate is not a supported DriveRates value";
    const char* right_ascension_rate_not_finite = "RightAscensionRate must be finite";
    const char* right_ascension_rate_range = "RightAscensionRate is out of range";
    const char* declination_rate_not_finite = "DeclinationRate must be finite";
    const char* declination_rate_range = "DeclinationRate is out of range";
    const char* altitude_range = "Altitude out of range";
    const char* azimuth_range = "Azimuth out of range";
    /// True: an Alt/Az text follows the member's name and a space ("SlewToAltAz altitude must be ...").
    bool alt_az_texts_name_member = false;
};

/// The largest double under 1e-9, for a driver whose MoveAxis stop is |rate| < 1e-9 (strictly under): the gate's
/// stop test is <=, so this epsilon keeps 1e-9 itself a move (ZWO #2415, Bisque #2582). It is
/// std::nextafter(1e-9, 0.0), which is not constexpr before C++23 (GCC folds it, clang rejects it). A driver whose
/// stop is |rate| <= 1e-9 uses 1e-9 itself.
inline constexpr double kMoveAxisStopBelow1e9 = 0x1.12e0be826d694p-30;
// One ulp under 1e-9 (in [2^-30, 2^-29) a double's ulp is 2^-82), so nothing lies between it and 1e-9.
static_assert(kMoveAxisStopBelow1e9 < 1e-9 && 1e-9 - kMoveAxisStopBelow1e9 == 0x1p-82,
              "kMoveAxisStopBelow1e9 is the largest double under 1e-9");

/// A driver's protocol limits, orders and argument texts. Every range defaults to "no limit"; the PulseGuide
/// direction range (0-3) and the lower bounds (duration 0, axis 0) are universal and not fields.
struct TelescopeCommandLimits {
    int pulse_guide_max_ms = std::numeric_limits<int>::max();
    ParkedFaultOrder pulse_guide_order = ParkedFaultOrder::ParkedFirst;

    int move_axis_axes = 3;  ///< MoveAxis takes axes 0 .. move_axis_axes - 1 (ITelescopeV4 names three)
    double move_axis_max_rate = std::numeric_limits<double>::infinity();  ///< |rate|, degrees per second
    double move_axis_stop_epsilon = 0.0;  ///< |rate| at or under this is a stop; kept per driver (not unified)
    ParkedFaultOrder move_axis_order = ParkedFaultOrder::ParkedFirst;
    /// False: a MoveAxis stop skips the fault check, because the driver tries the stop on a faulted link (and
    /// checks the fault itself once the stop is sent, where it does).
    bool stop_checks_fault = true;

    ParkedFaultOrder slew_order = ParkedFaultOrder::ParkedFirst;

    /// Guide-rate writes take a value in [min, max] degrees per second, per axis (a range that depends on runtime
    /// state, such as a calibrated scale known only once connected, stays the driver's check after the connection).
    double guide_rate_ra_min = -std::numeric_limits<double>::infinity();
    double guide_rate_ra_max = std::numeric_limits<double>::infinity();
    double guide_rate_dec_min = -std::numeric_limits<double>::infinity();
    double guide_rate_dec_max = std::numeric_limits<double>::infinity();

    int tracking_rate_max = std::numeric_limits<int>::max();  ///< TrackingRate takes 0 .. tracking_rate_max

    /// A rate-offset write takes a value whose product with the axis's scale (the client's unit to the mount's) is
    /// finite and at most rate_offset_max in magnitude, in the mount's unit. By default the scale is 1 and there is
    /// no limit, so only a non-finite value is refused.
    double rate_offset_ra_scale = 1.0;
    double rate_offset_dec_scale = 1.0;
    double rate_offset_max = std::numeric_limits<double>::infinity();

    /// The Alt/Az slews and SyncToAltAz. Altitude takes [-90, 90] and azimuth [0, 360], or [0, 360) when
    /// alt_az_azimuth_360 is false. alt_az_site_first: the site check runs right after the connection check, before
    /// the parked and fault checks, instead of after them.
    bool alt_az_azimuth_360 = true;
    ParkedFaultOrder alt_az_order = ParkedFaultOrder::ParkedFirst;
    bool alt_az_site_first = false;

    TelescopeArgumentTexts texts;
};

namespace telescope_gate_detail {

[[noreturn]] inline void refuse(const char* text) { throw AlpacaException(text, AlpacaError::InvalidValue); }

template <typename Checks>
void parked_and_fault(ParkedFaultOrder order, bool stop, bool check_fault, Checks&& checks) {
    if (order == ParkedFaultOrder::FaultFirst && check_fault) {
        checks.check_no_fault();
    }
    checks.check_not_parked(stop);
    if (order == ParkedFaultOrder::ParkedFirst && check_fault) {
        checks.check_no_fault();
    }
}

}  // namespace telescope_gate_detail

// PulseGuide: direction, duration, then connected, parked and fault in the driver's order.

inline void check_pulse_guide_arguments(const TelescopeCommandLimits& limits, int direction, int duration_ms) {
    if (direction < 0 || direction > 3) {
        telescope_gate_detail::refuse(limits.texts.pulse_direction);
    }
    if (duration_ms < 0 || duration_ms > limits.pulse_guide_max_ms) {
        telescope_gate_detail::refuse(limits.texts.pulse_duration);
    }
}

template <typename Checks>
void check_pulse_guide_state(const TelescopeCommandLimits& limits, Checks&& checks) {
    checks.check_connected();
    telescope_gate_detail::parked_and_fault(limits.pulse_guide_order, /*stop=*/false, /*check_fault=*/true, checks);
}

template <typename Checks>
void check_pulse_guide(const TelescopeCommandLimits& limits, int direction, int duration_ms, Checks&& checks) {
    check_pulse_guide_arguments(limits, direction, duration_ms);
    check_pulse_guide_state(limits, checks);
}

// MoveAxis: axis, a finite rate, its magnitude, then connected, parked and fault in the driver's order. A stop (a
// rate at or under the driver's epsilon) passes `stop` to the parked check and skips the fault check unless the
// driver's limits say a stop checks it.

/// True when `rate` is a stop for this driver. Call after the arguments passed (a NaN rate is refused there).
inline bool is_move_axis_stop(const TelescopeCommandLimits& limits, double rate) {
    return std::abs(rate) <= limits.move_axis_stop_epsilon;
}

inline void check_move_axis_arguments(const TelescopeCommandLimits& limits, int axis, double rate) {
    if (axis < 0 || axis >= limits.move_axis_axes) {
        telescope_gate_detail::refuse(limits.texts.move_axis_axis);
    }
    if (!std::isfinite(rate)) {
        telescope_gate_detail::refuse(limits.texts.move_axis_rate_not_finite);
    }
    if (std::abs(rate) > limits.move_axis_max_rate) {
        telescope_gate_detail::refuse(limits.texts.move_axis_rate_range);
    }
}

template <typename Checks>
void check_move_axis_state(const TelescopeCommandLimits& limits, bool stop, Checks&& checks) {
    checks.check_connected();
    telescope_gate_detail::parked_and_fault(limits.move_axis_order, stop, !stop || limits.stop_checks_fault, checks);
}

/// Runs both halves and returns whether the call is a stop.
template <typename Checks>
bool check_move_axis(const TelescopeCommandLimits& limits, int axis, double rate, Checks&& checks) {
    check_move_axis_arguments(limits, axis, rate);
    const bool stop = is_move_axis_stop(limits, rate);
    check_move_axis_state(limits, stop, checks);
    return stop;
}

// SlewToCoordinates and SlewToCoordinatesAsync (and the target forms, which pass the target): RA and Dec, then
// connected, parked and fault in the driver's order, then Tracking (ITelescopeV4: InvalidOperation while it is off,
// open-astro#1274).

inline void check_equatorial_slew_arguments(double ra_hours, double dec_degrees, std::string_view who) {
    validate_ra_dec(ra_hours, dec_degrees, who);
}

template <typename Checks>
void check_equatorial_slew_state(const TelescopeCommandLimits& limits, Checks&& checks) {
    checks.check_connected();
    telescope_gate_detail::parked_and_fault(limits.slew_order, /*stop=*/false, /*check_fault=*/true, checks);
    checks.check_tracking();
}

template <typename Checks>
void check_equatorial_slew(const TelescopeCommandLimits& limits, double ra_hours, double dec_degrees,
                           std::string_view who, Checks&& checks) {
    check_equatorial_slew_arguments(ra_hours, dec_degrees, who);
    check_equatorial_slew_state(limits, checks);
}

// A guide-rate setter: a finite value inside the axis's range, then connected and the link fault. A guide-rate write
// stores the rate for later pulses and moves nothing, so it has no parked or Tracking check.

inline void check_guide_rate_arguments(const TelescopeCommandLimits& limits, GuideRateAxis axis, double deg_per_sec) {
    if (!std::isfinite(deg_per_sec)) {
        telescope_gate_detail::refuse(limits.texts.guide_rate_not_finite);
    }
    const bool ra = axis == GuideRateAxis::RightAscension;
    const double min = ra ? limits.guide_rate_ra_min : limits.guide_rate_dec_min;
    const double max = ra ? limits.guide_rate_ra_max : limits.guide_rate_dec_max;
    if (deg_per_sec < min || deg_per_sec > max) {
        telescope_gate_detail::refuse(limits.texts.guide_rate_range);
    }
}

template <typename Checks>
void check_guide_rate_state(const TelescopeCommandLimits& /*limits*/, Checks&& checks) {
    checks.check_connected();
    checks.check_no_fault();
}

template <typename Checks>
void check_guide_rate(const TelescopeCommandLimits& limits, GuideRateAxis axis, double deg_per_sec, Checks&& checks) {
    check_guide_rate_arguments(limits, axis, deg_per_sec);
    check_guide_rate_state(limits, checks);
}

// TrackingRate set: a rate from 0 to the driver's highest, then connected and the link fault. A rate change moves no
// axis, so it has no parked or Tracking check (ITelescopeV4 names neither).

inline void check_tracking_rate_arguments(const TelescopeCommandLimits& limits, int rate) {
    if (rate < 0 || rate > limits.tracking_rate_max) {
        telescope_gate_detail::refuse(limits.texts.tracking_rate);
    }
}

template <typename Checks>
void check_tracking_rate_state(const TelescopeCommandLimits& /*limits*/, Checks&& checks) {
    checks.check_connected();
    checks.check_no_fault();
}

template <typename Checks>
void check_tracking_rate(const TelescopeCommandLimits& limits, int rate, Checks&& checks) {
    check_tracking_rate_arguments(limits, rate);
    check_tracking_rate_state(limits, checks);
}

// DestinationSideOfPier: RA and Dec, then connected and the link fault. It answers from the site and the clock and
// moves no axis, so it has no parked or Tracking check.

inline void check_destination_side_of_pier_arguments(double ra_hours, double dec_degrees) {
    validate_ra_dec(ra_hours, dec_degrees, "DestinationSideOfPier");
}

template <typename Checks>
void check_destination_side_of_pier_state(const TelescopeCommandLimits& /*limits*/, Checks&& checks) {
    checks.check_connected();
    checks.check_no_fault();
}

template <typename Checks>
void check_destination_side_of_pier(const TelescopeCommandLimits& limits, double ra_hours, double dec_degrees,
                                    Checks&& checks) {
    check_destination_side_of_pier_arguments(ra_hours, dec_degrees);
    check_destination_side_of_pier_state(limits, checks);
}

// The target setters (TargetRightAscension, TargetDeclination): the value, then connected and the link fault. A
// target write stores a coordinate for a later *ToTarget call and moves no axis, so it has no parked or Tracking
// check (a driver that refuses a parked write checks that itself, after the gate).

inline void check_target_right_ascension_arguments(double ra_hours) { validate_ra(ra_hours, "TargetRightAscension"); }

inline void check_target_declination_arguments(double dec_degrees) { validate_dec(dec_degrees, "TargetDeclination"); }

template <typename Checks>
void check_target_state(const TelescopeCommandLimits& /*limits*/, Checks&& checks) {
    checks.check_connected();
    checks.check_no_fault();
}

template <typename Checks>
void check_target_right_ascension(const TelescopeCommandLimits& limits, double ra_hours, Checks&& checks) {
    check_target_right_ascension_arguments(ra_hours);
    check_target_state(limits, checks);
}

template <typename Checks>
void check_target_declination(const TelescopeCommandLimits& limits, double dec_degrees, Checks&& checks) {
    check_target_declination_arguments(dec_degrees);
    check_target_state(limits, checks);
}

// The rate offsets (RightAscensionRate, DeclinationRate set): the value, then connected, the link fault and
// TrackingRate (ITelescopeV4: InvalidOperation unless it is driveSidereal). An offset changes the tracking rate of an
// axis already tracking and starts no move, so it has no parked or Tracking check.

inline void check_rate_offset_arguments(const TelescopeCommandLimits& limits, RateOffsetAxis axis, double rate) {
    const bool ra = axis == RateOffsetAxis::RightAscension;
    const double scaled = rate * (ra ? limits.rate_offset_ra_scale : limits.rate_offset_dec_scale);
    if (!std::isfinite(scaled)) {
        telescope_gate_detail::refuse(ra ? limits.texts.right_ascension_rate_not_finite
                                         : limits.texts.declination_rate_not_finite);
    }
    if (std::abs(scaled) > limits.rate_offset_max) {
        telescope_gate_detail::refuse(ra ? limits.texts.right_ascension_rate_range
                                         : limits.texts.declination_rate_range);
    }
}

template <typename Checks>
void check_rate_offset_state(const TelescopeCommandLimits& /*limits*/, Checks&& checks) {
    checks.check_connected();
    checks.check_no_fault();
    checks.check_sidereal_rate();
}

template <typename Checks>
void check_rate_offset(const TelescopeCommandLimits& limits, RateOffsetAxis axis, double rate, Checks&& checks) {
    check_rate_offset_arguments(limits, axis, rate);
    check_rate_offset_state(limits, checks);
}

// The Alt/Az slews (SlewToAltAz, SlewToAltAzAsync) and SyncToAltAz: altitude and azimuth, then connected, parked and
// fault in the driver's order, and the site the conversion to RA/Dec needs (before parked, or after the fault, per
// driver). A slew then refuses while Tracking is true (ITelescopeV4, #1377: a tracking mount drifts off the Alt/Az).
// A sync does not: ITelescopeV4 only says it "may", and a plate-solve sync runs on a tracking mount.

inline void check_alt_az_arguments(const TelescopeCommandLimits& limits, double altitude, double azimuth,
                                   std::string_view who) {
    const auto refuse = [&](const char* text) {
        throw AlpacaException(limits.texts.alt_az_texts_name_member ? std::string(who) + " " + text : std::string(text),
                              AlpacaError::InvalidValue);
    };
    if (!std::isfinite(altitude) || altitude < -90.0 || altitude > 90.0) {
        refuse(limits.texts.altitude_range);
    }
    if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth > 360.0 ||
        (azimuth == 360.0 && !limits.alt_az_azimuth_360)) {
        refuse(limits.texts.azimuth_range);
    }
}

template <typename Checks>
void check_alt_az_sync_state(const TelescopeCommandLimits& limits, Checks&& checks) {
    checks.check_connected();
    if (limits.alt_az_site_first) checks.check_site();
    telescope_gate_detail::parked_and_fault(limits.alt_az_order, /*stop=*/false, /*check_fault=*/true, checks);
    if (!limits.alt_az_site_first) checks.check_site();
}

template <typename Checks>
void check_alt_az_slew_state(const TelescopeCommandLimits& limits, Checks&& checks) {
    check_alt_az_sync_state(limits, checks);
    checks.check_not_tracking();
}

template <typename Checks>
void check_alt_az_slew(const TelescopeCommandLimits& limits, double altitude, double azimuth, std::string_view who,
                       Checks&& checks) {
    check_alt_az_arguments(limits, altitude, azimuth, who);
    check_alt_az_slew_state(limits, checks);
}

template <typename Checks>
void check_alt_az_sync(const TelescopeCommandLimits& limits, double altitude, double azimuth, std::string_view who,
                       Checks&& checks) {
    check_alt_az_arguments(limits, altitude, azimuth, who);
    check_alt_az_sync_state(limits, checks);
}

}  // namespace alpacacore::util

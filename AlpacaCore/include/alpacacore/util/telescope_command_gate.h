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
#include <string_view>

// The telescope command gate (#2258): the checks a mount runs before a command, in one order for every mount
// driver. A member refuses in this order: argument (InvalidValue), connected, parked, link fault, tracking (#2257).
//
// Families: PulseGuide, MoveAxis, the equatorial slews and the guide-rate setters (the members #2257 probes). Sync,
// the Alt/Az slews, the rate offsets, the targets, TrackingRate and DestinationSideOfPier join after #2287.
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
//
// The gate itself does no I/O, takes no lock and keeps no state (decision 0016 rules 1 and 2): a driver calls it
// where it ran its checks before, holding the lock its check callbacks expect. That is its state lock when the
// callbacks read state under a lock the caller holds, and no lock when each callback takes the state lock itself.
// Only the parked-against-fault order and the protocol limits differ per driver; both are data in
// TelescopeCommandLimits, never a vendor name in here.

namespace alpacacore::util {

/// The axis of a guide-rate write.
enum class GuideRateAxis : std::uint8_t { RightAscension, Declination };

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
};

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

}  // namespace alpacacore::util

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

#include <alpacacore/util/pointing.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

// The telescope command gate (#2258): the checks a mount runs before a command, in one order for every mount
// driver. A member refuses in this order: argument (InvalidValue), connected, parked, link fault, tracking (#2257).
// Only the parked-against-fault order and the protocol limits differ per driver; both are data in
// TelescopeCommandLimits, never a vendor name in here.
//
// The gate decides which check runs and when; the driver says how each one is answered. A driver passes a checks
// object of its own (no base class, no virtual member: any type with the members below), and the gate calls them in
// order. Each check throws the driver's own AlpacaException, in its own words, or returns:
//
//   [[noreturn]] void refuse_argument(TelescopeArgument which, double value);  // InvalidValue
//   void check_connected();                                                   // NotConnected
//   void check_not_parked(bool stop);                                         // InvalidWhileParked
//   void check_no_fault();                                                    // DriverException (decision 0009 rule 1)
//   void check_tracking();                                                    // InvalidOperation (equatorial slews)
//
// The gate itself does no I/O, takes no lock and keeps no state (decision 0016 rules 1 and 2): a driver calls it
// under its own state lock, as it ran its checks before. A check that does more than answer (OnStep's parked check
// reads :GU# when no status has been read since the connect; a fault check that closes a link stale past
// util::kLinkStalenessBound) keeps that work in the driver's check, so it still runs at the same point of the order.
//
// The argument half of each family has no state, so a driver may run it before it takes any lock and the state half
// after; the combined form runs both. The RA and Dec of a slew go through util::validate_ra_dec(), so their text is
// the same on every mount.

namespace alpacacore::util {

/// The argument a gate refused, so the driver throws it in its own words.
enum class TelescopeArgument : std::uint8_t {
    PulseDirection,         ///< PulseGuide direction outside 0-3
    PulseDuration,          ///< PulseGuide duration below 0 or above TelescopeCommandLimits::pulse_guide_max_ms
    MoveAxisAxis,           ///< a MoveAxis axis outside 0 .. move_axis_axes - 1
    MoveAxisRateNotFinite,  ///< a NaN or infinite MoveAxis rate
    MoveAxisRateRange,      ///< |rate| above move_axis_max_rate
};

/// Which of the parked and link-fault checks a member family runs first. #2257 leaves this order as each driver has
/// it, so it is per driver and member family.
enum class ParkedFaultOrder : std::uint8_t { ParkedFirst, FaultFirst };

/// A driver's protocol limits and orders. Every field defaults to "no limit"; the PulseGuide direction range (0-3)
/// and the lower bounds (duration 0, axis 0) are universal and not fields.
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
};

namespace telescope_gate_detail {

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

template <typename Checks>
void check_pulse_guide_arguments(const TelescopeCommandLimits& limits, int direction, int duration_ms,
                                 Checks&& checks) {
    if (direction < 0 || direction > 3) {
        checks.refuse_argument(TelescopeArgument::PulseDirection, direction);
    }
    if (duration_ms < 0 || duration_ms > limits.pulse_guide_max_ms) {
        checks.refuse_argument(TelescopeArgument::PulseDuration, duration_ms);
    }
}

template <typename Checks>
void check_pulse_guide_state(const TelescopeCommandLimits& limits, Checks&& checks) {
    checks.check_connected();
    telescope_gate_detail::parked_and_fault(limits.pulse_guide_order, /*stop=*/false, /*check_fault=*/true, checks);
}

template <typename Checks>
void check_pulse_guide(const TelescopeCommandLimits& limits, int direction, int duration_ms, Checks&& checks) {
    check_pulse_guide_arguments(limits, direction, duration_ms, checks);
    check_pulse_guide_state(limits, checks);
}

// MoveAxis: axis, a finite rate, its magnitude, then connected, parked and fault in the driver's order. A stop (a
// rate at or under the driver's epsilon) passes `stop` to the parked check and skips the fault check unless the
// driver's limits say a stop checks it.

/// True when `rate` is a stop for this driver. Call after the arguments passed (a NaN rate is refused there).
inline bool is_move_axis_stop(const TelescopeCommandLimits& limits, double rate) {
    return std::abs(rate) <= limits.move_axis_stop_epsilon;
}

template <typename Checks>
void check_move_axis_arguments(const TelescopeCommandLimits& limits, int axis, double rate, Checks&& checks) {
    if (axis < 0 || axis >= limits.move_axis_axes) {
        checks.refuse_argument(TelescopeArgument::MoveAxisAxis, axis);
    }
    if (!std::isfinite(rate)) {
        checks.refuse_argument(TelescopeArgument::MoveAxisRateNotFinite, rate);
    }
    if (std::abs(rate) > limits.move_axis_max_rate) {
        checks.refuse_argument(TelescopeArgument::MoveAxisRateRange, rate);
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
    check_move_axis_arguments(limits, axis, rate, checks);
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

}  // namespace alpacacore::util

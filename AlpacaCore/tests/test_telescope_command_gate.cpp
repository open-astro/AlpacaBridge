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

// util/telescope_command_gate.h (#2258): a table of (member, state, argument) to the refusal a caller sees. The
// checks object below stands in for a driver: each check records that it ran and throws the code a driver throws
// when its state says so. No driver, no fake mount, no clock.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/telescope_command_gate.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

using alpacacore::AlpacaException;
using alpacacore::test::error_code_of;
using alpacacore::util::ParkedFaultOrder;
using alpacacore::util::TelescopeCommandLimits;
namespace AlpacaError = alpacacore::AlpacaError;

namespace {

struct State {
    bool connected = true;
    bool parked = false;
    bool faulted = false;
    bool tracking = true;
};

// A driver's state checks, recorded.
struct RecordingChecks {
    State state;
    std::vector<std::string> calls;

    void check_connected() {
        calls.emplace_back("connected");
        if (!state.connected) throw AlpacaException("not connected", AlpacaError::NotConnected);
    }
    void check_not_parked(bool stop) {
        calls.emplace_back(stop ? "parked(stop)" : "parked");
        if (state.parked) throw AlpacaException("parked", AlpacaError::InvalidWhileParked);
    }
    void check_no_fault() {
        calls.emplace_back("fault");
        if (state.faulted) throw AlpacaException("faulted", AlpacaError::DriverException);
    }
    void check_tracking() {
        calls.emplace_back("tracking");
        if (!state.tracking) throw AlpacaException("tracking off", AlpacaError::InvalidOperation);
    }
};

// The message `call` throws, or "" when it passes.
template <typename F>
std::string text_of(F&& call) {
    try {
        call();
    } catch (const AlpacaException& e) {
        return e.what();
    }
    return "";
}

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

State disconnected() { return State{false, false, false, true}; }
State parked() { return State{true, true, false, true}; }
State faulted() { return State{true, false, true, true}; }
State parked_and_faulted() { return State{true, true, true, true}; }
State tracking_off() { return State{true, false, false, false}; }
State everything_wrong() { return State{false, true, true, false}; }

TelescopeCommandLimits limits_with(int pulse_max_ms, double rate_max, int axes) {
    TelescopeCommandLimits limits;
    limits.pulse_guide_max_ms = pulse_max_ms;
    limits.move_axis_max_rate = rate_max;
    limits.move_axis_axes = axes;
    return limits;
}

}  // namespace

TEST_CASE("Telescope command gate - PulseGuide refuses in the order argument, connected, parked, fault",
          "[util][command-gate]") {
    TelescopeCommandLimits limits = limits_with(9999, kInf, 2);
    limits.texts.pulse_direction = "direction";
    limits.texts.pulse_duration = "duration";
    // The driver's own words, from its limits; the direction is checked first.
    CHECK(text_of([&] { alpacacore::util::check_pulse_guide_arguments(limits, 4, -1); }) == "direction");
    CHECK(text_of([&] { alpacacore::util::check_pulse_guide_arguments(limits, 3, 10000); }) == "duration");
    struct Row {
        State state;
        int direction;
        int duration;
        int expected;
    };
    const std::vector<Row> rows = {
        {State{}, 0, 0, 0},
        {State{}, 3, 9999, 0},
        {State{}, -1, 100, AlpacaError::InvalidValue},
        {State{}, 4, 100, AlpacaError::InvalidValue},
        {State{}, 0, -1, AlpacaError::InvalidValue},
        {State{}, 0, 10000, AlpacaError::InvalidValue},
        {everything_wrong(), 4, 100, AlpacaError::InvalidValue},
        {everything_wrong(), 0, 100, AlpacaError::NotConnected},
        {disconnected(), 0, 100, AlpacaError::NotConnected},
        {parked(), 0, 100, AlpacaError::InvalidWhileParked},
        {faulted(), 0, 100, AlpacaError::DriverException},
        {parked_and_faulted(), 0, 100, AlpacaError::InvalidWhileParked},
        {tracking_off(), 0, 100, 0},  // PulseGuide does not need Tracking
    };
    for (const Row& row : rows) {
        RecordingChecks checks{row.state, {}};
        INFO("direction " << row.direction << " duration " << row.duration);
        CHECK(error_code_of([&] {
                  alpacacore::util::check_pulse_guide(limits, row.direction, row.duration, checks);
              }) == row.expected);
    }
}

TEST_CASE("Telescope command gate - the parked-against-fault order is the driver's, per member family",
          "[util][command-gate]") {
    TelescopeCommandLimits limits = limits_with(9999, 4.0, 2);
    limits.pulse_guide_order = ParkedFaultOrder::FaultFirst;
    limits.move_axis_order = ParkedFaultOrder::FaultFirst;
    limits.slew_order = ParkedFaultOrder::FaultFirst;
    RecordingChecks pulse{parked_and_faulted(), {}};
    CHECK(error_code_of([&] { alpacacore::util::check_pulse_guide(limits, 0, 100, pulse); }) ==
          AlpacaError::DriverException);
    RecordingChecks move{parked_and_faulted(), {}};
    CHECK(error_code_of([&] { alpacacore::util::check_move_axis(limits, 0, 1.0, move); }) ==
          AlpacaError::DriverException);
    RecordingChecks slew{parked_and_faulted(), {}};
    CHECK(error_code_of([&] { alpacacore::util::check_equatorial_slew(limits, 5.0, 20.0, "Slew", slew); }) ==
          AlpacaError::DriverException);

    // One family's order does not leak into another's.
    limits.slew_order = ParkedFaultOrder::ParkedFirst;
    RecordingChecks slew_parked_first{parked_and_faulted(), {}};
    CHECK(error_code_of([&] {
              alpacacore::util::check_equatorial_slew(limits, 5.0, 20.0, "Slew", slew_parked_first);
          }) == AlpacaError::InvalidWhileParked);
}

TEST_CASE("Telescope command gate - MoveAxis refuses in the order axis, rate, connected, parked, fault",
          "[util][command-gate]") {
    TelescopeCommandLimits limits = limits_with(9999, 4.0, 2);
    limits.move_axis_stop_epsilon = 1e-6;
    limits.texts.move_axis_axis = "axis";
    limits.texts.move_axis_rate_not_finite = "not finite";
    limits.texts.move_axis_rate_range = "too fast";
    struct Row {
        State state;
        int axis;
        double rate;
        int expected;
        const char* text;  // the driver's text, when expected is InvalidValue
    };
    const std::vector<Row> rows = {
        {State{}, 0, 1.0, 0, {}},
        {State{}, 1, -4.0, 0, {}},
        {State{}, -1, 1.0, AlpacaError::InvalidValue, "axis"},
        {State{}, 2, 1.0, AlpacaError::InvalidValue, "axis"},
        {State{}, 0, kNaN, AlpacaError::InvalidValue, "not finite"},
        {State{}, 0, kInf, AlpacaError::InvalidValue, "not finite"},
        {State{}, 0, 4.0001, AlpacaError::InvalidValue, "too fast"},
        {State{}, 0, -4.0001, AlpacaError::InvalidValue, "too fast"},
        {everything_wrong(), 2, kNaN, AlpacaError::InvalidValue, "axis"},
        {everything_wrong(), 0, 1.0, AlpacaError::NotConnected, {}},
        {parked(), 0, 1.0, AlpacaError::InvalidWhileParked, {}},
        {faulted(), 0, 1.0, AlpacaError::DriverException, {}},
        {parked_and_faulted(), 0, 1.0, AlpacaError::InvalidWhileParked, {}},
        {tracking_off(), 0, 1.0, 0, {}},  // MoveAxis does not need Tracking
    };
    for (const Row& row : rows) {
        RecordingChecks checks{row.state, {}};
        INFO("axis " << row.axis << " rate " << row.rate);
        CHECK(error_code_of([&] { (void)alpacacore::util::check_move_axis(limits, row.axis, row.rate, checks); }) ==
              row.expected);
        if (row.expected == AlpacaError::InvalidValue) {
            CHECK(text_of([&] { alpacacore::util::check_move_axis_arguments(limits, row.axis, row.rate); }) ==
                  row.text);
            CHECK(checks.calls.empty());  // nothing past the arguments ran
        }
    }
}

TEST_CASE("Telescope command gate - a MoveAxis stop tells the parked check and skips the fault check if allowed",
          "[util][command-gate]") {
    TelescopeCommandLimits limits = limits_with(9999, 4.0, 2);
    limits.move_axis_stop_epsilon = 1e-6;

    SECTION("a stop on a faulted link passes when the driver tries stops there") {
        limits.stop_checks_fault = false;
        RecordingChecks checks{faulted(), {}};
        bool stop = false;
        CHECK(error_code_of([&] { stop = alpacacore::util::check_move_axis(limits, 0, 1e-7, checks); }) == 0);
        CHECK(stop);
        CHECK(checks.calls == std::vector<std::string>{"connected", "parked(stop)"});
    }
    SECTION("a stop on a faulted link is refused when the driver checks the fault for stops") {
        limits.stop_checks_fault = true;
        RecordingChecks checks{faulted(), {}};
        CHECK(error_code_of([&] { (void)alpacacore::util::check_move_axis(limits, 0, 0.0, checks); }) ==
              AlpacaError::DriverException);
        CHECK(checks.calls == std::vector<std::string>{"connected", "parked(stop)", "fault"});
    }
    SECTION("a rate just over the epsilon is a move, not a stop") {
        limits.stop_checks_fault = false;
        RecordingChecks checks{faulted(), {}};
        CHECK(error_code_of([&] { (void)alpacacore::util::check_move_axis(limits, 0, 2e-6, checks); }) ==
              AlpacaError::DriverException);
        CHECK(checks.calls == std::vector<std::string>{"connected", "parked", "fault"});
    }
    SECTION("a stop on a parked mount is still refused when the driver's parked check says so") {
        RecordingChecks checks{parked(), {}};
        CHECK(error_code_of([&] { (void)alpacacore::util::check_move_axis(limits, 1, -0.0, checks); }) ==
              AlpacaError::InvalidWhileParked);
    }
    CHECK(alpacacore::util::is_move_axis_stop(limits, 1e-6));
    CHECK_FALSE(alpacacore::util::is_move_axis_stop(limits, -1.1e-6));
}

TEST_CASE("Telescope command gate - a slew refuses in the order coordinates, connected, parked, fault, tracking",
          "[util][command-gate]") {
    const TelescopeCommandLimits limits;
    struct Row {
        State state;
        double ra;
        double dec;
        int expected;
    };
    const std::vector<Row> rows = {
        {State{}, 0.0, -90.0, 0},
        {State{}, 23.999, 90.0, 0},
        {State{}, 24.0, 0.0, AlpacaError::InvalidValue},
        {State{}, -0.1, 0.0, AlpacaError::InvalidValue},
        {State{}, 5.0, 90.1, AlpacaError::InvalidValue},
        {State{}, kNaN, 0.0, AlpacaError::InvalidValue},
        {State{}, 5.0, kInf, AlpacaError::InvalidValue},
        {everything_wrong(), 24.0, 0.0, AlpacaError::InvalidValue},
        {everything_wrong(), 5.0, 20.0, AlpacaError::NotConnected},
        {parked(), 5.0, 20.0, AlpacaError::InvalidWhileParked},
        {faulted(), 5.0, 20.0, AlpacaError::DriverException},
        {parked_and_faulted(), 5.0, 20.0, AlpacaError::InvalidWhileParked},
        {tracking_off(), 5.0, 20.0, AlpacaError::InvalidOperation},
        {State{true, true, true, false}, 5.0, 20.0, AlpacaError::InvalidWhileParked},
        {State{true, false, true, false}, 5.0, 20.0, AlpacaError::DriverException},
    };
    for (const Row& row : rows) {
        RecordingChecks checks{row.state, {}};
        INFO("ra " << row.ra << " dec " << row.dec);
        CHECK(error_code_of([&] {
                  alpacacore::util::check_equatorial_slew(limits, row.ra, row.dec, "SlewToCoordinates", checks);
              }) == row.expected);
    }
}

TEST_CASE("Telescope command gate - a passing call runs each check once, in order", "[util][command-gate]") {
    TelescopeCommandLimits limits = limits_with(9999, 4.0, 2);
    limits.pulse_guide_order = ParkedFaultOrder::FaultFirst;
    RecordingChecks pulse;
    alpacacore::util::check_pulse_guide(limits, 2, 500, pulse);
    CHECK(pulse.calls == std::vector<std::string>{"connected", "fault", "parked"});

    RecordingChecks move;
    CHECK_FALSE(alpacacore::util::check_move_axis(limits, 1, 0.5, move));
    CHECK(move.calls == std::vector<std::string>{"connected", "parked", "fault"});

    RecordingChecks slew;
    alpacacore::util::check_equatorial_slew(limits, 5.0, 20.0, "SlewToCoordinatesAsync", slew);
    CHECK(slew.calls == std::vector<std::string>{"connected", "parked", "fault", "tracking"});

    // The argument halves take the limits only and pass a valid call.
    CHECK_NOTHROW(alpacacore::util::check_pulse_guide_arguments(limits, 0, 0));
    CHECK_NOTHROW(alpacacore::util::check_move_axis_arguments(limits, 0, 4.0));
}

TEST_CASE("Telescope command gate - the default limits refuse nothing a protocol limit would", "[util][command-gate]") {
    const TelescopeCommandLimits limits;
    RecordingChecks checks;
    CHECK(error_code_of(
              [&] { alpacacore::util::check_pulse_guide(limits, 0, std::numeric_limits<int>::max(), checks); }) == 0);
    CHECK(error_code_of([&] { (void)alpacacore::util::check_move_axis(limits, 2, 1e300, checks); }) == 0);
    CHECK(error_code_of([&] { (void)alpacacore::util::check_move_axis(limits, 3, 1.0, checks); }) ==
          AlpacaError::InvalidValue);  // ITelescopeV4 names three axes
    // The universal bounds stay: a negative duration and a direction outside 0-3.
    CHECK(error_code_of([&] { alpacacore::util::check_pulse_guide(limits, 0, -1, checks); }) ==
          AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { alpacacore::util::check_pulse_guide(limits, 4, 0, checks); }) ==
          AlpacaError::InvalidValue);
    // The default epsilon makes only a zero rate a stop.
    CHECK(alpacacore::util::is_move_axis_stop(limits, 0.0));
    CHECK_FALSE(alpacacore::util::is_move_axis_stop(limits, 1e-12));
}

TEST_CASE("Telescope command gate - a guide-rate write refuses in the order value, connected, fault",
          "[util][command-gate]") {
    TelescopeCommandLimits limits;
    limits.guide_rate_ra_min = 0.0;
    limits.guide_rate_ra_max = 0.004;
    limits.guide_rate_dec_min = 0.001;
    limits.guide_rate_dec_max = 0.003;
    limits.texts.guide_rate_not_finite = "not finite";
    limits.texts.guide_rate_range = "out of range";
    using alpacacore::util::GuideRateAxis;
    struct Row {
        State state;
        GuideRateAxis axis;
        double rate;
        int expected;
        const char* text;  // the driver's text, when expected is InvalidValue
    };
    const std::vector<Row> rows = {
        {State{}, GuideRateAxis::RightAscension, 0.0, 0, {}},
        {State{}, GuideRateAxis::RightAscension, 0.004, 0, {}},
        {State{}, GuideRateAxis::Declination, 0.001, 0, {}},
        {State{}, GuideRateAxis::RightAscension, -0.0001, AlpacaError::InvalidValue, "out of range"},
        {State{}, GuideRateAxis::RightAscension, 0.0041, AlpacaError::InvalidValue, "out of range"},
        {State{}, GuideRateAxis::Declination, 0.0005, AlpacaError::InvalidValue, "out of range"},
        {State{}, GuideRateAxis::Declination, 0.0035, AlpacaError::InvalidValue, "out of range"},
        {State{}, GuideRateAxis::Declination, kNaN, AlpacaError::InvalidValue, "not finite"},
        {everything_wrong(), GuideRateAxis::RightAscension, kInf, AlpacaError::InvalidValue, "not finite"},
        {everything_wrong(), GuideRateAxis::RightAscension, 0.002, AlpacaError::NotConnected, {}},
        {faulted(), GuideRateAxis::Declination, 0.002, AlpacaError::DriverException, {}},
        {parked(), GuideRateAxis::RightAscension, 0.002, 0, {}},  // a stored rate moves nothing
        {tracking_off(), GuideRateAxis::Declination, 0.002, 0, {}},
    };
    for (const Row& row : rows) {
        RecordingChecks checks{row.state, {}};
        INFO("rate " << row.rate);
        CHECK(error_code_of([&] { alpacacore::util::check_guide_rate(limits, row.axis, row.rate, checks); }) ==
              row.expected);
        if (row.expected == AlpacaError::InvalidValue) {
            CHECK(text_of([&] { alpacacore::util::check_guide_rate_arguments(limits, row.axis, row.rate); }) ==
                  row.text);
            CHECK(checks.calls.empty());
        }
    }
    RecordingChecks passing;
    alpacacore::util::check_guide_rate(limits, GuideRateAxis::Declination, 0.002, passing);
    CHECK(passing.calls == std::vector<std::string>{"connected", "fault"});
}

TEST_CASE("Telescope command gate - a TrackingRate write refuses in the order rate, connected, fault",
          "[util][command-gate]") {
    TelescopeCommandLimits limits;
    limits.tracking_rate_max = 2;
    limits.texts.tracking_rate = "rate";
    struct Row {
        State state;
        int rate;
        int expected;
    };
    const std::vector<Row> rows = {
        {State{}, 0, 0},
        {State{}, 2, 0},
        {State{}, -1, AlpacaError::InvalidValue},
        {State{}, 3, AlpacaError::InvalidValue},
        {everything_wrong(), 3, AlpacaError::InvalidValue},
        {everything_wrong(), 1, AlpacaError::NotConnected},
        {faulted(), 1, AlpacaError::DriverException},
        {parked(), 1, 0},  // a rate change moves no axis
        {tracking_off(), 1, 0},
    };
    for (const Row& row : rows) {
        RecordingChecks checks{row.state, {}};
        INFO("rate " << row.rate);
        CHECK(error_code_of([&] { alpacacore::util::check_tracking_rate(limits, row.rate, checks); }) == row.expected);
        if (row.expected == AlpacaError::InvalidValue) {
            CHECK(text_of([&] { alpacacore::util::check_tracking_rate_arguments(limits, row.rate); }) == "rate");
            CHECK(checks.calls.empty());
        }
    }
    RecordingChecks passing;
    alpacacore::util::check_tracking_rate(limits, 1, passing);
    CHECK(passing.calls == std::vector<std::string>{"connected", "fault"});

    // The default refuses only a negative rate.
    const TelescopeCommandLimits defaults;
    CHECK_NOTHROW(alpacacore::util::check_tracking_rate_arguments(defaults, std::numeric_limits<int>::max()));
    CHECK(error_code_of([&] { alpacacore::util::check_tracking_rate_arguments(defaults, -1); }) ==
          AlpacaError::InvalidValue);
}

TEST_CASE("Telescope command gate - DestinationSideOfPier refuses in the order RA, Dec, connected, fault",
          "[util][command-gate]") {
    const TelescopeCommandLimits limits;
    struct Row {
        State state;
        double ra;
        double dec;
        int expected;
    };
    const std::vector<Row> rows = {
        {State{}, 0.0, -90.0, 0},
        {State{}, 23.99, 90.0, 0},
        {State{}, 24.0, 0.0, AlpacaError::InvalidValue},
        {State{}, -0.01, 0.0, AlpacaError::InvalidValue},
        {State{}, kNaN, 0.0, AlpacaError::InvalidValue},
        {State{}, 0.0, 90.01, AlpacaError::InvalidValue},
        {State{}, 0.0, kInf, AlpacaError::InvalidValue},
        {everything_wrong(), 25.0, 91.0, AlpacaError::InvalidValue},
        {everything_wrong(), 12.0, 45.0, AlpacaError::NotConnected},
        {faulted(), 12.0, 45.0, AlpacaError::DriverException},
        {parked(), 12.0, 45.0, 0},  // it moves no axis
        {tracking_off(), 12.0, 45.0, 0},
    };
    for (const Row& row : rows) {
        RecordingChecks checks{row.state, {}};
        INFO("ra " << row.ra << " dec " << row.dec);
        CHECK(error_code_of([&] {
                  alpacacore::util::check_destination_side_of_pier(limits, row.ra, row.dec, checks);
              }) == row.expected);
        if (row.expected == AlpacaError::InvalidValue) {
            CHECK(checks.calls.empty());
        }
    }
    // RA first, in util::validate_ra_dec()'s words with the member's name.
    CHECK(text_of([&] { alpacacore::util::check_destination_side_of_pier_arguments(25.0, 91.0); }) ==
          "DestinationSideOfPier: RightAscension must be in range 0 to <24 hours");
    CHECK(text_of([&] { alpacacore::util::check_destination_side_of_pier_arguments(0.0, 91.0); }) ==
          "DestinationSideOfPier: Declination must be in range -90 to 90 degrees");
    RecordingChecks passing;
    alpacacore::util::check_destination_side_of_pier(limits, 12.0, 45.0, passing);
    CHECK(passing.calls == std::vector<std::string>{"connected", "fault"});
}

TEST_CASE("Telescope command gate - a family with no fault check runs the parked check only", "[util][command-gate]") {
    TelescopeCommandLimits limits;
    limits.pulse_guide_order = ParkedFaultOrder::NoFaultCheck;
    limits.move_axis_order = ParkedFaultOrder::NoFaultCheck;
    limits.slew_order = ParkedFaultOrder::NoFaultCheck;
    RecordingChecks pulse{faulted(), {}};
    alpacacore::util::check_pulse_guide(limits, 1, 100, pulse);
    CHECK(pulse.calls == std::vector<std::string>{"connected", "parked"});
    RecordingChecks move{faulted(), {}};
    CHECK_FALSE(alpacacore::util::check_move_axis(limits, 0, 1.0, move));
    CHECK(move.calls == std::vector<std::string>{"connected", "parked"});
    RecordingChecks slew{faulted(), {}};
    alpacacore::util::check_equatorial_slew(limits, 5.0, 20.0, "SlewToCoordinates", slew);
    CHECK(slew.calls == std::vector<std::string>{"connected", "parked", "tracking"});
    RecordingChecks parked_slew{parked_and_faulted(), {}};
    CHECK(error_code_of([&] { alpacacore::util::check_equatorial_slew(limits, 5.0, 20.0, "Slew", parked_slew); }) ==
          AlpacaError::InvalidWhileParked);
}

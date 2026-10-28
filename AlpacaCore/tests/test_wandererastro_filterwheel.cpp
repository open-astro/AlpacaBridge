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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <functional>

#include "catch2_compat.h"
#include "driver_rig.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("WandererAstro FilterWheel Driver - Defaults", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // No model known before connect — generic vendor name.
    CHECK(driver->get_name() == "WandererAstro Filter Wheel");
    // The whole lineup is 8-slot, so names/offsets are sized at construction.
    CHECK(driver->get_names().size() == 8);
    CHECK(driver->get_focus_offsets().size() == 8);
    CHECK(driver->get_names()[0] == "Filter 1");
    CHECK(driver->get_names()[7] == "Filter 8");
    CHECK(driver->get_focus_offsets()[0] == 0);
}

TEST_CASE("WandererAstro FilterWheel Driver - Device metadata", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "WandererAstro SFW Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore WandererAstro FilterWheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "WANDERERASTRO_FILTERWHEEL_3");
    // No firmware known before the first streamed status frame.
    CHECK(!driver->get_device_firmware().has_value());
}

TEST_CASE("WandererAstro FilterWheel Driver - Not connected throws", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(1, "/dev/null");

    REQUIRE(driver->get_connected() == false);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(3); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro FilterWheel Driver - Unsupported actions", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("WandererAstro FilterWheel Driver - Device Number Assignment", "[wandererastro][filterwheel][unit]") {
    auto driver0 = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(0, 0);
    auto driver1 = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(1, 0);
    auto driver5 = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}

TEST_CASE("WandererAstro FilterWheel Driver - Value range validation", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(0, 0);

    // The slot count is fixed for the whole lineup, so the full Position range
    // is validated BEFORE the connection check — an out-of-range position is
    // unconditionally invalid and testable without hardware.
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(8); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(100); }, alpacacore::AlpacaError::InvalidValue);

    // Names / FocusOffsets must be exactly 8 entries.
    require_alpaca_error([&]() { driver->set_names({"L", "R", "G", "B"}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_focus_offsets({1, 2, 3}); }, alpacacore::AlpacaError::InvalidValue);

    // A failed set_names must leave the existing names untouched (staged copy).
    CHECK(driver->get_names()[0] == "Filter 1");
}

TEST_CASE("WandererAstro FilterWheel Driver - Names and offsets semantics", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(0, 0);

    // Full 8-name set round-trips.
    driver->set_names({"L", "R", "G", "B", "Ha", "OIII", "SII", "Clear"});
    CHECK(driver->get_names()[4] == "Ha");
    CHECK(driver->get_names()[7] == "Clear");

    // Empty entries fall back to positional defaults.
    driver->set_names({"L", "", "G", "", "Ha", "", "SII", ""});
    CHECK(driver->get_names()[1] == "Filter 2");
    CHECK(driver->get_names()[3] == "Filter 4");

    // Single-token shorthand expands to per-slot single-character names when
    // its length matches the slot count (matches the ZWO/PlayerOne drivers).
    driver->set_names({"LRGBSHOC"});
    CHECK(driver->get_names().size() == 8);
    CHECK(driver->get_names()[0] == "L");
    CHECK(driver->get_names()[7] == "C");

    // Focus offsets round-trip.
    driver->set_focus_offsets({10, 20, 30, 40, 50, 60, 70, 80});
    CHECK(driver->get_focus_offsets()[2] == 30);
    CHECK(driver->get_focus_offsets()[7] == 80);
}

TEST_CASE("WandererAstro FilterWheel Driver - State machine", "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: while disconnected the Position getter throws and
    // is omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; there must be no non-compliant
    // "Connected" entry.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "Position");  // omitted, not fabricated, when unknown
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("WandererAstro FilterWheel Protocol Wrapper - Disconnected behavior", "[wandererastro][filterwheel][unit]") {
    alpacacore::vendor::wandererastro::WandererFilterWheelProtocolWrapper wrapper;

    REQUIRE(wrapper.is_connected() == false);
    CHECK(!wrapper.get_firmware_date().has_value());

    const auto status = wrapper.get_status();
    CHECK(status.valid == false);
    CHECK(status.position == 0);

    // Slot range is validated before the connection check ([1, 8], 1-based on
    // the wire), so the boundary is testable without hardware.
    require_alpaca_error([&]() { wrapper.select_filter(0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.select_filter(9); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.select_filter(1); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.calibrate(); }, alpacacore::AlpacaError::NotConnected);

    // Connecting to a missing port must fail with NotConnected and leave the
    // wrapper disconnected.
    alpacacore::vendor::wandererastro::FilterWheelConnectionConfig config;
    config.serial_port = "/nonexistent/wanderer-filterwheel-test-port";
    config.serial_timeout_s = 1;
    require_alpaca_error([&]() { wrapper.connect(config); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(wrapper.is_connected() == false);
}

// ---------------------------------------------------------------------------
// Issue #237: a device that stops streaming must not be served from the cache
// forever. Wire-level over a pty-backed streamer (fake_serial_streamer.h).
// ---------------------------------------------------------------------------

#include <chrono>
#include <thread>

#include "fake_serial_streamer.h"
#include "fake_task_clock.h"

namespace {

// Moves the fake clock by @p d, then waits (bounded, real time) for the
// reader thread's next pass: its silence check is the one clock read a muted
// or severed link makes per pass, taken under the lock the driver's getters
// also take, so a getter called after this returns sees that pass's verdict.
bool advance_one_pass(alpacacore::test::FakeTaskClock& clock, std::chrono::nanoseconds d) {
    clock.advance(d);
    return clock.wait_for_now_calls(clock.now_calls() + 1, std::chrono::milliseconds(2000));
}

// set_muted() can land just after the streamer committed one more frame to the
// pty; give the reader time to take it at the current virtual time before the
// clock moves, or it would restart the silence window.
void drain_after_mute() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

template <typename Pred>
bool wait_until_sfw(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

}  // namespace

// <model>A<fw>A<position>A<letters>A<8 per-filter fields>A<deviceID>A
const char* const kSfwFrame = "WSFW368A20260124A3ABCDEFGHIA0A0A0A0A0A0A0A0A1A\n";

TEST_CASE("WandererAstro FilterWheel Driver - Silent link refuses Position and moves (issue #237)",
          "[wandererastro][filterwheel][unit][fake]") {
    alpacacore::test::FakeTaskClock clock;  // outlives the driver
    alpacacore::test::FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(300));
    auto driver =
        alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, wheel.slave_path(), 19200, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_position() == 2);  // wire slot 3 -> Alpaca 2

    wheel.set_muted(true);
    auto position_throws = [&] {
        try {
            (void)driver->get_position();
            return false;
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
    };
    drain_after_mute();
    // The 10 s limit is inclusive: exactly 10 s of silence still serves the cache.
    REQUIRE(advance_one_pass(clock, std::chrono::seconds(10)));
    CHECK(driver->get_position() == 2);
    // One millisecond more latches the fault.
    REQUIRE(advance_one_pass(clock, std::chrono::milliseconds(1)));
    CHECK(position_throws());
    CHECK(driver->get_connected());
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing
    require_alpaca_error([&]() { (void)driver->get_position(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->set_position(5); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(wheel.received("6"));  // no move went on the wire while faulted
    // Names and offsets are driver-side and keep answering.
    CHECK(driver->get_names().size() == 8);

    wheel.set_muted(false);
    CHECK(wait_until_sfw([&] { return !position_throws(); }, std::chrono::milliseconds(3000)));
    CHECK(driver->get_position() == 2);
    CHECK(driver->get_link_fault().empty());
    CHECK_NOTHROW(driver->set_connected(false));
}

// #508 item 1: a saved serial entry with an empty portPath is registered so it stays listed; its
// connect refuses in words the operator can act on instead of opening "" or auto-detecting.
TEST_CASE("WandererAstro FilterWheel Driver - Connect with empty serial port names portPath",
          "[wandererastro][filterwheel][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, "");
    std::string message;
    try {
        driver->set_connected(true);
    } catch (const alpacacore::AlpacaException& e) {
        message = e.what();
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    CHECK(message.find("portPath") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
}

// ---------------------------------------------------------------------------
// Issue #1202: the filter wheel opens its port through util::SerialLink, so a port
// another device holds is refused untouched, its own claim is released on
// disconnect, a retuned port cannot hang Disconnect, and a link-fault latch
// puts back line settings another program changed.
// ---------------------------------------------------------------------------

#include <alpacacore/util/serial_port_registry.h>

#include "wandererastro_serial_ownership_helpers.h"

TEST_CASE("WandererAstro FilterWheel Driver - Refuses a port another device holds (issue #1202)",
          "[wandererastro][filterwheel][serial][issue1202]") {
    namespace util = alpacacore::util;
    alpacacore::test::FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(300));
    const speed_t before = alpacacore::test::wanderer::line_speed(wheel.keepalive_fd());
    auto claim = util::SerialPortClaim::acquire(wheel.slave_path(), util::SerialClaimKind::Link, "Other device");
    REQUIRE(claim);

    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, wheel.slave_path());
    const std::string error = alpacacore::test::wanderer::connect_error([&] { driver->set_connected(true); });
    CHECK(error.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    // Refused before the open: the holder's line settings are untouched.
    CHECK(alpacacore::test::wanderer::line_speed(wheel.keepalive_fd()) == before);
}

TEST_CASE("WandererAstro FilterWheel Driver - Holds its port while connected and releases it (issue #1202)",
          "[wandererastro][filterwheel][serial][issue1202]") {
    alpacacore::test::FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(300));
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, wheel.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(alpacacore::util::is_serial_port_in_use(wheel.slave_path()));
    driver->set_connected(false);
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(wheel.slave_path()));
    // TIOCEXCL and the claim are both gone: the same port connects again.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE("WandererAstro FilterWheel Driver - Disconnect returns after the port is retuned to VMIN=1 (issue #1202)",
          "[wandererastro][filterwheel][serial][issue1202]") {
    alpacacore::test::FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(300));
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, wheel.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // A silent link under a blocking-read retune: a VTIME-paced reader would
    // park in read() and never see its stop flag.
    wheel.set_muted(true);
    drain_after_mute();
    alpacacore::test::wanderer::retune_to_blocking_read(wheel.keepalive_fd());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    CHECK(alpacacore::test::wanderer::returns_within([&] { driver->set_connected(false); }, std::chrono::seconds(3),
                                                     [&] { wheel.set_muted(false); }));
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("WandererAstro FilterWheel Driver - Link fault restores retuned line settings (issue #1202)",
          "[wandererastro][filterwheel][serial][issue1202]") {
    alpacacore::test::FakeTaskClock clock;  // outlives the driver
    alpacacore::test::FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(50));
    auto driver =
        alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, wheel.slave_path(), 19200, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(alpacacore::test::wanderer::line_speed(wheel.keepalive_fd()) == B19200);

    // Bytes keep arriving but no frame parses (the wrong baud's garbage), so no
    // read ever times out and tty_read()'s own restore never runs.
    wheel.set_frame("ZZZZZZ\r");
    drain_after_mute();
    alpacacore::test::wanderer::set_line_speed(wheel.keepalive_fd(), B9600);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    REQUIRE(alpacacore::test::wanderer::line_speed(wheel.keepalive_fd()) == B9600);

    REQUIRE(advance_one_pass(clock, std::chrono::milliseconds(10001)));
    CHECK(wait_until_sfw([&] { return !driver->get_link_fault().empty(); }, std::chrono::milliseconds(2000)));
    CHECK(wait_until_sfw([&] { return alpacacore::test::wanderer::line_speed(wheel.keepalive_fd()) == B19200; },
                         std::chrono::milliseconds(2000)));
    driver->set_connected(false);
}

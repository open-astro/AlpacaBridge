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
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_qhy_cfw3.h"

namespace {

using alpacacore::test::require_alpaca_error;

using alpacacore::test::FakeQhyCfw3;

// A pty never DTR-resets the fake, so the real 25 s boot wait would only be
// dead time here; a short wait still exercises the "no boot byte" path.
alpacacore::vendor::qhy::Cfw3Settings fast_settings() {
    alpacacore::vendor::qhy::Cfw3Settings s;
    s.boot_timeout_ms = 150;
    s.reply_timeout_ms = 300;
    s.move_timeout_ms = 3000;
    return s;
}

std::unique_ptr<alpacacore::FilterWheelDriver> make_driver(const FakeQhyCfw3& fake, int device_number = 0) {
    return alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(device_number, fake.slave_path(), fast_settings());
}

// Poll Position until it stops reading -1 (the move worker has published).
int settle_position(alpacacore::FilterWheelDriver& driver, std::chrono::milliseconds budget = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    int pos = driver.get_position();
    while (pos == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pos = driver.get_position();
    }
    return pos;
}

}  // namespace

TEST_CASE("QHY CFW3 Filter Wheel Driver - Defaults", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "/dev/null");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "QHY CFW3");
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Device metadata", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(3, "/dev/null");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY CFW3 filter wheel driver (USB)");
    CHECK(driver->get_driver_info() == "AlpacaCore QHY CFW3 Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "QHY_CFW3_3");
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Not connected throws", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "/dev/null");

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Unsupported actions", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "/dev/null");

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Names and focus offsets configurable while disconnected",
          "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(1, "/dev/null");

    // Slot count is unknown until connect, so no length validation yet.
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10}));
    REQUIRE_NOTHROW(driver->set_names({"L", "R"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "R"});
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Value range validation", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "/dev/null");

    // ASCOM precedence: range validation before the connection check. A
    // negative slot, or one past the protocol's 16-position ceiling ('F'), is
    // unconditionally invalid; an in-range-but-unverifiable slot needs the
    // hardware slot count and reports NotConnected here (connected coverage
    // below and in ConformU).
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(16); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(5); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - State machine contracts", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "/dev/null");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; no non-compliant "Connected" entry.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") has_timestamp = true;
    }
    REQUIRE_FALSE(has_timestamp);
    // The disconnected list is empty, so the loop above never iterates: the
    // "no Connected entry" claim is only live on a connected driver.
    CHECK(state.empty());

    FakeQhyCfw3 fake(7);
    auto connected = make_driver(fake);
    connected->set_connected(true);
    const auto live = connected->get_device_state();
    REQUIRE_FALSE(live.empty());
    for (const auto& entry : live) CHECK(entry.name != "Connected");
    connected->set_connected(false);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Unsupported methods", "[qhy][filterwheel][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "/dev/null");

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("QHY CFW3 protocol - slot to command character and back", "[qhy][filterwheel][cfw3][unit]") {
    using alpacacore::vendor::qhy::cfw3_parse_slot;
    using alpacacore::vendor::qhy::cfw3_slot_to_command;

    CHECK(cfw3_slot_to_command(0) == '0');
    CHECK(cfw3_slot_to_command(9) == '9');
    CHECK(cfw3_slot_to_command(10) == 'A');
    CHECK(cfw3_slot_to_command(15) == 'F');
    CHECK_FALSE(cfw3_slot_to_command(16).has_value());
    CHECK_FALSE(cfw3_slot_to_command(-1).has_value());
    for (int slot = 0; slot <= 15; ++slot) {
        CHECK(cfw3_parse_slot(*cfw3_slot_to_command(slot)) == slot);
    }
    CHECK_FALSE(cfw3_parse_slot('G').has_value());
    CHECK_FALSE(cfw3_parse_slot('a').has_value());
    CHECK_FALSE(cfw3_parse_slot('\n').has_value());
}

// ── Connect path over the pty fake ───────────────────────────────────────────

TEST_CASE("QHY CFW3 Filter Wheel Driver - Connects, reads slot count, firmware and position",
          "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(7);
    fake.set_position(2);
    auto driver = make_driver(fake);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.count("VRS") == 1);
    CHECK(fake.count("MXP") == 1);
    CHECK(fake.count("NOW") == 1);
    CHECK(driver->get_names().size() == 7);
    CHECK(driver->get_names()[0] == "Filter 1");
    CHECK(driver->get_focus_offsets().size() == 7);
    CHECK(driver->get_device_firmware() == std::string("20181114"));
    // Served from the handshake, no second NOW on the wire.
    CHECK(driver->get_position() == 2);
    CHECK(fake.count("NOW") == 1);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - The boot byte ends the boot wait early", "[qhy][filterwheel][cfw3][unit]") {
    // The real wheel emits its position ~17 s after the port is opened; the
    // driver must proceed on that byte, not sit out the whole timeout.
    FakeQhyCfw3 fake(7);
    fake.set_position(4);
    alpacacore::vendor::qhy::Cfw3Settings settings;
    settings.boot_timeout_ms = 5000;
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);

    // The open flushes whatever was buffered (a stale arrival reply from the
    // previous session), so the boot byte must land AFTER the open, as it
    // does on hardware: start the async connect, then emit it.
    const auto start = std::chrono::steady_clock::now();
    driver->connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fake.emit_boot_byte();
    while (driver->get_connecting() && std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(driver->get_connected());
    CHECK(elapsed < std::chrono::milliseconds(2500));
    CHECK(driver->get_position() == 4);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A wheel that answers nothing is refused", "[qhy][filterwheel][cfw3][unit]") {
    // Pre-201409 firmware, or a wheel left in 4-pin mode: MXP/NOW go
    // unanswered and the connect must fail with NotConnected, port released.
    FakeQhyCfw3 fake(7);
    fake.set_old_firmware(true);
    auto driver = make_driver(fake);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    // The port is free again: a second attempt reaches the wire.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK(fake.count("MXP") == 2);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A move reads -1 in transit then the arrived slot",
          "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(7);
    fake.set_travel_ms(120);
    auto driver = make_driver(fake);
    driver->set_connected(true);

    driver->set_position(3);
    // The goto returned before the wheel arrived (STANDARD-budget initiator);
    // Position is the moving sentinel until the arrival reply lands.
    CHECK(driver->get_position() == -1);
    CHECK(settle_position(*driver) == 3);
    CHECK(fake.position() == 3);
    CHECK(fake.count("3") == 1);
    // Settled reads come from the cache, never the wire.
    CHECK(fake.count("NOW") == 1);

    driver->set_connected(false);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Slot bounds from the wheel's own count", "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(5);
    auto driver = make_driver(fake);
    driver->set_connected(true);

    require_alpaca_error([&]() { driver->set_position(5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(12); }, alpacacore::AlpacaError::InvalidValue);
    // Nothing was put on the wire for a refused slot.
    CHECK(fake.count("5") == 0);
    CHECK(fake.count("C") == 0);
    // Names/offsets must now match the wheel.
    require_alpaca_error([&]() { driver->set_names({"L", "R"}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_focus_offsets({1, 2, 3}); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_names({"LRGBH"}));  // shorthand expands to one letter per slot
    CHECK(driver->get_names() == std::vector<std::string>{"L", "R", "G", "B", "H"});
    // Lowercase marks an ordinary name, not a shorthand: it stays one name,
    // which is the wrong length for a 5-slot wheel.
    require_alpaca_error([&]() { driver->set_names({"Clear"}); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_names() == std::vector<std::string>{"L", "R", "G", "B", "H"});
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A same-slot goto never touches the wire", "[qhy][filterwheel][cfw3][unit]") {
    // Pre-201409 firmware is silent on a same-slot goto, so the wait would
    // run to its timeout; skipping the wire is right for every firmware.
    FakeQhyCfw3 fake(7);
    fake.set_position(1);
    auto driver = make_driver(fake);
    driver->set_connected(true);

    driver->set_position(1);
    CHECK(driver->get_position() == 1);
    CHECK(fake.count("1") == 0);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A second goto mid-move is InvalidOperation",
          "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(7);
    fake.set_travel_ms(150);
    auto driver = make_driver(fake);
    driver->set_connected(true);

    driver->set_position(4);
    require_alpaca_error([&]() { driver->set_position(2); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(settle_position(*driver) == 4);
    // Once settled the wheel takes the next goto.
    driver->set_position(2);
    CHECK(settle_position(*driver) == 2);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - Disconnect during a move cancels the wait and joins",
          "[qhy][filterwheel][cfw3][unit]") {
    // Slot 0 -> 3 is three slots by shortest path: 2400 ms of travel, so an
    // uncancelled wait outlasts the 1000 ms bound below (a one-slot move
    // finished inside the old bound and passed with the cancel deleted).
    FakeQhyCfw3 fake(7);
    fake.set_travel_ms(800);
    auto driver = make_driver(fake);
    driver->set_connected(true);

    driver->set_position(3);
    CHECK(driver->get_position() == -1);
    const auto start = std::chrono::steady_clock::now();
    driver->set_connected(false);  // must not wait the full travel, and must not leave the move body running
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    // The pty carries the arrival reply later; a reconnect handshake must
    // still succeed with that stale byte in the buffer. Wait out the travel
    // (the fake is silent while it turns) before reconnecting.
    const auto settled_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int quiet_polls = 0;
    while (quiet_polls < 10 && std::chrono::steady_clock::now() < settled_by) {
        quiet_polls = fake.quiescent() ? quiet_polls + 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(quiet_polls == 10);
    driver->set_connected(true);
    CHECK(driver->get_position() == 3);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A failed move makes Position raise until the next Position PUT",
          "[qhy][filterwheel][cfw3][unit]") {
    // The wheel goes silent mid-move (hung MCU): the worker times out. The
    // ASCOM async FAQ makes a failed Position PUT a failed async operation, so
    // Position raises the kept failure (no -1, no live read that invents a
    // slot) until the next Position PUT or a reconnect clears it.
    FakeQhyCfw3 fake(7);
    alpacacore::vendor::qhy::Cfw3Settings settings = fast_settings();
    settings.move_timeout_ms = 300;
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);
    driver->set_connected(true);

    fake.set_muted(true);
    driver->set_position(3);
    // Wait for the worker to give up: Position stops reading -1.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool failed = false;
    std::string message;
    int code = 0;
    while (!failed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        try {
            failed = driver->get_position() != -1;
            if (failed) FAIL("Position returned a slot after a failed move");
        } catch (const alpacacore::AlpacaException& ex) {
            failed = true;
            message = ex.what();
            code = ex.error_code();
        }
    }
    REQUIRE(failed);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(message.find("move to slot 4 failed") != std::string::npos);
    // Kept: a second read raises the same failure and puts nothing on the wire.
    const int now_before = fake.count("NOW");
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::DriverException);
    CHECK(fake.count("NOW") == now_before);

    // The next Position PUT clears it.
    fake.set_muted(false);
    driver->set_position(3);
    CHECK(settle_position(*driver) == 3);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A reconnect clears a failed move", "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(7);
    alpacacore::vendor::qhy::Cfw3Settings settings = fast_settings();
    settings.move_timeout_ms = 300;
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);
    driver->set_connected(true);

    fake.set_muted(true);
    driver->set_position(3);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool failed = false;
    while (!failed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        try {
            (void)driver->get_position();
        } catch (const alpacacore::AlpacaException&) {
            failed = true;
        }
    }
    REQUIRE(failed);

    fake.set_muted(false);
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK_NOTHROW(driver->get_position());
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A severed link reads disconnected and reconnects",
          "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(7);
    auto driver = make_driver(fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    fake.sever();
    // Nothing is noticed until the next transaction (request/response, no
    // reader thread); a goto is that transaction.
    driver->set_position(2);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (driver->get_connected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    // Static data keeps answering.
    CHECK(driver->get_names().size() == 7);
    // Connected=true over the lost link is a real reconnect attempt, not the
    // idempotent no-op (#527): the port is gone, so it refuses.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    // Connected=false on the lost link still tears down cleanly.
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A reconnect on the same port skips the boot wait",
          "[qhy][filterwheel][cfw3][unit]") {
    // ConformU's Platform 7 Connect() abandons the device after 5 s of
    // Connecting, and the wheel's post-reset boot is ~17 s, so the port is
    // held open across disconnects: only the first connect waits.
    FakeQhyCfw3 fake(7);
    alpacacore::vendor::qhy::Cfw3Settings settings;
    settings.boot_timeout_ms = 1500;
    settings.reply_timeout_ms = 300;
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);

    auto first = std::chrono::steady_clock::now();
    driver->set_connected(true);
    CHECK(std::chrono::steady_clock::now() - first >= std::chrono::milliseconds(1400));
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());

    const auto again = std::chrono::steady_clock::now();
    driver->set_connected(true);
    CHECK(std::chrono::steady_clock::now() - again < std::chrono::milliseconds(700));
    REQUIRE(driver->get_connected());
    CHECK(fake.count("MXP") == 2);  // the handshake still runs on every connect
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A goto racing a disconnect never outlives the cancel",
          "[qhy][filterwheel][cfw3][unit]") {
    // PR #536 review: cancel_and_join_move() used to store the cancel flag
    // BEFORE taking the handle lock, and set_position() clears that flag
    // under the lock before spawning its worker. A goto that slipped in
    // between wiped the cancel, and the synchronous disconnect then sat out
    // the entire move (40 s by default, against the ~5 s an ASCOM client
    // gives Disconnect). Race the two from separate threads, many times, with
    // a travel time long enough that a lost cancel is unmistakable. Unforced,
    // the main-thread disconnect always won and no goto reached the wire (0 of
    // 25 rounds), so the cancel was never exercised: odd rounds wait for the
    // goto to be on the wire before disconnecting, even rounds stay a raw race.
    FakeQhyCfw3 fake(7);
    fake.set_travel_ms(1500);
    alpacacore::vendor::qhy::Cfw3Settings settings = fast_settings();
    settings.move_timeout_ms = 4000;
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);

    int gotos_seen = 0;
    int rounds_on_wire = 0;
    for (int round = 0; round < 25; ++round) {
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        const int target = 1 + (round % 6);
        gotos_seen = fake.count(std::string(1, static_cast<char>('0' + target)));
        std::thread mover([&] {
            try {
                driver->set_position(target);
            } catch (const alpacacore::AlpacaException&) {
                // NotConnected when the disconnect won: the expected outcome.
            }
        });
        if (round % 2 == 1) {
            const std::string goto_char(1, static_cast<char>('0' + target));
            const auto on_wire_by = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (fake.count(goto_char) <= gotos_seen && std::chrono::steady_clock::now() < on_wire_by) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            REQUIRE(fake.count(goto_char) > gotos_seen);
        }
        const auto start = std::chrono::steady_clock::now();
        driver->set_connected(false);
        const auto took = std::chrono::steady_clock::now() - start;
        mover.join();
        CHECK(took < std::chrono::milliseconds(800));
        CHECK_FALSE(driver->get_connected());
        // If the goto reached the wire the fake is mid-travel and, like the
        // real wheel, silent until it arrives; a reconnect handshake during
        // that window would time out. Wait until the fake has read and handled
        // everything the driver wrote. Counting the goto right after the join
        // is not enough: the byte can still be on its way to the fake's
        // reader, and under the -O0 coverage build it often was, so the round
        // skipped the wait and the next handshake met a travelling wheel. The
        // pty hands a write to the master side asynchronously, so quiescent()
        // must hold for a stretch, not once. Worst case is the fake's
        // shortest-path distance times its travel time (3 slots x 1.5 s from
        // slot 0), plus slack.
        const auto settled_by = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        int quiet_polls = 0;
        while (quiet_polls < 10 && std::chrono::steady_clock::now() < settled_by) {
            quiet_polls = fake.quiescent() ? quiet_polls + 1 : 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(quiet_polls == 10);
        const std::string goto_char(1, static_cast<char>('0' + target));
        if (fake.count(goto_char) > gotos_seen) {
            gotos_seen = fake.count(goto_char);
            REQUIRE(fake.position() == target);
            ++rounds_on_wire;
        }
    }
    // The forced rounds reached the wire, so the cancel was really raced.
    REQUIRE(rounds_on_wire >= 12);
}

TEST_CASE("QHY CFW3 Filter Wheel Driver - A reconnect mid-move waits out the arrival byte",
          "[qhy][filterwheel][cfw3][unit]") {
    // PR #536 review Note: with the port held open, a reconnect that lands
    // while the wheel is still turning must not read the arrival byte as the
    // MXP or NOW answer. The fake is silent during travel like the real wheel
    // and emits the slot on arrival; the handshake drains until idle first,
    // and a count below the position is refused outright.
    FakeQhyCfw3 fake(7);
    fake.set_travel_ms(120);
    auto driver = make_driver(fake);
    driver->set_connected(true);
    driver->set_position(5);
    CHECK(driver->get_position() == -1);
    // On a pty tcdrain() returns while the goto byte is still unread in the
    // buffer, and the held-port reconnect flushes the line (tcflush); a real
    // UART would have sent it. Wait until the fake has taken the byte so the
    // wheel is really mid-travel when the reconnect lands.
    const auto seen_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fake.count("5") == 0 && std::chrono::steady_clock::now() < seen_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(fake.count("5") > 0);
    driver->set_connected(false);  // cancels the wait; the fake keeps turning
    // Reconnect at once: the arrival '5' is still to come. The first attempt
    // must succeed: a retry loop would swallow a handshake that read the
    // arrival byte as its MXP or NOW answer.
    REQUIRE_NOTHROW(driver->set_connected(true));
    REQUIRE(driver->get_connected());
    // Whatever the timing, the wheel's real geometry survived the reconnect.
    CHECK(driver->get_names().size() == 7);
    // The arrival byte was drained, not swallowed into the VRS answer.
    CHECK(driver->get_device_firmware() == std::string("20181114"));
    CHECK(driver->get_position() == 5);
    REQUIRE_NOTHROW(driver->set_position(6));
    CHECK(settle_position(*driver) == 6);
}

// Issue #528: a synchronous disconnect that lands while a synchronous connect is inside the handshake must not be
// dropped. Without transition_mutex_ thread B saw "not connected" and returned as a no-op while A stored true.
TEST_CASE("QHY CFW3 Filter Wheel Driver - sync disconnect during sync connect is not dropped",
          "[qhy][filterwheel][cfw3][unit]") {
    FakeQhyCfw3 fake(7);
    auto driver = make_driver(fake);
    fake.hold_next_reply(std::chrono::milliseconds(150));  // the VRS answer: A stays inside the handshake

    std::thread a([&] {
        try {
            driver->set_connected(true);
        } catch (const std::exception&) {
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // inside A's boot wait, before it stores true
    driver->set_connected(false);
    a.join();

    CHECK(driver->get_connected() == false);
    CHECK(fake.connects() == 1);
    driver->set_connected(false);
}

// #508 item 1: a saved serial entry with an empty portPath is registered so it stays listed; its
// connect refuses in words the operator can act on instead of opening "" or auto-detecting.
TEST_CASE("QHY CFW3 Filter Wheel Driver - Connect with empty serial port names portPath", "[qhy][cfw3][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, "");
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

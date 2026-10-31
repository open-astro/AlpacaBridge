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
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>
#include <alpacacore/version.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_gemini_flatpanel.h"
#include "fake_task_clock.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

// Guards the PR #143 auto-detect fix: probe_port() must require the "*H"
// reply prefix instead of accepting any well-formed '#'-terminated reply, or
// it can misidentify the Gemini focuser's port as the flat panel when both
// devices are plugged in (both enumerate under the same CH340/CH341/
// USB_Serial/1a86 by-id patterns).
TEST_CASE("Gemini Flat Panel - handshake reply discrimination", "[gemini][flatpanel][unit]") {
    using alpacacore::vendor::gemini::is_flatpanel_handshake_reply;

    SECTION("accepts the confirmed real-hardware reply") {
        REQUIRE(is_flatpanel_handshake_reply("*HGeminiFlatPanelLite#") == true);
    }
    SECTION("accepts a bare *H prefix (future firmware/model string)") {
        REQUIRE(is_flatpanel_handshake_reply("*H#") == true);
    }
    SECTION("rejects a well-formed but wrong-letter reply (e.g. this driver's own >V#/>S#)") {
        REQUIRE(is_flatpanel_handshake_reply("*V206#") == false);
        REQUIRE(is_flatpanel_handshake_reply("*S011#") == false);
    }
    SECTION(
        "rejects a plausible non-flat-panel '#'-terminated reply, e.g. from the Gemini "
        "focuser's MyFocuserPro2 firmware answering an unrelated query on a shared-looking port") {
        REQUIRE(is_flatpanel_handshake_reply(":FD0000500#") == false);
    }
    SECTION("rejects short/empty responses") {
        REQUIRE(is_flatpanel_handshake_reply("") == false);
        REQUIRE(is_flatpanel_handshake_reply("*") == false);
        REQUIRE(is_flatpanel_handshake_reply("H") == false);
    }
    SECTION("requires the '*' before 'H', not just an 'H' anywhere") {
        REQUIRE(is_flatpanel_handshake_reply("H*#") == false);
    }
}

TEST_CASE("Gemini Flat Panel Driver - Defaults", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::CoverCalibrator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Gemini Astro Flat Panel Cover Lite");
    // MaxBrightness is a property: disconnected it refuses (issue #1478), the value is read connected.
    require_alpaca_error([&]() { (void)driver->get_max_brightness(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini Flat Panel Driver - Device metadata", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Gemini Astro Flat Panel Cover Lite Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Gemini Flat Panel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "GEMINI_FLATPANEL_3");
}

TEST_CASE("Gemini Flat Panel Driver - Firmware unavailable when disconnected", "[gemini][flatpanel][unit][fake]") {
    alpacacore::test::FakeGeminiFlatPanel panel;
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, panel.slave_path(), 9600);
    REQUIRE(driver->get_device_firmware() == std::nullopt);

    // Firmware is read from the panel at connect, so it is only known while
    // connected. Disconnected, the web UI shows no firmware row: the cached
    // string from the last connect must be gone.
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_device_firmware() == std::optional<std::string>("107"));

    driver->set_connected(false);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_device_firmware() == std::nullopt);
}

TEST_CASE("Gemini Flat Panel Driver - Not connected throws", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    // Every property/method that needs a live link must throw NotConnected
    // (0x407) so ConformU sees the correct Alpaca error number.
    require_alpaca_error([&]() { driver->get_brightness(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_max_brightness(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_changing(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_off(); }, alpacacore::AlpacaError::NotConnected);
    // A valid-brightness CalibratorOn passes range validation, then trips on the
    // connection check.
    require_alpaca_error([&]() { driver->calibrator_on(128); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini Flat Panel Driver - Unsupported actions", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Gemini Flat Panel Driver - No motorized cover", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    // This model is light-only: OpenCover/CloseCover/HaltCover are a
    // structurally absent capability (like SlewToAltAz on a mount without alt-
    // az slewing), so they throw MethodNotImplemented unconditionally --
    // never NotConnected -- even while disconnected.
    require_alpaca_error([&]() { driver->open_cover(); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->close_cover(); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Gemini Flat Panel Driver - Value range validation", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    // Out-of-range brightness must throw InvalidValue (0x401), not normalize.
    require_alpaca_error([&]() { driver->calibrator_on(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(256); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(1000); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_brightness(-5); }, alpacacore::AlpacaError::InvalidValue);

    // In-range boundary values are accepted by validation, then hit NotConnected.
    require_alpaca_error([&]() { driver->calibrator_on(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_on(255); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini Flat Panel Driver - State machine", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    // Not connecting and not connected at rest.
    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list. The non-compliant "Connected"
    // entry must never appear.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "CoverState");
        REQUIRE(entry.name != "CalibratorState");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("Gemini Flat Panel Driver - Create by index for auto-detect", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_by_index(1, 0);

    REQUIRE(driver->get_device_number() == 1);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_unique_id() == "GEMINI_FLATPANEL_1");
}

// ---------------------------------------------------------------------------
// Operation slots (the Rev2/Pro driver class): the cover and calibrator
// background bodies run in util::AsyncOperation slots over the pty-backed
// fake Pro panel (fake_gemini_flatpanel.h).
// ---------------------------------------------------------------------------

namespace {

using alpacacore::CalibratorState;
using alpacacore::CoverState;
using alpacacore::test::FakeGeminiFlatPanel;
using std::chrono::milliseconds;

std::unique_ptr<alpacacore::CoverCalibratorDriver> connect_slot_pro(const FakeGeminiFlatPanel& panel) {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

template <typename Pred>
bool slot_wait_until(Pred pred, milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(5));
    }
    return pred();
}

// A reply the wrapper rejects at once: longer than its 32-character limit
// and never terminated.
const std::string kOverlongReply(40, 'X');

}  // namespace

TEST_CASE("Gemini Flat Panel Slots - calibrator commands queued behind a cover move return at once and run in order",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">O#", milliseconds(1500));  // a long "mechanical" open holds the port
    auto driver = connect_slot_pro(panel);

    const auto t_cover = std::chrono::steady_clock::now();
    driver->open_cover();
    CHECK(std::chrono::steady_clock::now() - t_cover < milliseconds(150));
    REQUIRE(slot_wait_until([&] { return panel.count(">O#") == 1; }, milliseconds(1000)));

    // More commands than the slot keeps replaced bodies (3): none may block
    // the caller for the slot's stale-reap wait or throw.
    for (int level = 10; level <= 70; level += 10) {
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE_NOTHROW(driver->calibrator_on(level));
        CHECK(std::chrono::steady_clock::now() - t0 < milliseconds(150));
    }
    CHECK(driver->get_calibrator_changing());
    CHECK(driver->get_calibrator_state() == CalibratorState::NotReady);

    REQUIRE(slot_wait_until([&] { return !driver->get_calibrator_changing(); }, milliseconds(6000)));
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(driver->get_brightness() == 70);
    CHECK(panel.brightness() == 70);

    // Every queued pair reached the wire, after the cover command, in
    // submission order.
    int previous = panel.index_of(">O#");
    for (int level = 10; level <= 70; level += 10) {
        const int at = panel.index_of(">B" + std::to_string(level) + "#");
        REQUIRE(at > previous);
        previous = at;
    }
    CHECK(panel.count(">L#") == 7);
}

TEST_CASE("Gemini Flat Panel Slots - a cover body that fails reports Error until the next cover command",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_override(">O#", "*?#");  // the firmware refuses the open
    auto driver = connect_slot_pro(panel);

    driver->open_cover();
    REQUIRE(slot_wait_until([&] { return !driver->get_cover_moving(); }, milliseconds(3000)));
    // Never a live >S# read in between: Error from the moment the body ends.
    for (int i = 0; i < 3; ++i) {
        CHECK(driver->get_cover_state() == CoverState::Error);
        std::this_thread::sleep_for(milliseconds(20));
    }
    // The TTL cache window does not heal it.
    std::this_thread::sleep_for(milliseconds(1100));
    CHECK(driver->get_cover_state() == CoverState::Error);

    // The next start of the family clears it; a command that succeeds leaves
    // the live state.
    driver->close_cover();
    REQUIRE(
        slot_wait_until([&] { return panel.count(">C#") == 1 && !driver->get_cover_moving(); }, milliseconds(3000)));
    CHECK(driver->get_cover_state() == CoverState::Closed);
}

TEST_CASE("Gemini Flat Panel Slots - a calibrator body that fails reports Error until the next calibrator command",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">O#", milliseconds(300));
    panel.set_reply_override(">L#", kOverlongReply);
    auto driver = connect_slot_pro(panel);

    driver->open_cover();
    REQUIRE(slot_wait_until([&] { return panel.count(">O#") == 1; }, milliseconds(1000)));
    driver->calibrator_on(80);  // background path: the cover holds the port
    REQUIRE(slot_wait_until([&] { return !driver->get_calibrator_changing(); }, milliseconds(3000)));
    CHECK(driver->get_calibrator_state() == CalibratorState::Error);
    CHECK(driver->get_brightness() == 0);  // the failed command was not committed

    // Next command (inline, the port is free): the Error is gone once it succeeds.
    REQUIRE(slot_wait_until([&] { return !driver->get_cover_moving(); }, milliseconds(3000)));
    panel.set_reply_override(">L#", "");
    driver->calibrator_on(50);
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(driver->get_brightness() == 50);
}

TEST_CASE("Gemini Flat Panel Slots - disconnect drops queued commands, joins, and a reconnect starts clean",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">O#", milliseconds(700));
    auto driver = connect_slot_pro(panel);

    driver->open_cover();
    REQUIRE(slot_wait_until([&] { return panel.count(">O#") == 1; }, milliseconds(1000)));
    driver->calibrator_on(30);
    driver->calibrator_on(40);
    driver->calibrator_on(50);
    REQUIRE(driver->get_calibrator_changing());

    driver->set_connected(false);  // joins the cover body and drops the queued light commands
    CHECK_FALSE(driver->get_connected());
    // The first command may already be committed to the port when the
    // disconnect lands (its transaction then finishes); the other two never go out.
    CHECK(panel.count(">L#") <= 1);

    driver->set_connected(true);
    CHECK_FALSE(driver->get_calibrator_changing());
    CHECK(driver->get_calibrator_state() == (panel.light_on() ? CalibratorState::Ready : CalibratorState::Off));
    CHECK_FALSE(driver->get_cover_moving());
    driver->calibrator_on(25);
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(panel.brightness() == 25);
}

TEST_CASE("Gemini Flat Panel Slots - disconnect cancels a cover body still waiting for a calibrator toggle",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">B", milliseconds(600));  // slow brightness ack holds the inline toggle
    auto driver = connect_slot_pro(panel);

    std::thread toggler([&] { driver->calibrator_on(200); });
    REQUIRE(slot_wait_until([&] { return panel.count(">L#") == 1; }, milliseconds(1000)));
    driver->open_cover();  // its body waits for the toggle to release the port
    driver->set_connected(false);
    toggler.join();

    CHECK_FALSE(driver->get_connected());
    CHECK(panel.count(">O#") == 0);  // cancelled before it reached the wire
    driver->set_connected(true);
    CHECK_FALSE(driver->get_cover_moving());
}

TEST_CASE("Gemini Flat Panel Slots - the idle CoverState cache ages on the injected clock",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    const int seeded = panel.count(">S#");
    REQUIRE(seeded >= 1);
    std::this_thread::sleep_for(milliseconds(1100));  // real time passing changes nothing
    CHECK(driver->get_cover_state() == CoverState::Closed);
    CHECK(panel.count(">S#") == seeded);

    clock.advance(milliseconds(1100));
    CHECK(driver->get_cover_state() == CoverState::Closed);
    CHECK(panel.count(">S#") == seeded + 1);
}

// ---------------------------------------------------------------------------
// Lite (v1) class: Brightness / CalibratorState are published state and must
// not wait for a CalibratorOn / CalibratorOff serial exchange (decision 0016).
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<alpacacore::CoverCalibratorDriver> connect_lite(const FakeGeminiFlatPanel& panel) {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, panel.slave_path(), 9600);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

}  // namespace

TEST_CASE("Gemini Flat Panel Lite - Brightness and CalibratorState do not wait for a calibrator command",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    auto driver = connect_lite(panel);

    SECTION("CalibratorOn in flight") {
        panel.set_reply_delay(">L#", milliseconds(300));
        std::jthread caller([&] { driver->calibrator_on(40); });
        REQUIRE(slot_wait_until([&] { return panel.count(">L#") == 1; }, milliseconds(1000)));

        const auto t0 = std::chrono::steady_clock::now();
        (void)driver->get_brightness();
        (void)driver->get_calibrator_state();
        CHECK(std::chrono::steady_clock::now() - t0 < milliseconds(20));
        caller.join();
        CHECK(driver->get_brightness() == 40);
        CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    }

    SECTION("CalibratorOff in flight") {
        driver->calibrator_on(40);
        panel.set_reply_delay(">D#", milliseconds(300));
        std::jthread caller([&] { driver->calibrator_off(); });
        REQUIRE(slot_wait_until([&] { return panel.count(">D#") == 1; }, milliseconds(1000)));

        const auto t0 = std::chrono::steady_clock::now();
        (void)driver->get_brightness();
        (void)driver->get_calibrator_state();
        CHECK(std::chrono::steady_clock::now() - t0 < milliseconds(20));
        caller.join();
        CHECK(driver->get_brightness() == 0);
        CHECK(driver->get_calibrator_state() == CalibratorState::Off);
    }
}

TEST_CASE("Gemini Flat Panel Lite - a failed calibrator command restores the published state",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    auto driver = connect_lite(panel);
    driver->calibrator_on(40);

    panel.set_reply_override(">B", kOverlongReply);
    CHECK_THROWS(driver->calibrator_on(90));
    CHECK(driver->get_brightness() == 40);
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
}

TEST_CASE("Gemini Flat Panel Lite - set_connected(false) returns promptly with a command in flight",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    auto driver = connect_lite(panel);
    panel.set_reply_delay(">L#", milliseconds(300));
    std::jthread caller([&] {
        try {
            driver->calibrator_on(40);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(slot_wait_until([&] { return panel.count(">L#") == 1; }, milliseconds(1000)));

    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    CHECK_FALSE(driver->get_connected());
    caller.join();
}

TEST_CASE("Gemini Flat Panel Lite - a command finishing during a disconnect does not republish its state",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    auto driver = connect_lite(panel);
    panel.set_reply_delay(">L#", milliseconds(300));
    std::jthread caller([&] { driver->calibrator_on(40); });
    REQUIRE(slot_wait_until([&] { return panel.count(">L#") == 1; }, milliseconds(1000)));

    driver->set_connected(false);
    caller.join();

    // A failing seed read leaves the cleared state in place on reconnect.
    panel.set_reply_override(">S", kOverlongReply);
    driver->set_connected(true);
    CHECK(driver->get_brightness() == 0);
    CHECK(driver->get_calibrator_state() == CalibratorState::Off);
}

TEST_CASE("Gemini Flat Panel Driver - connected, MaxBrightness is 255 and Brightness is 0 while the light is off",
          "[gemini][flatpanel][unit][fake]") {
    alpacacore::test::FakeGeminiFlatPanel panel;
    // The panel remembers a level while its light is off; ICoverCalibratorV2 wants Brightness 0 then (issue #1478).
    panel.set_reply_override(">J#", "*J128#");
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, panel.slave_path(), 9600);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_max_brightness() == 255);
    REQUIRE(driver->get_calibrator_state() == alpacacore::CalibratorState::Off);
    CHECK(driver->get_brightness() == 0);
}

// A device that drops off the bus between the command and its reply: the wrapper reports the hangup and the
// errno at once, not a read timeout after serial_timeout_s.
// Falsified by: gemini_flatpanel_protocol_wrapper.cpp:842 read_response() sending ReadOutcome::LinkDead to the timeout
// branch (the message reads "Read timeout" and the call takes the full serial_timeout_s).
TEST_CASE("Gemini flat panel protocol wrapper - a hangup before the reply throws serial link lost with the errno",
          "[gemini][flatpanel][unit][link-dead]") {
    FakeGeminiFlatPanel panel;
    alpacacore::vendor::gemini::GeminiFlatPanelProtocolWrapper wrapper;
    alpacacore::vendor::gemini::FlatPanelConnectionConfig config;
    config.serial_port = panel.slave_path();
    wrapper.connect(config);
    panel.set_reply_delay(">S#",
                          std::chrono::milliseconds(300));  // the panel sits on the >S# reply while the cable is pulled
    std::thread pull([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        panel.unplug();
    });
    const auto start = std::chrono::steady_clock::now();
    std::string what;
    try {
        wrapper.get_light_on();
        FAIL("a hung-up port must not return a value");
    } catch (const alpacacore::AlpacaException& ex) {
        what = ex.what();
    }
    pull.join();
    CHECK(what == "Serial link lost (errno 5)");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    try {
        wrapper.disconnect();
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Decision 0009 rule 2 (issue #1723): a pulled adapter loses the link at once.
// Connected reads false, every member throws NotConnected, DeviceState is
// empty, the port is released, and only a client Connect reopens it.
// ---------------------------------------------------------------------------

namespace {

// A by-id style symlink the driver is configured with, so a replugged panel (a
// new fake on a new pty) comes back under the same name, as a by-id path does.
// The driver never holds the raw /dev/pts/N, which devpts reissues once freed.
struct ReplugPort {
    std::filesystem::path path;
    explicit ReplugPort(const FakeGeminiFlatPanel& panel) {
        static std::atomic<int> serial{0};
        path = std::filesystem::temp_directory_path() /
               ("alpacacore-gemini-panel-" + std::to_string(::getpid()) + "-" + std::to_string(serial.fetch_add(1)));
        std::filesystem::create_symlink(panel.slave_path(), path);
    }
    void replug(const FakeGeminiFlatPanel& panel) {
        std::filesystem::remove(path);
        std::filesystem::create_symlink(panel.slave_path(), path);
    }
    ~ReplugPort() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ReplugPort(const ReplugPort&) = delete;
    ReplugPort& operator=(const ReplugPort&) = delete;
};

// The AlpacaException code fn threw, or 0. Safe on a non-test thread.
int alpaca_code_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.error_code();
    } catch (...) {
        return -1;
    }
    return 0;
}

constexpr int kNotConnected = static_cast<int>(alpacacore::AlpacaError::NotConnected);

// The members a lost link refuses, Lite and Rev2/Pro alike.
void require_lost(alpacacore::CoverCalibratorDriver& driver, const std::string& old_pts) {
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_device_state().empty());
    require_alpaca_error([&] { (void)driver.get_brightness(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_calibrator_state(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_cover_state(); }, kNotConnected);
    require_alpaca_error([&] { driver.calibrator_on(10); }, kNotConnected);
    require_alpaca_error([&] { driver.calibrator_off(); }, kNotConnected);
    // The wrapper released the port: no claim is left on the dead node.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(old_pts));
}

}  // namespace

TEST_CASE("Gemini Flat Panel Lite - a cable pulled during a command loses the link at once and Connect reopens",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    FakeGeminiFlatPanel panel;
    FakeGeminiFlatPanel replugged;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, port.path.string(), 9600);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = panel.slave_path();
    REQUIRE(alpacacore::util::is_serial_port_in_use(old_pts));

    panel.set_reply_delay(">L#", milliseconds(300));  // the panel sits on the >L# reply while the cable is pulled
    std::atomic<int> code{0};
    std::jthread caller([&] { code.store(alpaca_code_of([&] { driver->calibrator_on(40); })); });
    REQUIRE(slot_wait_until([&] { return panel.count(">L#") == 1; }, milliseconds(1000)));
    const auto pulled = std::chrono::steady_clock::now();
    panel.unplug();
    // Real time: the wait is for the driver's read to see the hangup on the wire.
    REQUIRE(slot_wait_until([&] { return !driver->get_connected(); }, milliseconds(5000)));
    CHECK(std::chrono::steady_clock::now() - pulled < std::chrono::seconds(5));  // one exchange timeout
    caller.join();
    CHECK(code.load() == kNotConnected);
    require_lost(*driver, old_pts);
    // Decision 0009 rule 5: the loss reason is kept while the link stays lost.
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);
    require_alpaca_error([&] { driver->calibrator_on(40); }, kNotConnected);
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);

    // Nothing reopens it on its own; a client Connect over the lost link does.
    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    driver->calibrator_on(30);
    CHECK(driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(replugged.count(">L#") == 1);
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini Flat Panel Lite - a write to a pulled adapter loses the link at once",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    FakeGeminiFlatPanel panel;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, port.path.string(), 9600);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = panel.slave_path();

    panel.unplug();  // idle: the next command's write is what meets the dead port
    require_alpaca_error([&] { driver->calibrator_on(40); }, kNotConnected);
    require_lost(*driver, old_pts);
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK(driver->get_link_fault().empty());  // a Disconnect clears the reason
}

TEST_CASE("Gemini Flat Panel Slots - a cable pulled during a cover move loses the link and a reconnect starts clean",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;  // before the driver, which waits on it
    FakeGeminiFlatPanel panel;
    FakeGeminiFlatPanel replugged;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, port.path.string(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = panel.slave_path();

    panel.set_reply_delay(">O#", milliseconds(300));  // the cover is travelling when the cable is pulled
    driver->open_cover();
    REQUIRE(slot_wait_until([&] { return panel.count(">O#") == 1; }, milliseconds(1000)));
    const auto pulled = std::chrono::steady_clock::now();
    panel.unplug();
    // Real time: the cover body's read sees the hangup on the wire.
    REQUIRE(slot_wait_until([&] { return !driver->get_connected(); }, milliseconds(5000)));
    CHECK(std::chrono::steady_clock::now() - pulled < std::chrono::seconds(5));  // well inside the 30 s cover wait
    require_lost(*driver, old_pts);
    // Decision 0009 rule 5: the loss reason is kept while the link stays lost.
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);
    require_alpaca_error([&] { driver->open_cover(); }, kNotConnected);
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);
    require_alpaca_error([&] { driver->halt_cover(); }, kNotConnected);

    // Disconnect after the loss joins the failed body and closes nothing twice.
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // a Disconnect clears the reason

    // The reconnect clears the body's kept failure: the cover reads its live state.
    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_cover_state() == CoverState::Closed);
    CHECK(driver->get_calibrator_state() == CalibratorState::Off);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

TEST_CASE("Gemini Flat Panel Slots - an idle CoverState read that finds the cable gone throws NotConnected",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;  // before the driver, which waits on it
    FakeGeminiFlatPanel panel;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, port.path.string(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    clock.advance(milliseconds(1100));  // the cached status is stale: the next CoverState read goes to the wire
    panel.unplug();
    require_alpaca_error([&] { (void)driver->get_cover_state(); }, kNotConnected);
    CHECK_FALSE(driver->get_connected());
    REQUIRE_NOTHROW(driver->set_connected(false));
}

TEST_CASE("Gemini Flat Panel Lite - a cable pulled during the connect state seed fails the Connect",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    FakeGeminiFlatPanel panel;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, port.path.string(), 9600);

    panel.set_reply_delay(">S#", milliseconds(300));  // the seed's first read sits on the reply while the cable goes
    std::atomic<int> code{0};
    std::jthread connecting([&] { code.store(alpaca_code_of([&] { driver->set_connected(true); })); });
    REQUIRE(slot_wait_until([&] { return panel.count(">S#") == 1; }, milliseconds(2000)));
    panel.unplug();
    connecting.join();
    CHECK(code.load() == kNotConnected);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini Flat Panel Slots - a cable pulled during the connect state seed fails the Connect",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;  // before the driver, which waits on it
    FakeGeminiFlatPanel panel;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, port.path.string(), 9600, clock);

    panel.set_reply_delay(">S#", milliseconds(300));  // the seed's first read sits on the reply while the cable goes
    std::atomic<int> code{0};
    std::jthread connecting([&] { code.store(alpaca_code_of([&] { driver->set_connected(true); })); });
    REQUIRE(slot_wait_until([&] { return panel.count(">S#") == 1; }, milliseconds(2000)));
    panel.unplug();
    connecting.join();
    CHECK(code.load() == kNotConnected);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini Flat Panel Slots - a Connect over a lost link reopens without a Disconnect first",
          "[gemini][flatpanel][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;  // before the driver, which waits on it
    FakeGeminiFlatPanel panel;
    FakeGeminiFlatPanel replugged;
    ReplugPort port(panel);
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, port.path.string(), 9600, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    panel.unplug();
    require_alpaca_error([&] { driver->calibrator_on(10); }, kNotConnected);  // the write meets the dead port
    REQUIRE_FALSE(driver->get_connected());

    // No Disconnect: a Connect while the raw flag is still set must not be taken as already connected.
    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_cover_state() == CoverState::Closed);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

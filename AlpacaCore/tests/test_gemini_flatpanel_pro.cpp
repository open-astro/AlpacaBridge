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
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <optional>
#include <thread>

#include "catch2_compat.h"
#include "fake_gemini_flatpanel.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

}  // namespace

TEST_CASE("Gemini Flat Panel Pro Driver - Defaults", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::CoverCalibrator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Gemini Motorized Flat Panel V3");
    // MaxBrightness is a static capability and must be readable without a connection.
    // 255 matches INDI's Rev2 adapter (setBrightness clamps to 0-255, same as the Lite model).
    CHECK(driver->get_max_brightness() == 255);
}

TEST_CASE("Gemini Flat Panel Pro Driver - Device metadata", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Gemini Motorized Flat Panel V3 (Pro) Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Gemini Flat Panel Pro Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "GEMINI_FLATPANEL_PRO_3");
}

TEST_CASE("Gemini Flat Panel Pro Driver - Firmware unavailable when disconnected", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_device_firmware() == std::nullopt);
}

TEST_CASE("Gemini Flat Panel Pro Driver - Not connected throws", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

    // Every property/method that needs a live link must throw NotConnected
    // (0x407) so ConformU sees the correct Alpaca error number.
    require_alpaca_error([&]() { driver->get_brightness(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_changing(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_state(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_off(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_on(128); }, alpacacore::AlpacaError::NotConnected);

    // Unlike the Cover Lite (light-only, no motor -- these three always
    // throw MethodNotImplemented, connected or not), this model HAS a
    // motorized cover, so OpenCover/CloseCover/HaltCover are real operations
    // gated by connection state like everything else, not a structurally
    // absent capability.
    require_alpaca_error([&]() { driver->open_cover(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->close_cover(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini Flat Panel Pro Driver - Unsupported actions", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

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

TEST_CASE("Gemini Flat Panel Pro Driver - Motorized cover is a real capability, not structurally absent",
          "[gemini][flatpanel][unit]") {
    // This is the key contrast with the Cover Lite (test_gemini_flatpanel.cpp
    // "No motorized cover" case): there, OpenCover/CloseCover/HaltCover throw
    // MethodNotImplemented UNCONDITIONALLY, even disconnected, because the
    // capability doesn't exist on that hardware at all. Here it does exist,
    // so the SAME calls must throw NotConnected instead (asserted above) --
    // never MethodNotImplemented -- while disconnected.
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

    try {
        driver->open_cover();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() != alpacacore::AlpacaError::MethodNotImplemented);
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    try {
        driver->halt_cover();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() != alpacacore::AlpacaError::MethodNotImplemented);
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
    }
}

TEST_CASE("Gemini Flat Panel Pro Driver - Value range validation", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

    // Out-of-range brightness must throw InvalidValue (0x401), not normalize,
    // and range validation happens BEFORE the connection check.
    require_alpaca_error([&]() { driver->calibrator_on(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(256); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(1000); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_brightness(-5); }, alpacacore::AlpacaError::InvalidValue);

    // In-range boundary values pass validation, then hit NotConnected.
    require_alpaca_error([&]() { driver->calibrator_on(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_on(255); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini Flat Panel Pro Driver - State machine", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");

    // Not connecting and not connected at rest.
    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, leaving just the TimeStamp. The non-compliant "Connected"
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
    REQUIRE(has_timestamp);
}

TEST_CASE("Gemini Flat Panel Pro Driver - Create by index for auto-detect", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro_by_index(1, 0);

    REQUIRE(driver->get_device_number() == 1);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_unique_id() == "GEMINI_FLATPANEL_PRO_1");
}

TEST_CASE("Gemini Flat Panel Pro Driver - Expected handshake identity per model", "[gemini][flatpanel][unit]") {
    using alpacacore::vendor::gemini::expected_flatpanel_handshake_reply;
    using alpacacore::vendor::gemini::FlatPanelModel;
    // Confirmed on hardware: Lite fw 206, Rev2 fw 408, Pro fw 107. The Pro
    // string is what the "Motorized Flat Panel V3" answers to >H#.
    CHECK(expected_flatpanel_handshake_reply(FlatPanelModel::Pro) == "*HGeminiFlatPanelPro#");
    CHECK(expected_flatpanel_handshake_reply(FlatPanelModel::Rev2) == "*HGeminiFlatPanel#");
    CHECK(expected_flatpanel_handshake_reply(FlatPanelModel::Lite) == "*HGeminiFlatPanelLite#");
    // All three still pass the generic "*H" gate used by auto-detect.
    CHECK(alpacacore::vendor::gemini::is_flatpanel_handshake_reply("*HGeminiFlatPanelPro#"));
}

TEST_CASE("Gemini Flat Panel Pro Driver - Unsupported methods", "[gemini][flatpanel][unit]") {
    // Pro has no wire-level abort either (INDI's AbortCap() just returns OK),
    // so HaltCover must FUNCTION (ConformU rule for cover-capable devices)
    // rather than throw MethodNotImplemented; disconnected it gates on
    // NotConnected like every other cover member.
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, "/dev/ttyUSB0");
    require_alpaca_error([&]() { driver->halt_cover(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->open_cover(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->close_cover(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_max_brightness() == 255);
}

// ---------------------------------------------------------------------------
// Wire-level behaviour over a pty-backed fake Pro panel (see
// fake_gemini_flatpanel.h). These cover the two paths the PR #226 review
// asked for: the inline calibrator fast path vs the background path, and
// the idle CoverState TTL cache.
// ---------------------------------------------------------------------------

namespace {

using alpacacore::test::FakeGeminiFlatPanel;

std::unique_ptr<alpacacore::CoverCalibratorDriver> connect_fake_pro(const FakeGeminiFlatPanel& panel) {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace

TEST_CASE("Gemini Flat Panel Pro Driver - CalibratorOn takes the inline fast path when the port is free",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    auto driver = connect_fake_pro(panel);

    const auto t0 = std::chrono::steady_clock::now();
    driver->calibrator_on(128);
    const auto took = std::chrono::steady_clock::now() - t0;

    // Returned with the state already applied (no NotReady window) and the
    // on/off + brightness pair went out back-to-back as one transaction.
    CHECK(took < std::chrono::milliseconds(500));
    CHECK_FALSE(driver->get_calibrator_changing());
    CHECK(driver->get_calibrator_state() == alpacacore::CalibratorState::Ready);
    CHECK(driver->get_brightness() == 128);
    CHECK(panel.light_on());
    CHECK(panel.brightness() == 128);
    const int l = panel.index_of(">L#");
    const int b = panel.index_of(">B128#");
    REQUIRE(l >= 0);
    CHECK(b == l + 1);

    driver->calibrator_off();
    CHECK(driver->get_calibrator_state() == alpacacore::CalibratorState::Off);
    CHECK_FALSE(panel.light_on());
    CHECK(panel.index_of(">B0#") == panel.index_of(">D#") + 1);
}

TEST_CASE("Gemini Flat Panel Pro Driver - CalibratorOn goes to the background path while a cover move holds the port",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">O#", std::chrono::milliseconds(400));  // a "mechanical" open
    auto driver = connect_fake_pro(panel);

    driver->open_cover();
    CHECK(driver->get_cover_state() == alpacacore::CoverState::Moving);
    // Let the cover task actually put >O# on the wire (the fake records a
    // command on receipt, before its delayed ack), so the port is genuinely
    // held for the next 400 ms.
    REQUIRE(wait_until([&] { return panel.count(">O#") == 1; }, std::chrono::milliseconds(1000)));

    const auto t0 = std::chrono::steady_clock::now();
    driver->calibrator_on(64);
    const auto took = std::chrono::steady_clock::now() - t0;

    // Must not block behind the cover move; reports NotReady until its
    // queued wire command has actually run.
    CHECK(took < std::chrono::milliseconds(150));
    CHECK(driver->get_calibrator_changing());
    CHECK(driver->get_calibrator_state() == alpacacore::CalibratorState::NotReady);

    REQUIRE(wait_until([&] { return !driver->get_calibrator_changing(); }, std::chrono::milliseconds(3000)));
    CHECK(driver->get_calibrator_state() == alpacacore::CalibratorState::Ready);
    CHECK(panel.brightness() == 64);
    // The light commands were serialized behind the in-flight cover move.
    CHECK(panel.index_of(">L#") > panel.index_of(">O#"));

    REQUIRE(wait_until([&] { return !driver->get_cover_moving(); }, std::chrono::milliseconds(3000)));
    CHECK(driver->get_cover_state() == alpacacore::CoverState::Open);
}

TEST_CASE("Gemini Flat Panel Pro Driver - A cover move issued mid-toggle waits for the inline toggle",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">B", std::chrono::milliseconds(300));  // slow brightness ack
    auto driver = connect_fake_pro(panel);

    std::thread toggler([&] { driver->calibrator_on(200); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));  // toggle is now on the wire
    const auto t0 = std::chrono::steady_clock::now();
    driver->open_cover();  // async initiator: returns at once, task waits for the toggle
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150));
    toggler.join();

    // The inline toggle completed without being stalled behind the cover
    // move, and the cover command went out only after the brightness ack.
    CHECK(driver->get_calibrator_state() == alpacacore::CalibratorState::Ready);
    REQUIRE(wait_until([&] { return !driver->get_cover_moving(); }, std::chrono::milliseconds(3000)));
    CHECK(panel.index_of(">O#") > panel.index_of(">B200#"));
    CHECK(driver->get_cover_state() == alpacacore::CoverState::Open);
}

TEST_CASE("Gemini Flat Panel Pro Driver - Idle CoverState is served from the TTL cache and invalidated by a move",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    auto driver = connect_fake_pro(panel);

    // Connect seeded the cache with one >S#; a burst of reads must not hit
    // the wire again.
    const int seeded = panel.count(">S#");
    REQUIRE(seeded >= 1);
    for (int i = 0; i < 5; ++i) {
        CHECK(driver->get_cover_state() == alpacacore::CoverState::Closed);
    }
    CHECK(panel.count(">S#") == seeded);

    // A completed move invalidates the cache: the next read is live and
    // sees the new position.
    driver->open_cover();
    REQUIRE(wait_until([&] { return !driver->get_cover_moving(); }, std::chrono::milliseconds(3000)));
    CHECK(driver->get_cover_state() == alpacacore::CoverState::Open);
    const int after_move = panel.count(">S#");
    CHECK(after_move == seeded + 1);

    // Within the TTL: cached. After it lapses: one more live read.
    CHECK(driver->get_cover_state() == alpacacore::CoverState::Open);
    CHECK(panel.count(">S#") == after_move);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(driver->get_cover_state() == alpacacore::CoverState::Open);
    CHECK(panel.count(">S#") == after_move + 1);
}

TEST_CASE("Gemini Flat Panel Pro Driver - A CalibratorOff overlapping an inline CalibratorOn keeps submission order",
          "[gemini][flatpanel][unit][fake]") {
    FakeGeminiFlatPanel panel;
    panel.set_reply_delay(">B", std::chrono::milliseconds(300));  // slow brightness ack holds the toggle
    auto driver = connect_fake_pro(panel);

    std::thread on_thread([&] { driver->calibrator_on(200); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));  // the ON toggle is on the wire
    const auto t0 = std::chrono::steady_clock::now();
    driver->calibrator_off();  // pending != 0 -> background path, must queue behind the inline ON
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150));
    CHECK(driver->get_calibrator_changing());
    on_thread.join();

    REQUIRE(wait_until([&] { return !driver->get_calibrator_changing(); }, std::chrono::milliseconds(3000)));
    // The later command wins: panel off, state Off, and the wire order is
    // ON's pair then OFF's pair.
    CHECK(driver->get_calibrator_state() == alpacacore::CalibratorState::Off);
    CHECK(driver->get_brightness() == 0);
    CHECK_FALSE(panel.light_on());
    CHECK(panel.index_of(">D#") > panel.index_of(">B200#"));
    CHECK(panel.index_of(">B0#") == panel.index_of(">D#") + 1);
}

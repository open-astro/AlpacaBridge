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

#include <functional>
#include <optional>

#include "catch2_compat.h"

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
    // MaxBrightness is a static capability and must be readable without a connection.
    CHECK(driver->get_max_brightness() == 255);
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

TEST_CASE("Gemini Flat Panel Driver - Firmware unavailable when disconnected", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    // Firmware is read from the panel at connect, so it is only known while
    // connected. Disconnected, the web UI shows no firmware row.
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_device_firmware() == std::nullopt);
}

TEST_CASE("Gemini Flat Panel Driver - Not connected throws", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel(0, "/dev/ttyUSB0");

    // Every property/method that needs a live link must throw NotConnected
    // (0x407) so ConformU sees the correct Alpaca error number.
    require_alpaca_error([&]() { driver->get_brightness(); }, alpacacore::AlpacaError::NotConnected);
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

TEST_CASE("Gemini Flat Panel Driver - Create by index for auto-detect", "[gemini][flatpanel][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_by_index(1, 0);

    REQUIRE(driver->get_device_number() == 1);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_unique_id() == "GEMINI_FLATPANEL_1");
}

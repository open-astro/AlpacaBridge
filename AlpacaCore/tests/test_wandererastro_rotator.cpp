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
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <functional>

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

TEST_CASE("WandererAstro Rotator Driver - Defaults", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Rotator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "WandererAstro WandererRotator Mini");
    CHECK(driver->get_can_reverse() == true);
    // One full step of the 1142 steps/degree worm drive; static, no hardware.
    CHECK(driver->get_step_size() == 1.0 / alpacacore::vendor::wandererastro::kRotatorMiniStepsPerDegree);
}

TEST_CASE("WandererAstro Rotator Driver - Device metadata", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "WandererAstro WandererRotator Mini Rotator Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore WandererAstro Rotator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "WANDERERASTRO_ROTATOR_3");
    // No firmware known before a connection handshake.
    CHECK(!driver->get_device_firmware().has_value());
}

TEST_CASE("WandererAstro Rotator Driver - Not connected throws", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(1, "/dev/null");

    REQUIRE(driver->get_connected() == false);
    require_alpaca_error([&]() { driver->get_reverse(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_mechanical_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_target_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_target_position(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_absolute(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_mechanical(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->sync(10.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro Rotator Driver - Unsupported actions", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("WandererAstro Rotator Driver - Device Number Assignment", "[wandererastro][rotator][unit]") {
    auto driver0 = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);
    auto driver1 = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(1, 0);
    auto driver5 = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}

TEST_CASE("WandererAstro Rotator Driver - State machine", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);

    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, leaving just the TimeStamp; there must be no
    // non-compliant "Connected" entry.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "IsMoving");  // omitted, not fabricated, when unknown
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);
}

TEST_CASE("WandererAstro Rotator Protocol Wrapper - Value range validation", "[wandererastro][rotator][unit]") {
    alpacacore::vendor::wandererastro::WandererRotatorProtocolWrapper wrapper;

    // Backlash range is validated BEFORE the connection check so the [0, 3]
    // boundary is testable without hardware (equivalent when connected).
    require_alpaca_error([&]() { wrapper.set_backlash(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_backlash(3.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("WandererAstro Rotator Protocol Wrapper - Disconnected behavior", "[wandererastro][rotator][unit]") {
    alpacacore::vendor::wandererastro::WandererRotatorProtocolWrapper wrapper;

    REQUIRE(wrapper.is_connected() == false);
    CHECK(!wrapper.get_firmware_date().has_value());

    const auto state = wrapper.get_state();
    CHECK(state.valid == false);
    CHECK(state.moving == false);

    require_alpaca_error([&]() { wrapper.move_relative(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_backlash(1.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_zero(); }, alpacacore::AlpacaError::NotConnected);

    // Connecting to a missing port must fail with NotConnected and leave the
    // wrapper disconnected. (/dev/null is not used here: it opens fine but is
    // not a tty, so serial configuration fails with DriverException instead.)
    alpacacore::vendor::wandererastro::RotatorConnectionConfig config;
    config.serial_port = "/nonexistent/wanderer-rotator-test-port";
    config.serial_timeout_s = 1;
    require_alpaca_error([&]() { wrapper.connect(config); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(wrapper.is_connected() == false);
}

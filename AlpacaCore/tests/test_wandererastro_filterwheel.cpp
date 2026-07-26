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
    // is omitted, leaving just the TimeStamp; there must be no non-compliant
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
    REQUIRE(has_timestamp);
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
    // wrapper disconnected. (/dev/null is not used here: it opens fine but is
    // not a tty, so serial configuration fails with DriverException instead.)
    alpacacore::vendor::wandererastro::FilterWheelConnectionConfig config;
    config.serial_port = "/nonexistent/wanderer-filterwheel-test-port";
    config.serial_timeout_s = 1;
    require_alpaca_error([&]() { wrapper.connect(config); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(wrapper.is_connected() == false);
}

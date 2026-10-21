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
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <variant>

#include "catch2_compat.h"
#include "fake_touptek_sdk.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

void wait_past_status_ttl() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

std::unique_ptr<alpacacore::FocuserDriver> make_connected(alpacacore::test::FakeToupTekSDK& fake) {
    alpacacore::test::FakeToupTekSDK::ToupFocuserInfo focuser;
    focuser.id = "fake-aaf-0";
    focuser.name = "FakeAAF";
    focuser.model_name = "AAF";
    fake.focusers.push_back(focuser);
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", fake);
    driver->set_connected(true);
    return driver;
}

} // namespace

TEST_CASE("ToupTek AAF Focuser Driver - Defaults", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ToupTek AAF");
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    CHECK(driver->get_absolute() == true);
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - Device metadata", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ToupTek AAF Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ToupTek AAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "TOUPTEK_AAF_3");
}

TEST_CASE("ToupTek AAF Focuser Driver - Not connected throws", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    REQUIRE(driver->get_connected() == false);

    require_alpaca_error([&]() { driver->get_is_moving(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - Unsupported actions", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);

    require_alpaca_error([&]() { driver->action("noop", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ToupTek AAF Focuser Driver - Absolute focuser semantics",
          "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(2, 0);

    // Absolute focuser, no temperature compensation, step size unsupported.
    // Disconnected, the two TempComp properties refuse rather than answer
    // (open-astro#309).
    CHECK(driver->get_absolute() == true);
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - Value range validation",
          "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    // Move() validates the connection first. The contract is: when not
    // connected, NotConnected is reported; when connected, out-of-range
    // positions yield InvalidValue. Without hardware we exercise the
    // disconnected path here and rely on ConformU for the connected path.
    require_alpaca_error([&]() { driver->move(-1); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(100000000); },
                         alpacacore::AlpacaError::NotConnected);

    // Set temp comp is unsupported, but the connection check answers first
    // while disconnected (open-astro#309). The NotImplemented answer on a
    // connected focuser is unchanged.
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - State machine", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; the old non-compliant "Connected"
    // entry is gone.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);

    // Disconnected reads return NotConnected, never a generic driver error.
    require_alpaca_error([&]() { driver->get_is_moving(); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - unsupported methods refuse while disconnected", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);

    // Step size is not exposed because the AAF firmware does not report a
    // mechanically-valid microns-per-step value for arbitrary focuser setups.
    // Disconnected, the connection check answers first (open-astro#309): the
    // driver does not report on a capability of hardware it has not opened.
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);

    // Temperature compensation is not implemented (no AAF action exists for
    // it) and must report NotImplemented rather than DriverException on a
    // CONNECTED focuser. Disconnected, as here, the connection check answers
    // first (open-astro#309).
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ToupTek AAF Focuser Driver - Device number assignment",
          "[touptek][focuser][unit]") {
    auto driver0 = alpacacore::vendor::touptek::create_touptek_focuser_by_index(0, 0);
    auto driver1 = alpacacore::vendor::touptek::create_touptek_focuser_by_index(1, 0);
    auto driver5 = alpacacore::vendor::touptek::create_touptek_focuser_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}

TEST_CASE("ToupTek AAF Focuser Driver - Create by id", "[touptek][focuser][unit]") {
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(7,
                                                                              "tp-aaf-test-id");

    CHECK(driver->get_device_number() == 7);
    CHECK(driver->get_unique_id() == "TOUPTEK_AAF_tp-aaf-test-id");
    CHECK(driver->get_connected() == false);
}

// open-astro#309 follow-up: the connection check now answers first while
// disconnected, so the not-implemented answers these methods give a CONNECTED
// focuser are no longer reachable from the disconnected cases above. Without
// this, deleting either throw would leave the whole suite green -- which is
// exactly what a review of that PR caught.
TEST_CASE("ToupTek AAF Focuser Driver - connected, StepSize and TempComp are not implemented",
          "[touptek][focuser][unit][fake]") {
    alpacacore::test::FakeToupTekSDK fake;
    alpacacore::test::FakeToupTekSDK::ToupFocuserInfo focuser;
    focuser.id = "fake-aaf-0";
    focuser.name = "FakeAAF";
    focuser.model_name = "AAF";
    fake.focusers.push_back(focuser);

    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // .github/instructions/touptek.instructions.md pins the AAF rule: NotImplemented, not DriverException.
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotImplemented);

    // The two getters answer honestly once there is a connection to answer about.
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);

    driver->set_connected(false);
}

// open-astro#294: DeviceState and each getter used to call aaf_get once per property.
// Falsified by: touptek_focuser_driver.cpp read_status() calling the SDK on every
// read instead of through status_cache_.get() (aaf_get delta 6, not 3).
TEST_CASE("ToupTek AAF Focuser Driver - DeviceState is one status read, served from the TTL cache",
          "[touptek][focuser][unit][fake]") {
    alpacacore::test::FakeToupTekSDK fake;
    auto driver = make_connected(fake);
    const int base = fake.call_count("aaf_get");

    CHECK(driver->get_device_state().size() == 4);  // IsMoving, Position, Temperature, TimeStamp
    CHECK(fake.call_count("aaf_get") - base == 3);  // one refill: IsMoving, GetPosition, GetTemp

    (void)driver->get_device_state();
    (void)driver->get_position();
    (void)driver->get_is_moving();
    (void)driver->get_temperature();
    CHECK(fake.call_count("aaf_get") - base == 3);  // inside the TTL: no device I/O

    wait_past_status_ttl();
    (void)driver->get_device_state();
    CHECK(fake.call_count("aaf_get") - base == 6);
}

// Falsified by: touptek_focuser_driver.cpp get_max_step() calling sdk_.aaf_get()
// instead of returning max_step_current_ (aaf_get delta 3 after the reads, not 0).
TEST_CASE("ToupTek AAF Focuser Driver - Static members cost no device read", "[touptek][focuser][unit][fake]") {
    alpacacore::test::FakeToupTekSDK fake;
    auto driver = make_connected(fake);
    const int base = fake.call_count("aaf_get");
    for (int i = 0; i < 3; ++i) {
        (void)driver->get_max_step();
        (void)driver->get_max_increment();
        (void)driver->get_name();
    }
    CHECK(fake.call_count("aaf_get") == base);
}

// Falsified by: touptek_focuser_driver.cpp move() dropping its
// status_cache_.invalidate() (Position keeps the pre-move value for a TTL).
TEST_CASE("ToupTek AAF Focuser Driver - A move and a halt drop the cached frame", "[touptek][focuser][unit][fake]") {
    alpacacore::test::FakeToupTekSDK fake;
    auto driver = make_connected(fake);
    CHECK(driver->get_position() == 0);
    driver->move(900);
    CHECK(driver->get_position() == 900);
    const int before = fake.call_count("aaf_get");
    driver->halt();
    (void)driver->get_position();
    CHECK(fake.call_count("aaf_get") - before == 3);  // halt() forced a refill inside the TTL
}

// Falsified by: ttl_status_cache.h get() dropping its health_.faulted() throw
// (the third failure reports the raw SDK error, not "communications compromised").
TEST_CASE("ToupTek AAF Focuser Driver - A dead link refuses reads instead of serving the cache",
          "[touptek][focuser][unit][fake]") {
    alpacacore::test::FakeToupTekSDK fake;
    auto driver = make_connected(fake);
    (void)driver->get_position();

    fake.throw_from.insert("aaf_get");
    wait_past_status_ttl();
    for (int i = 0; i < 2; ++i) {
        CHECK_THROWS(driver->get_position());
    }
    try {
        (void)driver->get_position();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_device_state().size() == 1);  // TimeStamp only

    fake.throw_from.clear();
    CHECK(driver->get_position() == 0);
}

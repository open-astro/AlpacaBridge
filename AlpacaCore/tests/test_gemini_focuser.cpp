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
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_gemini_focuser.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

} // namespace

TEST_CASE("Gemini Focuser Driver - Defaults", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Gemini Automatic Astro Focuser Pro");
}

TEST_CASE("Gemini Focuser Driver - Metadata", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_description() == "Gemini Automatic Astro Focuser Pro Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Gemini Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "GEMINI_FOCUSER_0");
}

TEST_CASE("Gemini Focuser Driver - Disconnected Behavior", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    // This focuser DOES support temperature compensation, so the connected
    // answer is true -- all the more reason the disconnected read must refuse
    // instead of reporting a capability nobody has asked the hardware about.
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, leaving just the TimeStamp; the old non-compliant
    // "Connected" entry is gone.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Gemini Focuser Driver - Connecting State", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("Gemini Focuser Driver - Device Number Assignment", "[gemini][focuser][unit]") {
    auto driver0 = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB1");
    auto driver5 = alpacacore::vendor::gemini::create_gemini_focuser(5, "/dev/ttyUSB2");

    REQUIRE(driver0->get_device_number() == 0);
    REQUIRE(driver1->get_device_number() == 1);
    REQUIRE(driver5->get_device_number() == 5);
}

TEST_CASE("Gemini Focuser Driver - Unique IDs", "[gemini][focuser][unit]") {
    auto driver0 = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "GEMINI_FOCUSER_0");
    CHECK(driver1->get_unique_id() == "GEMINI_FOCUSER_1");
}

#ifndef _WIN32

TEST_CASE("Gemini Focuser Driver - concurrent set_connected(true) is one transition (#333)",
          "[gemini][focuser][concurrency]") {
    // The focuser was the only Gemini driver without a transition mutex, so
    // set_connected() was a check-then-act on a plain atomic. Two HTTP workers
    // could both observe connected_ == false and both reach
    // protocol_.connect(), which assigns serial_fd_ with no prior close, so
    // the first descriptor leaks for the life of the process. (Not an MCU
    // reset: the first fd is still open and connect_serial() clears HUPCL,
    // so a second open() on a live tty raises no DTR edge.)
    //
    // The window is the handshake ladder, up to ~9.1 s on hardware. The fake
    // holds its handshake reply so the race is reproducible rather than
    // timing-dependent.
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_handshake_delay(std::chrono::milliseconds(300));
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            try {
                driver->set_connected(true);
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    // Neither caller may fail: both asked for a state the driver is already
    // reaching. Without the transition mutex the loser reaches the wrapper,
    // whose already-connected guard throws.
    CHECK(failures.load() == 0);
    CHECK(driver->get_connected());
    // Exactly one connect reached the wire. Without either fix this is 2, and
    // the first descriptor is the one that leaks.
    CHECK(fake.connects() == 1);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini protocol wrapper - a second connect on a live wrapper throws (#333)",
          "[gemini][focuser][concurrency]") {
    // The driver's transition mutex keeps this unreachable through the
    // driver, so the guard is pinned directly: a live wrapper refuses a
    // second connect with InvalidOperation instead of leaking the first
    // descriptor.
    alpacacore::test::FakeGeminiFocuser fake;
    alpacacore::vendor::gemini::GeminiProtocolWrapper wrapper;
    alpacacore::vendor::gemini::ConnectionConfig config;
    config.serial_port = fake.slave_path();
    CHECK(wrapper.connect(config) > 0);
    CHECK(wrapper.is_connected());
    require_alpaca_error([&]() { wrapper.connect(config); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(wrapper.is_connected());
    CHECK(fake.connects() == 1);  // the refused connect never reached the wire
    wrapper.disconnect();
    CHECK_FALSE(wrapper.is_connected());
}

TEST_CASE("Gemini Focuser Driver - a connect racing a disconnect settles once (#333)",
          "[gemini][focuser][concurrency]") {
    // The same unguarded serial_fd_ a second connect overwrites is the one a
    // concurrent disconnect closes, so the two must not interleave either.
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_handshake_delay(std::chrono::milliseconds(200));
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    std::atomic<int> failures{0};
    auto flip = [&](bool target) {
        try {
            driver->set_connected(target);
        } catch (const std::exception&) {
            ++failures;
        }
    };

    for (int round = 0; round < 5; ++round) {
        std::thread up(flip, true);
        std::thread down(flip, false);
        up.join();
        down.join();
        CHECK(failures.load() == 0);
        // Whichever won, the driver is in a definite state and the link
        // agrees with it: a getter must not throw NotConnected while
        // get_connected() reports true.
        if (driver->get_connected()) {
            CHECK_NOTHROW(driver->get_position());
        }
        driver->set_connected(false);
    }
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini Focuser Driver - connect/disconnect cycles reuse the port cleanly (#333)",
          "[gemini][focuser][concurrency]") {
    // The already-connected guard must not break the ordinary sequential
    // reconnect the web UI does every time a device is re-enabled.
    alpacacore::test::FakeGeminiFocuser fake;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    for (int i = 0; i < 3; ++i) {
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK_NOTHROW(driver->get_position());
        driver->set_connected(false);
        REQUIRE_FALSE(driver->get_connected());
    }
    // One handshake per connect, no more.
    CHECK(fake.connects() == 3);
}

#endif  // _WIN32

// open-astro#309 follow-up: with the connection check answering first, nothing
// else asserts this focuser's CONNECTED capability answers. Gemini is the one
// of the five that really supports temperature compensation, so
// TempCompAvailable == true is a claim worth pinning rather than losing.
TEST_CASE("Gemini Focuser Driver - connected, TempCompAvailable is true", "[gemini][focuser][unit][fake]") {
    alpacacore::test::FakeGeminiFocuser fake;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_temp_comp_available() == true);
    // The fake answers ":24#" with "10#", i.e. temp comp off.
    CHECK(driver->get_temp_comp() == false);

    driver->set_connected(false);
}

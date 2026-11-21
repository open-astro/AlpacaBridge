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

// The Gemini focuser's connect-time first poll runs inside Connect and holds the transition mutex
// (issue #2064). A port that answers the handshake and the firmware query and then goes silent must not
// push Connect past the 5 s Connecting budget or park a Disconnect behind the probe. The serial timeout is
// a real poll() on the pty, so these cases use real time.

#ifndef _WIN32

#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_gemini_focuser.h"

TEST_CASE("Gemini focuser - Connect to a port that goes silent after the handshake fails inside the 5 s budget (#2064)",
          "[gemini][focuser][unit][fake][connect_budget]") {
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_silent_state_replies(true);
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    const auto connect_ms = alpacacore::test::timed([&] {
                                alpacacore::test::require_alpaca_error([&] { driver->set_connected(true); },
                                                                       alpacacore::AlpacaError::DriverException);
                            }).count() /
                            1000;
    REQUIRE(fake.count(":00#") >= 1);
    WARN("connect_ms=" << connect_ms);
    CHECK(connect_ms <= 5000);  // Platform 7 / ConformU Connecting budget
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini focuser - Disconnect during a silent connect-time probe waits at most 3.5 s (#2064)",
          "[gemini][focuser][unit][fake][connect_budget]") {
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_silent_state_replies(true);
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    std::thread connector([&] {
        try {
            driver->set_connected(true);
        } catch (const std::exception&) {
        }
    });
    for (int i = 0; i < 400 && fake.count(":00#") == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(fake.count(":00#") >= 1);
    const auto wait_ms = alpacacore::test::timed([&] { driver->set_connected(false); }).count() / 1000;
    connector.join();
    WARN("disconnect_wait_ms=" << wait_ms);
    CHECK(wait_ms <= 3500);
}

TEST_CASE("Gemini focuser - A connect with a healthy port still publishes a first frame (#2064)",
          "[gemini][focuser][unit][fake][connect_budget]") {
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_position(1234);
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_position() == 1234);
    CHECK_NOTHROW(driver->get_temperature());
}

TEST_CASE("Gemini focuser - A connect with no temperature sensor still succeeds (#2064)",
          "[gemini][focuser][unit][fake][connect_budget]") {
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_garbage_temperature(true);
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_position() == 1000);
}

#endif

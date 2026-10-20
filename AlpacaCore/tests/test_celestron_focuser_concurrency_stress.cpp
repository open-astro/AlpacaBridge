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

// [stress] coverage for the Celestron focus motor (issue #896), the shape of
// the Gemini focuser cases, over fake_celestron_focuser.h.

#ifndef _WIN32

#include <alpacacore/focuser_driver.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_celestron_focuser.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeCelestronFocuser;

namespace {

void focuser_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
    guard([&] { static_cast<void>(focuser.get_is_moving()); });
    guard([&] { static_cast<void>(focuser.get_position()); });
    guard([&] { static_cast<void>(focuser.get_max_step()); });
    guard([&] { static_cast<void>(focuser.get_max_increment()); });
    guard([&] { focuser.move(1234); });
    guard([&] { static_cast<void>(focuser.action("CalibrationStatus", "")); });
    // Halt resynchronizes before its stop; it must never reopen the port a
    // racing disconnect closed (issue #896).
    guard([&] { focuser.halt(); });
}

}  // namespace

TEST_CASE("Celestron focuser - concurrent connect/disconnect/operate stress", "[celestron][focuser][stress]") {
    FakeCelestronFocuser fake;
    auto driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());

    // Prove the fake connects before storming it.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK_NOTHROW(driver->set_connected(false));

    // A connected focus motor's only legitimate throw here is NotConnected
    // from a racing disconnect (ensure_connected(), or the transport's own
    // check under its lock in exchange()/resync()). move() clamps and the fake
    // answers every frame, so a DriverException would be a real regression.
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { focuser_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Celestron focuser - destruction races an in-flight connect", "[celestron][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<FakeCelestronFocuser>(); },
        [](FakeCelestronFocuser& fake) {
            return alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());
        },
        25);
}

TEST_CASE("Celestron focuser - racing disconnect is never dropped", "[celestron][focuser][stress]") {
    FakeCelestronFocuser fake;
    auto driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake.slave_path());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

#endif  // _WIN32

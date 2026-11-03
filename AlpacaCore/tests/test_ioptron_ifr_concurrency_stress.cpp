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

// Connect/disconnect/operate concurrency stress for the iOptron iFR focuser and rotator, both CONNECTED over
// FakeIoptronIfr (a pty fake answering the iFR command set), so the storm reaches the post-connect paths and
// the shared serial session (IfrLink). The case that only this unit has: one device's connect/disconnect storm
// while the other device keeps using the same session, which is where a session closed under a live holder, or
// an acquire that opens the port twice, would show. Run under TSan (sanitizers-tsan, RUN_TSAN=1 pre-flight).
// The explicit-port factories: the by-index ones scan real /dev/ttyUSB* adapters at connect and never find a pty.

#ifndef _WIN32

#include <alpacacore/focuser_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_rotator_driver.h>

#include <initializer_list>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_ioptron_ifr.h"

using alpacacore::AlpacaDriver;

namespace {

void focuser_operate(alpacacore::test::StressCallGuard& guard, alpacacore::FocuserDriver& focuser) {
    guard([&] { static_cast<void>(focuser.get_position()); });
    guard([&] { static_cast<void>(focuser.get_is_moving()); });
    guard([&] { static_cast<void>(focuser.get_temperature()); });
    guard([&] { static_cast<void>(focuser.get_device_state()); });
    guard([&] { focuser.move(2000); });
    guard([&] { focuser.halt(); });
}

void rotator_operate(alpacacore::test::StressCallGuard& guard, alpacacore::RotatorDriver& rotator) {
    guard([&] { static_cast<void>(rotator.get_position()); });
    guard([&] { static_cast<void>(rotator.get_mechanical_position()); });
    guard([&] { static_cast<void>(rotator.get_is_moving()); });
    guard([&] { static_cast<void>(rotator.get_reverse()); });
    guard([&] { static_cast<void>(rotator.get_device_state()); });
    guard([&] { rotator.move_absolute(40.0); });
    guard([&] { rotator.halt(); });
    guard([&] { rotator.sync(40.0); });
}

}  // namespace

TEST_CASE("iFR focuser - concurrent connect/disconnect/operate stress", "[ioptron][ifr][focuser][stress]") {
    alpacacore::test::FakeIoptronIfr fake;
    auto driver = alpacacore::vendor::ioptron::create_ifr_focuser(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(fake.handshakes() >= 1);

    // A refused :FM (the fake refuses a move to the current position only when it is the target, which the
    // driver answers as success) is never an error here, so only a racing disconnect is expected.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(
        *driver, [&guard](AlpacaDriver& d) { focuser_operate(guard, static_cast<alpacacore::FocuserDriver&>(d)); });
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iFR rotator - concurrent connect/disconnect/operate stress", "[ioptron][ifr][rotator][stress]") {
    alpacacore::test::FakeIoptronIfr fake;
    auto driver = alpacacore::vendor::ioptron::create_ifr_rotator(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(
        *driver, [&guard](AlpacaDriver& d) { rotator_operate(guard, static_cast<alpacacore::RotatorDriver&>(d)); });
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iFR - the focuser's lifecycle storm while the rotator uses the same session",
          "[ioptron][ifr][focuser][stress]") {
    alpacacore::test::FakeIoptronIfr fake;
    auto focuser = alpacacore::vendor::ioptron::create_ifr_focuser(0, fake.slave_path());
    auto rotator = alpacacore::vendor::ioptron::create_ifr_rotator(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*rotator, true));
    REQUIRE(alpacacore::test::settle_connected(*focuser, true));

    // The rotator never disconnects here: every rotator call must succeed while the focuser comes and goes on
    // the session it shares, so NotConnected from the rotator would be a dropped holder and is counted.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::StressCallGuard rotator_guard{std::initializer_list<int>{}};
    alpacacore::test::run_lifecycle_stress(*focuser, [&](AlpacaDriver& d) {
        focuser_operate(guard, static_cast<alpacacore::FocuserDriver&>(d));
        rotator_guard([&] { static_cast<void>(rotator->get_position()); });
        rotator_guard([&] { static_cast<void>(rotator->get_is_moving()); });
    });
    CHECK(alpacacore::test::settle_connected(*focuser, false));
    CHECK(rotator->get_connected());
    CHECK(alpacacore::test::settle_connected(*rotator, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    INFO(rotator_guard.report());
    CHECK(rotator_guard.unexpected_count() == 0);
    CHECK(rotator_guard.total_calls() > 0);
}

TEST_CASE("iFR - the rotator's lifecycle storm while the focuser uses the same session",
          "[ioptron][ifr][rotator][stress]") {
    alpacacore::test::FakeIoptronIfr fake;
    auto focuser = alpacacore::vendor::ioptron::create_ifr_focuser(0, fake.slave_path());
    auto rotator = alpacacore::vendor::ioptron::create_ifr_rotator(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*focuser, true));
    REQUIRE(alpacacore::test::settle_connected(*rotator, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::StressCallGuard focuser_guard{std::initializer_list<int>{}};
    alpacacore::test::run_lifecycle_stress(*rotator, [&](AlpacaDriver& d) {
        rotator_operate(guard, static_cast<alpacacore::RotatorDriver&>(d));
        focuser_guard([&] { static_cast<void>(focuser->get_position()); });
        focuser_guard([&] { static_cast<void>(focuser->get_temperature()); });
    });
    CHECK(alpacacore::test::settle_connected(*rotator, false));
    CHECK(focuser->get_connected());
    CHECK(alpacacore::test::settle_connected(*focuser, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    INFO(focuser_guard.report());
    CHECK(focuser_guard.unexpected_count() == 0);
    CHECK(focuser_guard.total_calls() > 0);
}

TEST_CASE("iFR focuser - a racing disconnect is never dropped", "[ioptron][ifr][focuser][stress]") {
    alpacacore::test::FakeIoptronIfr fake;
    auto driver = alpacacore::vendor::ioptron::create_ifr_focuser(0, fake.slave_path());
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
}

TEST_CASE("iFR rotator - a racing disconnect is never dropped", "[ioptron][ifr][rotator][stress]") {
    alpacacore::test::FakeIoptronIfr fake;
    auto driver = alpacacore::vendor::ioptron::create_ifr_rotator(0, fake.slave_path());
    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
}

TEST_CASE("iFR focuser - destruction races an in-flight connect", "[ioptron][ifr][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<alpacacore::test::FakeIoptronIfr>(); },
        [](alpacacore::test::FakeIoptronIfr& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::ioptron::create_ifr_focuser(0, fake.slave_path());
        },
        25);
}

TEST_CASE("iFR rotator - destruction races an in-flight connect", "[ioptron][ifr][rotator][stress]") {
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<alpacacore::test::FakeIoptronIfr>(); },
        [](alpacacore::test::FakeIoptronIfr& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::ioptron::create_ifr_rotator(0, fake.slave_path());
        },
        25);
}

#endif  // !_WIN32

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

// [stress] registration for the standalone QHYCFW3 (USB) filter wheel. The
// FakeQhyCfw3 pty gives the storm a real connect path: every iteration runs
// the VRS/MXP/NOW handshake through the real protocol wrapper, and the
// operate callback's goto spawns the arrival-wait worker that disconnect must
// cancel and join.
//
// This registers the (qhy, filterwheel) pair. The integrated CFW driver
// (qhy_filterwheel_driver.cpp, over the camera handle) shares that pair and
// is therefore masked by this registration; it is covered by its fake-SDK
// unit cases and code review only. See check_stress_registration.py.

#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_qhy_cfw3.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeQhyCfw3;

namespace {

alpacacore::vendor::qhy::Cfw3Settings stress_settings() {
    alpacacore::vendor::qhy::Cfw3Settings s;
    s.boot_timeout_ms = 50;  // a pty never resets the fake; do not sit out the real 25 s
    s.reply_timeout_ms = 500;
    s.move_timeout_ms = 2000;
    return s;
}

void wheel_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
    guard([&] { static_cast<void>(wheel.get_position()); });
    guard([&] { static_cast<void>(wheel.get_names()); });
    guard([&] { static_cast<void>(wheel.get_focus_offsets()); });
    guard([&] { wheel.set_position(3); });
    guard([&] { static_cast<void>(wheel.get_position()); });
    guard([&] { wheel.set_position(0); });
    guard([&] { static_cast<void>(wheel.get_device_state()); });
    guard([&] { static_cast<void>(wheel.get_device_firmware()); });
}

}  // namespace

TEST_CASE("QHY CFW3 - concurrent connect/disconnect/operate stress", "[qhy][filterwheel][stress]") {
    FakeQhyCfw3 fake(7);
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), stress_settings());

    // Prove the fake connects before storming it, so the case cannot
    // silently degrade into a fail-fast run that never reaches the wire.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    CHECK_NOTHROW(driver->set_connected(false));

    // Connected registration, so the expected set is explicit: NotConnected
    // from losing the race with a disconnect (set_position re-checks it under
    // its locks, so a goto that lands during a teardown throws rather than
    // spawning a worker), and InvalidOperation from a goto landing while the
    // previous one is still in flight (set_position's one-goto-at-a-time
    // rule). DriverException is deliberately NOT expected: it is what the
    // driver throws for genuine internal failures, and the storm must keep
    // seeing those.
    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidOperation});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { wheel_operate(guard, d); });

    // A connect must still succeed once the storm ends. Every in-storm failure
    // is swallowed by design, so a change that made every connect fail would
    // otherwise leave this case green while it only exercised fail-fast paths.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("QHY CFW3 - destruction races an in-flight connect", "[qhy][filterwheel][stress]") {
    const auto settings = stress_settings();
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<FakeQhyCfw3>(7); },
        [&settings](FakeQhyCfw3& fake) {
            return alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), settings);
        },
        25);
}

TEST_CASE("QHY CFW3 - racing disconnect is never dropped", "[qhy][filterwheel][stress]") {
    FakeQhyCfw3 fake(7);
    auto driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), stress_settings());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

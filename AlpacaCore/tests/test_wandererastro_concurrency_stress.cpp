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

// Connect/disconnect/operate concurrency stress for the four WandererAstro
// drivers (issue #101).
//
// Three of them stream a status frame unprompted, so the existing
// fake_serial_streamer.h pty fake drives them into the CONNECTED state and
// the storm reaches the surface that matters: each driver owns a reader
// thread that fills a status cache the Alpaca getters read, and a disconnect
// has to reap it. The frames are the same constants the issue-#237
// link-health unit tests already use.
//
// The rotator is the exception: it speaks request/response ("1500001" ->
// five 'A'-terminated fields), not an unprompted stream, so the streamer
// fake cannot drive its handshake. It takes the accepted fail-fast bar
// against a serial path that cannot exist -- never a plausible /dev/ttyUSB*,
// because this storm connects hundreds of times and would otherwise send the
// handshake to whatever is actually plugged into a dev box.

#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_driver.h>

#include <chrono>
#include <string>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_serial_streamer.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeSerialStreamer;

namespace {

// Same wire frames the issue-#237 link-health tests use.
const char* const kCoverFrame = "WandererCoverV4ProA20240301A10.0A270.0A10.0A12.5A0A0A0\n";
const char* const kSfwFrame = "WSFW368A20260124A3ABCDEFGHIA0A0A0A0A0A0A0A0A1A\n";
const char* const kBoxFrame =
    "ZXWBProV3A20250410A-127.00A-127.00A-127.00A45.20A21.30A1.50A0.20A0.30A13.10A1A1A1A1A1A1A0A0A0A1A1A120A\n";

// Cannot exist: the rotator's connect writes a handshake, so a plausible
// port would put "1500001" on whatever device is really attached.
constexpr const char* kAbsentSerialPort = "/dev/wanderer-alpacabridge-absent";

// open-astro#326: the operate callbacks take the storm's StressCallGuard by
// reference instead of the file-scope call() template they used to share. A
// racing disconnect makes any operate call throw NotConnected (or
// DriverException once the link-health latch trips), and run_lifecycle_stress
// only swallows the callback as a whole -- so per-call guarding is what keeps
// one throw from skipping every call below it for that iteration. The guard
// also COUNTS what it swallows, so an unexpected throw now fails the case.
// The three full-seam storms therefore widen the expected set to
// {NotConnected, DriverException}: the link-fault latch needs ~6-10 s of
// silence, far longer than a storm, so DriverException is not expected in
// practice, but it is the code these calls legitimately throw once the latch
// trips, and a set that counts it as a regression would contradict this
// comment (review note on PR #468).

void cover_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& panel = static_cast<alpacacore::CoverCalibratorDriver&>(d);
    guard([&] { static_cast<void>(panel.get_cover_state()); });
    guard([&] { static_cast<void>(panel.get_calibrator_state()); });
    guard([&] { static_cast<void>(panel.get_brightness()); });
    guard([&] { static_cast<void>(panel.get_max_brightness()); });
    guard([&] { panel.calibrator_on(100); });
    guard([&] { panel.calibrator_off(); });
    guard([&] { panel.open_cover(); });
    guard([&] { panel.halt_cover(); });
    guard([&] { static_cast<void>(panel.get_device_state()); });
}

void filterwheel_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
    guard([&] { static_cast<void>(wheel.get_position()); });
    guard([&] { wheel.set_position(1); });
    guard([&] { static_cast<void>(wheel.get_names()); });
    guard([&] { static_cast<void>(wheel.get_focus_offsets()); });
    guard([&] { static_cast<void>(wheel.get_device_state()); });
}

void box_switch_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
    guard([&] { static_cast<void>(sw.get_max_switch()); });
    guard([&] { static_cast<void>(sw.get_switch_value(0)); });
    guard([&] { static_cast<void>(sw.get_switch_name(0)); });
    guard([&] { static_cast<void>(sw.get_can_write(0)); });
    // Write to id 2 ("DC3-4"), not id 0: ids 0/1 are the always-on
    // read-only rails, so a write there throws NotImplemented before
    // reaching dispatch_write() and the state_mutex_-guarded commanded-value
    // path this storm is supposed to exercise would never run.
    guard([&] { sw.set_switch_value(2, 1.0); });
    guard([&] { static_cast<void>(sw.get_device_state()); });
}

void rotator_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& rotator = static_cast<alpacacore::RotatorDriver&>(d);
    guard([&] { static_cast<void>(rotator.get_position()); });
    guard([&] { static_cast<void>(rotator.get_mechanical_position()); });
    guard([&] { static_cast<void>(rotator.get_is_moving()); });
    guard([&] { static_cast<void>(rotator.get_target_position()); });
    guard([&] { rotator.move(1.0); });
    guard([&] { rotator.move_absolute(10.0); });
    guard([&] { rotator.halt(); });
    guard([&] { static_cast<void>(rotator.get_device_state()); });
}

}  // namespace

TEST_CASE("WandererAstro cover calibrator - concurrent connect/disconnect/operate stress",
          "[wandererastro][covercalibrator][stress]") {
    FakeSerialStreamer cover(kCoverFrame, std::chrono::milliseconds(50));
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, cover.slave_path());

    // Proves the fake really connects, so this can't silently degrade into a
    // fail-fast test that never reaches the reader thread.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_connected(false);

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { cover_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("WandererAstro cover calibrator - destruction races an in-flight connect",
          "[wandererastro][covercalibrator][stress]") {
    // 50 ms, not the unit tests' 300/500 ms: each destruction-race iteration
    // pays a full successful connect on the fake, i.e. waits out one frame
    // interval, and the harness default of 100 iterations at 300 ms would add
    // ~30 s to sanitizers-tsan before the TSan slowdown, eating into the
    // 30-minute timeout-minutes bound the whole [stress] suite shares (there
    // is no step-level bound under it). The frame CONTENT is what the driver
    // keys on, not the cadence, so a faster interval deepens the storm for
    // free and makes the harness default affordable.
    FakeSerialStreamer cover(kCoverFrame, std::chrono::milliseconds(50));
    const std::string port = cover.slave_path();
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, port); });
}

TEST_CASE("WandererAstro filter wheel - concurrent connect/disconnect/operate stress",
          "[wandererastro][filterwheel][stress]") {
    FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(50));
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, wheel.slave_path());

    // 15 s, not the cover's 5 s: settle_connected() gates whether to START
    // a new attempt on the deadline, not how long an in-flight one may run,
    // so it isn't strictly "one attempt" -- but the wheel's own connect wait
    // (serial_timeout_s * 1000 = 5000 ms exactly) leaves zero slack inside a
    // 5 s budget: if the very first attempt is the one that's slow, there is
    // no time left to even start a second. The cover's connect wait is only
    // 3000 ms against the same 5 s budget (2 s of real margin), which is why
    // it did not need raising -- this is a budget-vs-connect-time margin
    // difference between the two drivers, not an inconsistency.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    driver->set_connected(false);

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { filterwheel_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("WandererAstro filter wheel - destruction races an in-flight connect",
          "[wandererastro][filterwheel][stress]") {
    // 50 ms frame interval, same reason as the cover calibrator above.
    FakeSerialStreamer wheel(kSfwFrame, std::chrono::milliseconds(50));
    const std::string port = wheel.slave_path();
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, port); });
}

TEST_CASE("WandererAstro box switch - concurrent connect/disconnect/operate stress",
          "[wandererastro][switch][stress]") {
    FakeSerialStreamer box(kBoxFrame, std::chrono::milliseconds(50));
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, box.slave_path());

    // 15 s, not the cover's 5 s: same margin argument as the filter wheel
    // above, sharper here -- the box's connect wait is
    // serial_timeout_s * 1000 + 3500 = 6500 ms, which already EXCEEDS a 5 s
    // budget on its own, so the first attempt alone could exhaust it.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    driver->set_connected(false);

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { box_switch_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("WandererAstro box switch - destruction races an in-flight connect", "[wandererastro][switch][stress]") {
    // 50 ms frame interval, same reason as the cover calibrator above --
    // sharper here, since the unit tests' 500 ms would have added ~50 s at
    // the harness default.
    FakeSerialStreamer box(kBoxFrame, std::chrono::milliseconds(50));
    const std::string port = box.slave_path();
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, port); });
}

TEST_CASE("WandererAstro rotator - concurrent connect/disconnect/operate stress", "[wandererastro][rotator][stress]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, kAbsentSerialPort);

    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { rotator_operate(guard, d); });

    // The port cannot exist, so no connect in the storm can have succeeded.
    CHECK(driver->get_connected() == false);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("WandererAstro rotator - destruction races an in-flight connect", "[wandererastro][rotator][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, kAbsentSerialPort); });
}

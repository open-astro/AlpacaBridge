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

// ZWO camera waits on the TaskClock (issue #1280, decision 0005): every wait
// and deadline in the driver reads the injected clock, so these cases drive
// the driver through FakeZWOSDK with FakeTaskClock::advance() and never sleep.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_task_clock.h"
#include "fake_zwo_sdk.h"
#include "zwo_exposure_wait.h"

namespace {

using alpacacore::AlpacaException;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZWOSDK;
using alpacacore::vendor::zwo::create_zwo_camera;
namespace AlpacaError = alpacacore::AlpacaError;
using std::chrono::milliseconds;
using Direction = FakeZWOSDK::GuideDirection;

constexpr auto kRendezvous = std::chrono::seconds(5);

int error_code(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    }
    return -1;
}

}  // namespace

TEST_CASE("ZWO camera clock - first CCDTemperature retry sleeps on the task clock", "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    sdk.zero_temperature_reads_after_open = 1000;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    const int connect_reads = sdk.call_count("get_control_value");

    double reading = -1.0;
    std::thread reader([&] { reading = camera->get_ccd_temperature(); });
    // Six reads: the first, then five retries, each behind a 50 ms clock sleep.
    for (int retry = 1; retry <= 5; ++retry) {
        REQUIRE(clock.wait_for_wait_count(static_cast<std::uint64_t>(retry), kRendezvous));
        CHECK(sdk.call_count("get_control_value") == connect_reads + retry);
        clock.advance(milliseconds(50));
    }
    reader.join();
    CHECK(reading == Catch::Approx(0.0));
    CHECK(sdk.call_count("get_control_value") == connect_reads + 6);
}

TEST_CASE("ZWO camera clock - IsPulseGuiding ends at the task clock deadline", "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    // A deadline taken from the real clock would be far ahead of this virtual time.
    clock.advance(std::chrono::seconds(10));

    camera->pulse_guide(0, 300);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    CHECK(camera->get_is_pulse_guiding());
    clock.advance(milliseconds(299));
    CHECK(camera->get_is_pulse_guiding());
    clock.advance(milliseconds(1));
    CHECK_FALSE(camera->get_is_pulse_guiding());
    REQUIRE(clock.wait_for_woken_settled(kRendezvous));
}

TEST_CASE("ZWO camera clock - pulse guide turns off after the duration on the task clock",
          "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);

    camera->pulse_guide(2, 300);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    CHECK(sdk.call_count("pulse_guide_on") == 1);
    clock.advance(milliseconds(299));
    CHECK(sdk.call_count("pulse_guide_off") == 0);
    clock.advance(milliseconds(1));
    REQUIRE(sdk.wait_for_call_count("pulse_guide_off", 1, kRendezvous));
    REQUIRE(clock.wait_for_woken_settled(kRendezvous));
}

// Falsified by: zwo_camera_driver.cpp set_connected(false) not cancelling and joining pulse_slot_ before the close
// (the guide stays on until the timer runs out and the timer outlives the Disconnect). Decision 0006 rule 1 and
// async_operation.h Rule 8.
TEST_CASE("ZWO camera clock - Disconnect mid-pulse turns the guide off at once", "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    camera->pulse_guide(0, 1000);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));

    camera->set_connected(false);

    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::North});
    CHECK_FALSE(camera->get_is_pulse_guiding());
    CHECK(clock.waiter_count() == 0);  // no timer outlives the Disconnect
}

// Falsified by: zwo_camera_driver.cpp run_pulse_timer() returning on a cancel without sending the pending turn-offs,
// or a detached turn-off thread (it outlives the driver and the guide stays on).
TEST_CASE("ZWO camera clock - destroying the driver mid-pulse turns the guide off and leaves no timer",
          "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    camera->pulse_guide(2, 1000);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));

    camera.reset();

    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::East});
    CHECK(clock.waiter_count() == 0);
}

// Falsified by: zwo_camera_driver.cpp set_connected(true) not joining pulse_slot_ before the open while note_loss()
// does not cancel it (the timer from before the replug survives the reconnect, free to turn off the new handle).
TEST_CASE("ZWO camera clock - a pulse from before a replug never turns off the guide on the new handle",
          "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    camera->pulse_guide(0, 1000);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    sdk.removed = true;
    CHECK(error_code([&] { (void)camera->get_ccd_temperature(); }) == AlpacaError::NotConnected);
    REQUIRE_FALSE(camera->get_connected());

    sdk.replug({{sdk.camera.camera_id, sdk.serial}});
    camera->set_connected(true);

    CHECK(clock.waiter_count() == 0);  // the old timer did not survive the reconnect
    clock.advance(milliseconds(2000));
    CHECK(sdk.guide_offs().empty());
    camera->set_connected(false);
}

// Falsified by: zwo_camera_driver.cpp pulse_guide() dropping the running pulse's pending turn-off when a new pulse
// supersedes the timer body (the North output would stay on for ever), or end_pulses() acting for a superseded body
// (it ends both pulses at once). Decision 0006 rules 3 and 6. IsPulseGuiding stays true until North ends: falsified
// by pulse_guide() taking the newer pulse's deadline as the end time, or end_pulses() clearing the flag while a
// turn-off is still pending.
TEST_CASE("ZWO camera clock - a pulse on another axis does not cut the running pulse short",
          "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    camera->pulse_guide(0, 1000);
    REQUIRE(clock.wait_for_wait_count(1, kRendezvous));
    camera->pulse_guide(2, 200);
    REQUIRE(clock.wait_for_wait_count(2, kRendezvous));
    CHECK(camera->get_is_pulse_guiding());

    clock.advance(milliseconds(200));
    REQUIRE(sdk.wait_for_call_count("pulse_guide_off", 1, kRendezvous));
    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::East});
    // A wait's deadline is fixed when it registers: advance only once the body waits again, for the North deadline.
    REQUIRE(clock.wait_for_wait_count(3, kRendezvous));
    CHECK(camera->get_is_pulse_guiding());  // North still runs
    clock.advance(milliseconds(799));
    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::East});
    CHECK(camera->get_is_pulse_guiding());
    clock.advance(milliseconds(1));
    REQUIRE(sdk.wait_for_call_count("pulse_guide_off", 2, kRendezvous));
    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::East, Direction::North});
    CHECK_FALSE(camera->get_is_pulse_guiding());
}

// Falsified by: zwo_camera_driver.cpp end_pulses() clearing the IsPulseGuiding flag after the short pulse's turn-off
// while the longer pulse's turn-off is still pending.
TEST_CASE("ZWO camera clock - IsPulseGuiding stays true after a short pulse ends while a longer one runs",
          "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    camera->pulse_guide(2, 200);
    REQUIRE(clock.wait_for_wait_count(1, kRendezvous));
    camera->pulse_guide(0, 1000);
    REQUIRE(clock.wait_for_wait_count(2, kRendezvous));
    CHECK(camera->get_is_pulse_guiding());

    clock.advance(milliseconds(200));
    REQUIRE(sdk.wait_for_call_count("pulse_guide_off", 1, kRendezvous));
    REQUIRE(clock.wait_for_wait_count(3, kRendezvous));
    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::East});
    CHECK(camera->get_is_pulse_guiding());  // North still runs
    clock.advance(milliseconds(799));
    CHECK(camera->get_is_pulse_guiding());
    clock.advance(milliseconds(1));
    REQUIRE(sdk.wait_for_call_count("pulse_guide_off", 2, kRendezvous));
    CHECK(sdk.guide_offs() == std::vector<Direction>{Direction::East, Direction::North});
    CHECK_FALSE(camera->get_is_pulse_guiding());
}

TEST_CASE("ZWO camera clock - exposure retries are paced by the task clock", "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    sdk.frame_bytes = std::vector<std::uint8_t>(16 * 8 * 2, 1);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    clock.advance(std::chrono::seconds(1));

    // One Failed per poll: a retry's restart resets the fake's own status.
    sdk.script_exposure_status(std::vector<FakeZWOSDK::ExposureStatus>(5, FakeZWOSDK::ExposureStatus::Failed));
    camera->start_exposure(0.001, true);
    // The first poll fails and retry 1 runs at once; the next poll fails inside the 250 ms window, so the exposure
    // body waits on the clock for the rest of it.
    REQUIRE(sdk.wait_for_call_count("start_exposure", 2, kRendezvous));
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    clock.advance(milliseconds(249));
    CHECK(sdk.call_count("start_exposure") == 2);
    clock.advance(milliseconds(1));
    REQUIRE(sdk.wait_for_call_count("start_exposure", 3, kRendezvous));  // retry 2
    // The third failure has no retry left: the exposure fails at once.
    REQUIRE(alpacacore::test::wait_exposure_end(*camera));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
    CHECK(sdk.call_count("start_exposure") == 3);
}

namespace {

// Exposure 1 on a camera of the given bus, downloaded at virtual time 1 s. The fake models the
// ASI120MM (issue #1349): an exposure longer than 3 s started less than 500 ms after a download fails
// at its end.
struct SettleRig {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::CameraDriver> camera;

    explicit SettleRig(bool usb3) {
        sdk.camera.is_usb3 = usb3;
        sdk.usb2_settle_clock = &clock;
        sdk.frame_bytes = std::vector<std::uint8_t>(16 * 8 * 2, 1);
        camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
        camera->set_connected(true);
        clock.advance(std::chrono::seconds(1));
        camera->start_exposure(0.001, true);
        REQUIRE(alpacacore::test::wait_exposure_end(*camera));
        REQUIRE(camera->get_image_ready());
        static_cast<void>(camera->get_image_array());
    }
};

// Long enough to need the settle wait (over 3 s), and the 3 s edge that does not.
constexpr double kLongExposure = 10.0;
constexpr double kShortExposure = 3.0;

}  // namespace

TEST_CASE("ZWO camera clock - USB 2.0 start waits 600 ms after the previous download", "[zwo][camera][unit][clock]") {
    SettleRig rig(false);
    const auto downloaded_at = rig.clock.now();

    rig.camera->start_exposure(kLongExposure, true);  // the settle runs in the exposure body (issue #1981)
    const bool waiting = rig.clock.wait_for_waiters(1, kRendezvous);
    CHECK(waiting);
    if (waiting) {
        rig.clock.advance(milliseconds(599));
        CHECK(rig.sdk.call_count("start_exposure") == 1);  // still inside the settle window
        rig.clock.advance(milliseconds(1));
    }
    REQUIRE(alpacacore::test::wait_exposure_end(*rig.camera));

    REQUIRE(rig.sdk.call_count("start_exposure") == 2);
    const auto starts = rig.sdk.start_times();
    CHECK(starts[1] - downloaded_at >= milliseconds(600));
    CHECK(rig.camera->get_image_ready());  // no ASI_EXP_FAILED, so no retry
    CHECK(rig.sdk.call_count("start_exposure") == 2);
}

namespace {

// Starts exposure 2 and reports whether its body parked on the clock before the trigger. A body that wrongly waits
// is released after the check, so a broken driver fails the case instead of hanging it.
bool start_parks_on_clock(SettleRig& rig, double duration = kLongExposure) {
    rig.camera->start_exposure(duration, true);
    const bool waited = rig.clock.wait_for_waiters(1, milliseconds(200));
    if (waited) rig.clock.advance(milliseconds(600));
    REQUIRE(alpacacore::test::wait_exposure_end(*rig.camera));
    return waited;
}

}  // namespace

TEST_CASE("ZWO camera clock - USB 3.0 start does not wait after a download", "[zwo][camera][unit][clock]") {
    SettleRig rig(true);

    CHECK_FALSE(start_parks_on_clock(rig));

    CHECK(rig.sdk.call_count("start_exposure") == 2);
    CHECK(rig.camera->get_image_ready());
    CHECK(rig.sdk.call_count("start_exposure") == 2);
}

TEST_CASE("ZWO camera clock - USB 2.0 start after the settle time has passed does not wait",
          "[zwo][camera][unit][clock]") {
    SettleRig rig(false);
    rig.clock.advance(milliseconds(600));

    CHECK_FALSE(start_parks_on_clock(rig));

    CHECK(rig.sdk.call_count("start_exposure") == 2);
    CHECK(rig.camera->get_image_ready());
}

TEST_CASE("ZWO camera clock - USB 2.0 first start after a reconnect does not wait", "[zwo][camera][unit][clock]") {
    SettleRig rig(false);  // downloaded one exposure on the first connection
    rig.camera->set_connected(false);
    rig.camera->set_connected(true);

    CHECK_FALSE(start_parks_on_clock(rig));

    CHECK(rig.sdk.call_count("start_exposure") == 2);
    CHECK(rig.camera->get_image_ready());
}

TEST_CASE("ZWO camera clock - USB 2.0 exposure of 3 s or less does not wait after a download",
          "[zwo][camera][unit][clock]") {
    SettleRig rig(false);  // downloaded at once: a long exposure would wait the full 600 ms here

    CHECK_FALSE(start_parks_on_clock(rig, kShortExposure));

    CHECK(rig.sdk.call_count("start_exposure") == 2);
    const auto starts = rig.sdk.start_times();
    REQUIRE(starts.size() == 2);
    CHECK(starts[1] - starts[0] < std::chrono::milliseconds(600));
}

// ---- The exposure body on the task clock (decision 0020, issue #1981) ----

namespace {

// A connected camera whose exposures stay Working until the case says otherwise.
struct WorkingRig {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::CameraDriver> camera;

    WorkingRig() {
        sdk.auto_complete_exposure = false;
        sdk.frame_bytes = std::vector<std::uint8_t>(16 * 8 * 2, 1);
        camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
        camera->set_connected(true);
        clock.advance(std::chrono::seconds(1));
    }
};

}  // namespace

// Rule 4.3: the body polls the exposure status once per 50 ms wait on the task clock, and delivers the frame at the
// first poll that reads Success.
TEST_CASE("ZWO camera clock - the status poll waits on the task clock in 50 ms steps",
          "[zwo][camera][unit][clock][capture-session]") {
    WorkingRig rig;
    rig.camera->start_exposure(1.0, true);
    REQUIRE(rig.clock.wait_for_wait_count(1, kRendezvous));
    const int polls = rig.sdk.call_count("get_exposure_status");
    for (std::uint64_t step = 1; step <= 3; ++step) {
        rig.clock.advance(milliseconds(50));
        REQUIRE(rig.clock.wait_for_wait_count(step + 1, kRendezvous));
        CHECK(rig.sdk.call_count("get_exposure_status") == polls + static_cast<int>(step));
    }
    CHECK(rig.camera->get_camera_state() == alpacacore::CameraState::Exposing);
    CHECK_FALSE(rig.camera->get_image_ready());
    rig.sdk.set_exposure_status(FakeZWOSDK::ExposureStatus::Success);
    rig.clock.advance(milliseconds(50));
    REQUIRE(alpacacore::test::wait_exposure_end(*rig.camera));
    CHECK(rig.camera->get_image_ready());
    CHECK(rig.sdk.call_count("get_exposure_status") == polls + 4);
}

// Rule 2: the per-start margin is kExposureRetries x duration + 15 s, so the deadline of a 1 s exposure is 18 s; an
// exposure that never leaves Working is failed there.
TEST_CASE("ZWO camera clock - the watchdog covers both retries and ends a stuck exposure",
          "[zwo][camera][unit][clock][capture-session]") {
    WorkingRig rig;
    rig.camera->start_exposure(1.0, true);
    REQUIRE(rig.clock.wait_for_wait_count(1, kRendezvous));
    rig.clock.advance(milliseconds(17999));
    CHECK(rig.camera->get_camera_state() == alpacacore::CameraState::Exposing);
    CHECK_FALSE(rig.camera->get_image_ready());
    rig.clock.advance(milliseconds(1));
    CHECK(rig.camera->get_camera_state() == alpacacore::CameraState::Idle);
    std::string message;
    try {
        static_cast<void>(rig.camera->get_image_ready());
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
        message = ex.what();
    }
    CHECK(message.find("did not complete before its deadline") != std::string::npos);
    // The body the watchdog cancelled stops the SDK exposure it started.
    CHECK(rig.sdk.wait_for_call_count("stop_exposure", 1, kRendezvous));
}

// Rule 5: AbortExposure stops the SDK exposure, then cancels the body parked in the status poll and joins it, with no
// clock advance; the body sends no second stop.
TEST_CASE("ZWO camera clock - AbortExposure wakes the status poll without a clock advance",
          "[zwo][camera][unit][clock][capture-session]") {
    WorkingRig rig;
    rig.camera->start_exposure(10.0, true);
    REQUIRE(rig.clock.wait_for_wait_count(1, kRendezvous));
    const int stops = rig.sdk.call_count("stop_exposure");
    CHECK(alpacacore::test::zwo_returns_without_advance(rig.clock, [&] { rig.camera->abort_exposure(); }));
    CHECK(rig.sdk.call_count("stop_exposure") == stops + 1);
    CHECK(rig.clock.waiter_count() == 0);
    CHECK(rig.camera->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK_FALSE(rig.camera->get_image_ready());
    rig.camera->set_connected(false);
}

// Rules 5 and 7.2: a Disconnect stops the SDK exposure and joins the body before the close, with no clock advance.
TEST_CASE("ZWO camera clock - disconnect wakes the status poll without a clock advance",
          "[zwo][camera][unit][clock][capture-session]") {
    WorkingRig rig;
    rig.camera->start_exposure(10.0, true);
    REQUIRE(rig.clock.wait_for_wait_count(1, kRendezvous));
    const int stops = rig.sdk.call_count("stop_exposure");
    CHECK(alpacacore::test::zwo_returns_without_advance(rig.clock, [&] { rig.camera->set_connected(false); }));
    CHECK_FALSE(rig.camera->get_connected());
    CHECK(rig.sdk.call_count("stop_exposure") == stops + 1);
    CHECK(rig.clock.waiter_count() == 0);
    CHECK(rig.sdk.open_count() == 0);
}

// Rules 5 and 7.5: the destructor stops the SDK exposure and joins the body before the close, with no clock advance.
TEST_CASE("ZWO camera clock - destruction wakes the status poll without a clock advance",
          "[zwo][camera][unit][clock][capture-session]") {
    WorkingRig rig;
    rig.camera->start_exposure(10.0, true);
    REQUIRE(rig.clock.wait_for_wait_count(1, kRendezvous));
    const int stops = rig.sdk.call_count("stop_exposure");
    CHECK(alpacacore::test::zwo_returns_without_advance(rig.clock, [&] { rig.camera.reset(); }));
    CHECK(rig.sdk.call_count("stop_exposure") > stops);
    CHECK(rig.clock.waiter_count() == 0);
    CHECK(rig.sdk.open_count() == 0);
}

// Rule 4.1: the USB 2.0 settle wait runs in the body, so StartExposure returns at once; the trigger waits for the
// settle on the task clock.
TEST_CASE("ZWO camera clock - StartExposure returns before the USB 2.0 settle ends",
          "[zwo][camera][unit][clock][capture-session]") {
    SettleRig rig(false);
    const auto downloaded_at = rig.clock.now();

    CHECK(alpacacore::test::zwo_returns_without_advance(rig.clock,
                                                        [&] { rig.camera->start_exposure(kLongExposure, true); }));
    const bool parked = rig.clock.wait_for_waiters(1, kRendezvous);
    CHECK(parked);
    if (parked) {
        rig.clock.advance(milliseconds(599));
        CHECK(rig.sdk.call_count("start_exposure") == 1);  // still inside the settle window
        rig.clock.advance(milliseconds(1));
    }
    REQUIRE(rig.sdk.wait_for_call_count("start_exposure", 2, kRendezvous));
    const auto starts = rig.sdk.start_times();
    REQUIRE(starts.size() == 2);
    CHECK(starts[1] - downloaded_at >= milliseconds(600));
    REQUIRE(alpacacore::test::wait_exposure_end(*rig.camera));
    CHECK(rig.camera->get_image_ready());
}

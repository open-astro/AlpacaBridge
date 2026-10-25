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

namespace {

using alpacacore::AlpacaException;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZWOSDK;
using alpacacore::vendor::zwo::create_zwo_camera;
namespace AlpacaError = alpacacore::AlpacaError;
using std::chrono::milliseconds;

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

TEST_CASE("ZWO camera clock - exposure retries are paced by the task clock", "[zwo][camera][unit][clock]") {
    FakeZWOSDK sdk;
    FakeTaskClock clock;
    sdk.frame_bytes = std::vector<std::uint8_t>(16 * 8 * 2, 1);
    auto camera = create_zwo_camera(0, sdk.camera.camera_id, sdk, clock);
    camera->set_connected(true);
    // The retry window is measured from the epoch before the first retry.
    clock.advance(std::chrono::seconds(1));

    camera->start_exposure(0.001, true);
    const int starts = sdk.call_count("start_exposure");
    // One Failed per poll: a retry's restart resets the fake's own status.
    sdk.script_exposure_status(std::vector<FakeZWOSDK::ExposureStatus>(5, FakeZWOSDK::ExposureStatus::Failed));

    CHECK_FALSE(camera->get_image_ready());  // retry 1
    CHECK(sdk.call_count("start_exposure") == starts + 1);
    CHECK_FALSE(camera->get_image_ready());  // inside the 250 ms window: no retry
    CHECK(sdk.call_count("start_exposure") == starts + 1);
    clock.advance(milliseconds(249));
    CHECK_FALSE(camera->get_image_ready());
    CHECK(sdk.call_count("start_exposure") == starts + 1);
    clock.advance(milliseconds(1));
    CHECK_FALSE(camera->get_image_ready());  // retry 2
    CHECK(sdk.call_count("start_exposure") == starts + 2);
    clock.advance(milliseconds(250));
    CHECK(error_code([&] { static_cast<void>(camera->get_image_ready()); }) == AlpacaError::DriverException);
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

    std::thread starter([&] { rig.camera->start_exposure(kLongExposure, true); });
    const bool waiting = rig.clock.wait_for_waiters(1, kRendezvous);
    CHECK(waiting);
    if (waiting) {
        rig.clock.advance(milliseconds(599));
        CHECK(rig.sdk.call_count("start_exposure") == 1);  // still inside the settle window
        rig.clock.advance(milliseconds(1));
    }
    starter.join();

    REQUIRE(rig.sdk.call_count("start_exposure") == 2);
    const auto starts = rig.sdk.start_times();
    CHECK(starts[1] - downloaded_at >= milliseconds(600));
    CHECK(rig.camera->get_image_ready());  // no ASI_EXP_FAILED, so no retry
    CHECK(rig.sdk.call_count("start_exposure") == 2);
}

namespace {

// Starts exposure 2 on a thread and reports whether it parked on the clock. A start that wrongly waits is
// released after the check, so a broken driver fails the case instead of hanging it.
bool start_parks_on_clock(SettleRig& rig, double duration = kLongExposure) {
    std::thread starter([&] { rig.camera->start_exposure(duration, true); });
    const bool waited = rig.clock.wait_for_waiters(1, milliseconds(200));
    if (waited) rig.clock.advance(milliseconds(600));
    starter.join();
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

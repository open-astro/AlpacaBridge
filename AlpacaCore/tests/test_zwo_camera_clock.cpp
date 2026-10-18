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

    double reading = -1.0;
    std::thread reader([&] { reading = camera->get_ccd_temperature(); });
    // Six reads: the first, then five retries, each behind a 50 ms clock sleep.
    for (int retry = 1; retry <= 5; ++retry) {
        REQUIRE(clock.wait_for_wait_count(static_cast<std::uint64_t>(retry), kRendezvous));
        CHECK(sdk.call_count("get_control_value") == retry);
        clock.advance(milliseconds(50));
    }
    reader.join();
    CHECK(reading == Catch::Approx(0.0));
    CHECK(sdk.call_count("get_control_value") == 6);
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

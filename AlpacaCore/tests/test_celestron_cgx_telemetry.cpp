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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

#include "catch2_compat.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {
using namespace std::chrono_literals;

struct TelemetryRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::chrono::system_clock::time_point utc{1700000000s};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    TelemetryRig() {
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock, [this] { return utc; });
        driver->set_connected(true);
        home();
    }

    void home(bool park = false) {
        if (park)
            driver->park();
        else
            driver->find_home();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        REQUIRE_FALSE(driver->get_slewing());
        REQUIRE(driver->get_at_home());
    }

    void coordinates() {
        static_cast<void>(driver->get_altitude());
        static_cast<void>(driver->get_azimuth());
        static_cast<void>(driver->get_declination());
        static_cast<void>(driver->get_right_ascension());
        static_cast<void>(driver->get_side_of_pier());
    }

    int reads(std::uint8_t axis) const { return mount.count_command(axis, 0x01); }
};
}  // namespace

TEST_CASE("CGX stationary telemetry shares only a bounded raw pair", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    for (const bool parked : {false, true}) {
        if (parked) rig.home(true);
        const int ra = rig.reads(0x10), dec = rig.reads(0x11);
        const int guide_ra = rig.mount.count_command(0x10, 0x27);
        const int guide_dec = rig.mount.count_command(0x11, 0x27);
        const auto state = rig.driver->get_device_state();
        CHECK(state.size() == 13);
        CHECK(rig.reads(0x10) == ra + 1);
        CHECK(rig.reads(0x11) == dec + 1);
        CHECK(rig.mount.count_command(0x10, 0x27) == guide_ra + 1);
        CHECK(rig.mount.count_command(0x11, 0x27) == guide_dec + 1);
        const auto old_ra = rig.driver->get_right_ascension();
        rig.utc += 1s;
        CHECK(rig.driver->get_right_ascension() > old_ra);
        rig.clock.advance(29ms);
        rig.coordinates();
        CHECK(rig.reads(0x10) == ra + 1);
        CHECK(rig.reads(0x11) == dec + 1);
        // Hits must not slide the deadline: exactly 30 ms from refill expires.
        rig.clock.advance(1ms);
        rig.coordinates();
        CHECK(rig.reads(0x10) == ra + 2);
        CHECK(rig.reads(0x11) == dec + 2);
    }
}

TEST_CASE("CGX DeviceState keeps coordinate reads together across slow live guide status",
          "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    for (const bool parked : {false, true}) {
        if (parked) rig.home(true);
        const int ra = rig.reads(0x10), dec = rig.reads(0x11);
        const int guide_ra = rig.mount.count_command(0x10, 0x27);
        const int guide_dec = rig.mount.count_command(0x11, 0x27);
        rig.mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x27});
        auto pending = std::async(std::launch::async, [&] { return rig.driver->get_device_state(); });
        const bool waiting = rig.mount.wait_for_reply_fragment(2s);
        // Real guide-status reads straddle the unchanged 30 ms raw-pair TTL.
        rig.clock.advance(32ms);
        rig.mount.release_reply_tail();
        REQUIRE(waiting);
        const auto state = pending.get();
        for (const char* name : {"Altitude", "AtHome", "AtPark", "Azimuth", "Declination", "IsPulseGuiding",
                                 "RightAscension", "SideOfPier", "SiderealTime", "Slewing", "Tracking", "TimeStamp"}) {
            CHECK(std::count_if(state.begin(), state.end(), [name](const auto& entry) { return entry.name == name; }) ==
                  1);
        }
        CHECK(rig.reads(0x10) == ra + 1);
        CHECK(rig.reads(0x11) == dec + 1);
        CHECK(rig.mount.count_command(0x10, 0x27) == guide_ra + 1);
        CHECK(rig.mount.count_command(0x11, 0x27) == guide_dec + 1);
    }
}

TEST_CASE("CGX telemetry never substitutes cached labels for control or moving reads", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    // Sync's raw-label/branch read is fresh even at a warmed stationary home.
    rig.mount.set_encoder_position(0x11, 0x810000);
    const auto lst = rig.driver->get_sidereal_time();
    const int sync_ra = rig.reads(0x10), sync_dec = rig.reads(0x11);
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(lst, 85.0); });
    for (int wait = 1; wait <= 5; ++wait) {
        const bool waiting = rig.clock.wait_for_wait_count(wait, 2s);
        rig.clock.advance(20ms);
        CHECK(waiting);
    }
    sync.get();
    CHECK(rig.reads(0x10) == sync_ra + 8);
    CHECK(rig.reads(0x11) == sync_dec + 8);
    CHECK_FALSE(rig.driver->get_at_home());
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra + 5);
    CHECK(rig.reads(0x11) == dec + 5);
    rig.driver->set_tracking(true);
    const int tracking_ra = rig.reads(0x10), tracking_dec = rig.reads(0x11);
    rig.coordinates();
    CHECK(rig.reads(0x10) == tracking_ra + 5);
    CHECK(rig.reads(0x11) == tracking_dec + 5);
    rig.mount.set_auto_complete_motions(false);
    rig.mount.set_hold_guide_until_completed(true);
    const int guide_ra = rig.reads(0x10), guide_dec = rig.reads(0x11);
    rig.driver->pulse_guide(0, 100);
    rig.coordinates();
    CHECK(rig.reads(0x10) == guide_ra + 6);  // one control pair plus five telemetry pairs
    CHECK(rig.reads(0x11) == guide_dec + 6);
    rig.mount.complete_axis_guide(0x11);
    rig.clock.advance(20ms);
    rig.mount.set_auto_complete_motions(true);
}

TEST_CASE("CGX telemetry rehome and reconnect discard the previous raw frame", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    // Rehome republishes the same transport generation, but new encoder labels.
    rig.mount.set_encoder_position(0x11, 0x810000);
    rig.clock.advance(30ms);
    const auto displaced = rig.driver->get_declination();
    CHECK(displaced < 90.0);
    rig.home();
    const int home_ra = rig.reads(0x10), home_dec = rig.reads(0x11);
    CHECK(rig.driver->get_declination() > displaced);
    rig.coordinates();
    CHECK(rig.reads(0x10) == home_ra + 1);
    CHECK(rig.reads(0x11) == home_dec + 1);
    rig.driver->set_connected(false);
    rig.driver->set_connected(true);
    const int connect_ra = rig.reads(0x10), connect_dec = rig.reads(0x11);
    rig.coordinates();
    CHECK(rig.reads(0x10) == connect_ra + 1);
    CHECK(rig.reads(0x11) == connect_dec + 1);
}

TEST_CASE("CGX telemetry GOTO planning and in-flight coordinates stay fresh", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    rig.driver->set_tracking(true);
    rig.mount.set_auto_complete_motions(false);
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    double target_ra = rig.driver->get_sidereal_time() - 1.0;
    if (target_ra < 0.0) target_ra += 24.0;
    rig.driver->slew_to_coordinates_async(target_ra, 85.0);
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra + 6);
    CHECK(rig.reads(0x11) == dec + 6);
    // Completion samples a fresh pair; the motion waiter uses the same seam.
    const bool waiting = rig.clock.wait_for_wait_count(1, 2s);
    rig.mount.complete_axis_slew(0x10);
    rig.mount.complete_axis_slew(0x11);
    rig.clock.advance(200ms);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(waiting);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.reads(0x10) == ra + 7);
    CHECK(rig.reads(0x11) == dec + 7);
}

TEST_CASE("CGX telemetry failed refill drops the entire pair", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    rig.mount.drop_next_reply(0x11, 0x01, false);
    rig.clock.advance(30ms);
    CHECK_THROWS_AS(rig.driver->get_declination(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->get_right_ascension(), alpacacore::AlpacaException);
    CHECK(rig.reads(0x10) == ra + 1);
    CHECK(rig.reads(0x11) == dec + 1);
}

TEST_CASE("CGX telemetry pending stop refuses a warm pair and completion changes its key",
          "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    rig.mount.set_stop_completion_reads(2);
    rig.driver->abort_slew();
    const bool first_wait = rig.clock.wait_for_wait_count(1, 2s);
    CHECK_THROWS_AS(rig.driver->get_declination(), alpacacore::AlpacaException);
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
    rig.clock.advance(20ms);
    const bool second_wait = rig.clock.wait_for_wait_count(2, 2s);
    rig.clock.advance(20ms);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(first_wait);
    CHECK(second_wait);
    REQUIRE_FALSE(rig.driver->get_slewing());
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra + 1);
    CHECK(rig.reads(0x11) == dec + 1);
    rig.mount.set_stop_completion_reads(0);
    // A whole stop can finish between telemetry calls inside the TTL, while
    // AtHome remains true. Its generation must still invalidate the pair.
    rig.driver->abort_slew();
    const auto second_deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < second_deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE_FALSE(rig.driver->get_slewing());
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra + 2);
    CHECK(rig.reads(0x11) == dec + 2);
}

TEST_CASE("CGX telemetry bounds unobserved loss and refuses a known fault", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    rig.mount.drop_next_reply(0x10, 0x27, false);
    // Unobserved loss is accepted only inside the fixed 30 ms window.
    rig.clock.advance(29ms);
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
    CHECK_THROWS_AS(rig.driver->get_is_pulse_guiding(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->get_right_ascension(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->get_declination(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->get_altitude(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->get_azimuth(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->get_side_of_pier(), alpacacore::AlpacaException);
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
    rig.clock.advance(1ms);
    CHECK_THROWS_AS(rig.driver->get_right_ascension(), alpacacore::AlpacaException);
    rig.driver->set_connected(false);
    rig.driver->set_connected(true);
    // Recovery changed the transport generation, but did not restore calibration.
    CHECK_THROWS_AS(rig.driver->get_declination(), alpacacore::AlpacaException);
    rig.home();
    const int recovered_ra = rig.reads(0x10), recovered_dec = rig.reads(0x11);
    rig.coordinates();
    CHECK(rig.reads(0x10) == recovered_ra + 1);
    CHECK(rig.reads(0x11) == recovered_dec + 1);
}

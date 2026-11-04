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

// The CGX coordinate getters serve the state poll's frame (decision 0010, #1940). Every case holds the poll
// (cgx_clock_cleanup.h), so the only poll exchanges are the connect's first frame and the ones a case runs with
// poll_now(): the wire counts below are exact.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {
using namespace std::chrono_literals;
using alpacacore::test::require_alpaca_error;

struct TelemetryRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::chrono::system_clock::time_point utc{1700000000s};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    TelemetryRig() {
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, [this] { return utc; });
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

    void poll_now() { alpacacore::vendor::celestron::poll_state_now_for_testing(*driver); }

    int reads(std::uint8_t axis) const { return mount.count_command(axis, 0x01); }
};

// The link fault itself (decision 0009 rule 1), not a later guard that the same fault also trips.
void require_fault(const std::function<void()>& read) {
    try {
        read();
        FAIL_CHECK("no throw");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(error.what()).find("communications compromised") != std::string::npos);
    }
}
}  // namespace

TEST_CASE("CGX coordinate getters and DeviceState serve the polled frame without touching the wire",
          "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    for (const bool parked : {false, true}) {
        if (parked) rig.home(true);
        const int ra = rig.reads(0x10), dec = rig.reads(0x11);
        const int guide_ra = rig.mount.count_command(0x10, 0x27);
        const int guide_dec = rig.mount.count_command(0x11, 0x27);
        const auto state = rig.driver->get_device_state();
        CHECK(state.size() == 12);
        CHECK(std::none_of(state.begin(), state.end(), [](const auto& entry) { return entry.name == "UTCDate"; }));
        for (int pass = 0; pass < 3; ++pass) {
            rig.coordinates();
            CHECK_FALSE(rig.driver->get_is_pulse_guiding());
        }
        // Sky values are computed when read, from the frame and the current UTC.
        const auto old_ra = rig.driver->get_right_ascension();
        rig.utc += 1s;
        CHECK(rig.driver->get_right_ascension() > old_ra);
        CHECK(rig.reads(0x10) == ra);
        CHECK(rig.reads(0x11) == dec);
        CHECK(rig.mount.count_command(0x10, 0x27) == guide_ra);
        CHECK(rig.mount.count_command(0x11, 0x27) == guide_dec);
    }
}

TEST_CASE("CGX DeviceState TimeStamp is when the served frame was measured", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.poll_now();
    const auto fresh = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*rig.driver));
    CHECK(std::chrono::abs(std::chrono::system_clock::now() - fresh) < 1s);
    // Ten seconds of task-clock time with no poll: stale, still served, stamped with its age.
    rig.clock.advance(10s);
    CHECK_NOTHROW(rig.coordinates());
    const auto aged = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*rig.driver));
    CHECK(std::chrono::abs((std::chrono::system_clock::now() - 10s) - aged) < 1s);
    rig.poll_now();
    const auto renewed = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*rig.driver));
    CHECK(std::chrono::abs(std::chrono::system_clock::now() - renewed) < 1s);
}

TEST_CASE("CGX control paths read the axes and write the pair through to the frame", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    // Sync's raw-label/branch read is fresh even at a stationary home.
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
    // The synced labels are served before any poll.
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    CHECK(rig.driver->get_right_ascension() == Catch::Approx(lst).margin(1e-3));
    CHECK(rig.driver->get_declination() == Catch::Approx(85.0).margin(1e-3));
    rig.coordinates();
    rig.driver->set_tracking(true);
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
    rig.mount.set_auto_complete_motions(false);
    rig.mount.set_hold_guide_until_completed(true);
    rig.driver->pulse_guide(0, 100);
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra + 1);  // the guide's own polarity read; the getters read nothing
    CHECK(rig.reads(0x11) == dec + 1);
    rig.mount.complete_axis_guide(0x11);
    rig.clock.advance(20ms);
    rig.mount.set_auto_complete_motions(true);
}

// Sync writes the RA label, then the Dec label, then verifies both, with mutex_ free (decision 0016) and the reference
// still valid. A poll between the two writes publishes the new RA label beside the old Dec label. The fake lags the
// Dec readback by one read after the write, so the poll run during the first verify sleep reads exactly that pair.
TEST_CASE("CGX getters serve the pre-Sync pair while Sync writes the labels", "[celestron][cgx][telemetry]") {
    namespace cgx = alpacacore::vendor::celestron;
    TelemetryRig rig;
    rig.mount.set_encoder_position(0x11, 0x810000);
    rig.poll_now();
    const double before_ra = rig.driver->get_right_ascension();
    const double before_dec = rig.driver->get_declination();
    const double lst = rig.driver->get_sidereal_time();
    const auto old_dec_label = rig.mount.axis_position(0x11);
    const auto side =
        cgx::cgx_encoder_position_to_sky(rig.mount.axis_position(0x10), old_dec_label, lst, cgx::CgxHemisphere::North)
            .pointing_side;
    const auto new_dec_label =
        cgx::cgx_sky_position_to_encoders(lst, 85.0, lst, cgx::CgxHemisphere::North, side).second;
    rig.mount.set_label_read_offsets(0x11, {0, cgx::signed_cgx_encoder_delta(old_dec_label, new_dec_label)});
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(lst, 85.0); });
    // Waits 1-3 are the settle before the writes; wait 4 is the first verify sleep, after both label writes.
    for (int wait = 1; wait <= 3; ++wait) {
        CHECK(rig.clock.wait_for_wait_count(wait, 2s));
        rig.clock.advance(20ms);
    }
    CHECK(rig.clock.wait_for_wait_count(4, 2s));
    rig.poll_now();  // new RA label, old Dec label
    const double during_ra = rig.driver->get_right_ascension();
    const double during_dec = rig.driver->get_declination();
    rig.clock.advance(20ms);
    CHECK(rig.clock.wait_for_wait_count(5, 2s));
    rig.clock.advance(20ms);
    CHECK_NOTHROW(sync.get());
    CHECK(rig.mount.axis_position(0x11) == new_dec_label);  // the lagged read was the old Dec label
    CHECK(std::abs(lst - before_ra) > 0.1);                 // a new RA label would show
    CHECK(during_ra == Catch::Approx(before_ra).margin(1e-6));
    CHECK(during_dec == Catch::Approx(before_dec).margin(1e-6));
    // Sync publishes the verified pair.
    CHECK(rig.driver->get_right_ascension() == Catch::Approx(lst).margin(1e-3));
    CHECK(rig.driver->get_declination() == Catch::Approx(85.0).margin(1e-3));
}

TEST_CASE("CGX getters drop the pre-Sync pair when Sync fails after the label writes", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.mount.set_encoder_position(0x11, 0x810000);
    rig.poll_now();
    const double lst = rig.driver->get_sidereal_time();
    rig.mount.set_label_read_offsets(0x11, std::vector<std::int32_t>(40, 100));  // the Dec label never reads back
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(lst, 85.0); });
    for (int wait = 1; wait <= 40 && sync.wait_for(0s) != std::future_status::ready; ++wait) {
        if (rig.clock.wait_for_wait_count(wait, 2s)) rig.clock.advance(20ms);
    }
    require_alpaca_error([&] { sync.get(); }, alpacacore::AlpacaError::DriverException);
    rig.mount.set_label_read_offsets(0x11, {});
    // FindHome restores the reference; the getters serve its labels, not the pair the failed Sync held.
    rig.home();
    CHECK(rig.driver->get_declination() > 89.99);
}

// A frame is up to a poll period old (#1940 rig, ConformU PulseGuide). While tracking, the encoders follow the sky, so
// the served position is the one when the pair was read; with tracking off, the sky moves past still encoders.
TEST_CASE("CGX RightAscension holds between polls while tracking", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    const auto device_state_ra = [&] {
        for (const auto& entry : rig.driver->get_device_state())
            if (entry.name == "RightAscension") return std::get<double>(entry.value);
        FAIL("no RightAscension in DeviceState");
        return 0.0;
    };
    const auto advance = [&](std::chrono::milliseconds step) {
        rig.clock.advance(step);
        rig.utc += step;
    };
    rig.mount.set_encoder_position(0x11, 0x810000);  // off the pole, so Altitude moves with the sky
    rig.driver->set_tracking(true);
    rig.poll_now();
    const double ra = rig.driver->get_right_ascension();
    const double dec = rig.driver->get_declination();
    const double altitude = rig.driver->get_altitude();
    advance(200ms);
    CHECK(rig.driver->get_right_ascension() == Catch::Approx(ra).epsilon(0).margin(1e-9));
    CHECK(device_state_ra() == Catch::Approx(ra).epsilon(0).margin(1e-9));
    CHECK(rig.driver->get_declination() == Catch::Approx(dec).epsilon(0).margin(1e-9));
    // The sky turns under a held position.
    CHECK(rig.driver->get_altitude() != Catch::Approx(altitude).epsilon(0).margin(1e-9));
    // Tracking off: the encoders stand still and the served RA advances with sidereal time.
    rig.driver->set_tracking(false);
    rig.poll_now();
    const double still = rig.driver->get_right_ascension();
    advance(200ms);
    CHECK(rig.driver->get_right_ascension() - still == Catch::Approx(0.2 * 1.0027379 / 3600.0).epsilon(0).margin(1e-8));
}

TEST_CASE("CGX FindHome and reconnect replace the served frame", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    rig.mount.set_encoder_position(0x11, 0x810000);
    rig.poll_now();
    const auto displaced = rig.driver->get_declination();
    CHECK(displaced < 90.0);
    // FindHome publishes the home labels with the reference; no poll runs in between.
    const int home_ra = rig.reads(0x10), home_dec = rig.reads(0x11);
    rig.home();
    CHECK(rig.driver->get_declination() > 89.99);
    CHECK(rig.reads(0x10) == home_ra);
    CHECK(rig.reads(0x11) == home_dec);
    // A reconnect drops the frame and takes a new one before Connected reads true.
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, false);
    rig.mount.set_encoder_position(0x11, 0x810000);
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, true);
    const int connect_ra = rig.reads(0x10), connect_dec = rig.reads(0x11);
    // The continuity check fails on the moved Dec axis, so the reference is gone with the old frame.
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    rig.home();
    CHECK(rig.driver->get_declination() > 89.99);
    rig.coordinates();
    CHECK(rig.reads(0x10) == connect_ra);
    CHECK(rig.reads(0x11) == connect_dec);
}

TEST_CASE("CGX GOTO planning reads the axes and in-flight getters serve the polled frame",
          "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    rig.driver->set_tracking(true);
    rig.mount.set_auto_complete_motions(false);
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    double target_ra = rig.driver->get_sidereal_time() - 1.0;
    if (target_ra < 0.0) target_ra += 24.0;
    rig.driver->slew_to_coordinates_async(target_ra, 85.0);
    const auto before = rig.driver->get_declination();
    rig.coordinates();
    CHECK(rig.reads(0x10) == ra + 1);  // the planning read only
    CHECK(rig.reads(0x11) == dec + 1);
    // In flight the frame moves only with a poll.
    rig.mount.advance_motion(1s);
    CHECK(rig.driver->get_declination() == before);
    rig.poll_now();
    CHECK(rig.driver->get_declination() != before);
    // Completion samples a fresh pair; the motion waiter uses the same seam.
    const bool waiting = rig.clock.wait_for_wait_count(1, 2s);
    rig.mount.complete_axis_slew(0x10);
    rig.mount.complete_axis_slew(0x11);
    rig.clock.advance(200ms);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(waiting);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.reads(0x10) == ra + 3);
    CHECK(rig.reads(0x11) == dec + 3);
    CHECK(rig.driver->get_declination() == Catch::Approx(85.0).margin(1e-3));
}

TEST_CASE("CGX a failed poll keeps the frame and the getters raise the fault", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    rig.mount.drop_next_reply(0x11, 0x01, false);
    rig.poll_now();
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    require_fault([&] { static_cast<void>(rig.driver->get_declination()); });
    require_fault([&] { static_cast<void>(rig.driver->get_right_ascension()); });
    CHECK(rig.driver->get_connected());
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
}

TEST_CASE("CGX a pending stop refuses the frame until the stop completes", "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    rig.mount.set_stop_completion_reads(2);
    rig.driver->abort_slew();
    const bool first_wait = rig.clock.wait_for_wait_count(1, 2s);
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
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
    CHECK_NOTHROW(rig.coordinates());
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
}

TEST_CASE("CGX a known fault refuses the frame and a recovered link still needs FindHome",
          "[celestron][cgx][telemetry]") {
    TelemetryRig rig;
    rig.coordinates();
    rig.mount.drop_next_reply(0x10, 0x01, false);
    rig.poll_now();
    const int ra = rig.reads(0x10), dec = rig.reads(0x11);
    const std::vector<std::function<void()>> reads{[&] { static_cast<void>(rig.driver->get_is_pulse_guiding()); },
                                                   [&] { static_cast<void>(rig.driver->get_right_ascension()); },
                                                   [&] { static_cast<void>(rig.driver->get_declination()); },
                                                   [&] { static_cast<void>(rig.driver->get_altitude()); },
                                                   [&] { static_cast<void>(rig.driver->get_azimuth()); },
                                                   [&] { static_cast<void>(rig.driver->get_side_of_pier()); }};
    for (const auto& read : reads) require_fault(read);
    CHECK(rig.reads(0x10) == ra);
    CHECK(rig.reads(0x11) == dec);
    // The next poll recovers the stream, not the reference (decision 0011 rule 2).
    rig.poll_now();
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, false);
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, true);
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    rig.home();
    CHECK(rig.driver->get_declination() > 89.99);
}

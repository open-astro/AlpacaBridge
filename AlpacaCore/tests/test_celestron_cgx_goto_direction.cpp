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

// #2640: on the bare northern CGX a GOTO from home to a correct Normal-side target (HA +0.5 h, Dec -10) drove Dec the
// long way, over the pole, with RA going the right way. The controller does not take the shorter way around the 24-bit
// circle there; the fake models that with a GOTO seam the motion never crosses. Every GOTO leg (the first FAST leg,
// the flip GOTO after its home route and each SLOW correction) is watched: an axis whose shorter-way distance to its
// target grows past the bound is stopped and the slew fails. Every case uses a generic northern site and a fixed UTC,
// so each target is fixed, and runs on the fake task clock with the state poll held (decision 0005 rule 2).

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {
using namespace std::chrono_literals;
namespace AlpacaError = alpacacore::AlpacaError;
namespace cgx = alpacacore::vendor::celestron;

constexpr std::uint8_t kRa = 0x10;
constexpr std::uint8_t kDec = 0x11;
constexpr double kLatitude = 30.0;  // generic northern site
constexpr std::uint32_t kDecHome = cgx::kCgxEncoderStepsPerRevolution / 2;
// The driver's wrong-way bound (kGotoWrongWayCounts), in encoder counts.
constexpr std::int32_t kWrongWayBound = 2000;
// The driver's coast allowance for a retarget (kGotoCoastCounts): 5 degrees.
constexpr std::int32_t kCoastBound = static_cast<std::int32_t>(cgx::kCgxEncoderStepsPerRevolution / 72);
constexpr std::uint32_t kFast = 1000000;  // counts per second, ~21 deg/s: a long leg ends in a few simulated seconds

std::uint32_t counts(double degrees) { return cgx::encode_cgx_encoder_degrees(degrees); }

struct DirectionRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    std::string failure;

    DirectionRig() {
        REQUIRE(mount.ok());
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = kLatitude;
        info.site_longitude_degrees = -75.0;
        const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, now);
        alpacacore::test::set_cgx_connected(*driver, clock, true);
        mount.set_auto_complete_motions(true);
        REQUIRE_FALSE(call([&] { driver->find_home(); }));
        REQUIRE(drive([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        mount.set_auto_complete_motions(false);
        for (const auto axis : {kRa, kDec}) mount.set_axis_velocity_counts_per_second(axis, kFast);
        mount.set_tracking_counts_per_second(kRa, 195);  // sidereal
        driver->set_tracking(true);
    }

    // Waits up to `real` for `done`; whenever the driver waits on the clock, the clock and the motors advance `step`.
    bool drive(const std::function<bool()>& done, std::chrono::milliseconds step = 20ms,
               std::chrono::seconds real = 30s) {
        const auto deadline = std::chrono::steady_clock::now() + real;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (clock.wait_for_waiters(1, 1ms)) {
                mount.advance_motion(step);
                clock.advance(step);
                if (!clock.wait_for_woken_settled(2s)) return false;
            }
        }
        return true;
    }
    bool call(const std::function<void()>& command) {
        auto task = std::async(std::launch::async, command);
        alpacacore::test::drain_cgx_clock_task(task, clock);
        try {
            task.get();
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
        return false;
    }
    // The slew ended; a failure it raises through Slewing is kept in `failure`.
    bool ended() {
        try {
            return !driver->get_slewing();
        } catch (const alpacacore::AlpacaException& error) {
            REQUIRE(error.error_code() == AlpacaError::DriverException);
            failure = error.what();
            return true;
        }
    }
    void slew(double hour_angle, double declination) {
        const double ra = std::fmod(driver->get_sidereal_time() - hour_angle + 48.0, 24.0);
        REQUIRE_FALSE(call([&] { driver->slew_to_coordinates_async(ra, declination); }));
    }
    bool stopped_away() const { return failure.find("moved away from its GOTO target") != std::string::npos; }
    std::int32_t dec_from_home() const { return cgx::signed_cgx_encoder_delta(mount.axis_position(kDec), kDecHome); }
    // Moves Dec so its label reads `offset` counts from home (the label is the physical position plus the home offset).
    void set_dec_from_home(std::uint32_t offset) {
        mount.set_physical_position(kDec,
                                    mount.axis_physical_position(kDec) + kDecHome + offset - mount.axis_position(kDec));
    }
    int count(std::uint8_t axis, std::uint8_t command) const { return mount.count_command(axis, command); }
};

}  // namespace

TEST_CASE("Celestron CGX GOTO direction - a FAST leg driven the long way over the pole is stopped",
          "[celestron][cgx][telescope][goto-direction]") {
    DirectionRig rig;
    // The rig's GOTO from home: Dec +100 degrees to label 280 degrees. The seam at 270 degrees makes the fake drive
    // Dec the other way, -260 degrees, over the pole, as the rig did.
    rig.mount.set_goto_seam(kDec, 0xC00000U);
    const int dec_stops = rig.count(kDec, 0x24) + rig.count(kDec, 0x25);
    rig.slew(0.5, -10.0);
    REQUIRE(rig.drive([&] { return rig.ended(); }));
    INFO("failure: " << rig.failure << "; Dec at " << rig.dec_from_home() << " counts from home");
    CHECK(rig.stopped_away());
    CHECK(rig.failure.find("Dec") != std::string::npos);
    // Stopped within one 200 ms poll of the wrong way (200,000 counts at this rate), on the BeyondPole side of home.
    CHECK(rig.dec_from_home() <= 0);
    CHECK(rig.dec_from_home() > -static_cast<std::int32_t>(counts(10.0)));
    CHECK(rig.count(kDec, 0x24) + rig.count(kDec, 0x25) > dec_stops);
}

TEST_CASE("Celestron CGX GOTO direction - a GOTO leg that backs off less than the bound keeps going",
          "[celestron][cgx][telescope][goto-direction]") {
    DirectionRig rig;
    rig.mount.set_axis_velocity_counts_per_second(kDec, 1);  // Dec stands still until the case moves it
    rig.slew(0.5, 30.0);
    REQUIRE(rig.drive([&] { return rig.count(kDec, 0x02) == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(1, 2s));  // the first poll took the starting distance
    const auto dec = rig.mount.axis_physical_position(kDec);

    SECTION("just inside the bound: the leg lands") {
        rig.mount.set_physical_position(kDec, dec - static_cast<std::uint32_t>(kWrongWayBound - 10));
        REQUIRE(rig.drive([&] { return rig.count(kDec, 0x13) > 4; }));
        rig.mount.complete_axis_slew(kRa);
        rig.mount.complete_axis_slew(kDec);
        REQUIRE(rig.drive([&] { return rig.ended(); }));
        INFO("failure: " << rig.failure);
        CHECK(rig.failure.empty());
        CHECK(rig.dec_from_home() == static_cast<std::int32_t>(counts(60.0)));
    }
    SECTION("just past the bound: the leg is stopped") {
        rig.mount.set_physical_position(kDec, dec - static_cast<std::uint32_t>(kWrongWayBound + 10));
        REQUIRE(rig.drive([&] { return rig.ended(); }));
        INFO("failure: " << rig.failure);
        CHECK(rig.stopped_away());
    }
}

TEST_CASE("Celestron CGX GOTO direction - the flip GOTO after the home route is watched",
          "[celestron][cgx][telescope][goto-direction]") {
    DirectionRig rig;
    // HA -0.5 h is BeyondPole: the home route, then Dec -100 degrees to label 80 degrees. A seam at 135 degrees sends
    // the fake the long way, +260 degrees.
    rig.mount.set_goto_seam(kDec, 0x600000U);
    rig.slew(-0.5, -10.0);
    REQUIRE(rig.drive([&] { return rig.mount.axis_homing(kRa) && rig.mount.axis_homing(kDec); }));
    rig.mount.complete_axis_home(kRa);
    rig.mount.complete_axis_home(kDec);
    REQUIRE(rig.drive([&] { return rig.ended(); }));
    INFO("failure: " << rig.failure << "; Dec at " << rig.dec_from_home() << " counts from home");
    CHECK(rig.count(kDec, 0x02) == 1);
    CHECK(rig.stopped_away());
    CHECK(rig.dec_from_home() >= 0);
    CHECK(rig.dec_from_home() < static_cast<std::int32_t>(counts(10.0)));
}

TEST_CASE("Celestron CGX GOTO direction - a SLOW correction driven the long way is stopped",
          "[celestron][cgx][telescope][goto-direction]") {
    DirectionRig rig;
    rig.mount.set_axis_velocity_counts_per_second(kDec, 1);
    rig.slew(0.5, 30.0);
    REQUIRE(rig.drive([&] { return rig.count(kDec, 0x02) == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(1, 2s));
    // The FAST leg lands 1000 counts (77") short of the target; the correction to it would cross the seam.
    const std::uint32_t target = kDecHome + counts(60.0);
    rig.mount.complete_axis_slew(kRa);
    rig.mount.complete_axis_slew_at(kDec, target - 1000);
    rig.mount.set_goto_seam(kDec, target - 500);
    rig.mount.set_axis_velocity_counts_per_second(kDec, kFast);
    REQUIRE(rig.drive([&] { return rig.ended(); }));
    INFO("failure: " << rig.failure);
    CHECK(rig.count(kDec, 0x17) == 1);
    CHECK(rig.stopped_away());
    CHECK(cgx::signed_cgx_encoder_delta(rig.mount.axis_position(kDec), target) < 0);
    CHECK(cgx::signed_cgx_encoder_delta(rig.mount.axis_position(kDec), target) >
          -static_cast<std::int32_t>(counts(5.0)));
}

TEST_CASE("Celestron CGX GOTO direction - a retarget may coast away from its new target before it turns",
          "[celestron][cgx][telescope][goto-direction]") {
    DirectionRig rig;
    // The fake turns an axis at once; the controller cannot. Dec holds unless the case moves it, so each section moves
    // it as a decelerating axis would, one poll at a time.
    rig.mount.set_axis_velocity_counts_per_second(kDec, 1);
    rig.slew(0.5, 30.0);  // slew A: Dec +60 degrees from home
    REQUIRE(rig.drive([&] { return rig.count(kDec, 0x02) == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(1, 2s));
    // A is in flight, 40 degrees up and still climbing, when slew B retargets it 20 degrees back (+20 from home).
    rig.set_dec_from_home(counts(40.0));
    rig.slew(0.5, 70.0);
    REQUIRE(rig.count(kDec, 0x02) == 2);
    // B's first poll takes its distance. The clock's waiter may still be A's superseded body, so wait for the read.
    const int planned = rig.count(kDec, 0x01);
    REQUIRE(rig.drive([&] { return rig.count(kDec, 0x01) > planned; }));
    const auto at_next_poll = [&](std::uint32_t dec_from_home) {
        rig.set_dec_from_home(dec_from_home);
        const int reads = rig.count(kDec, 0x01);
        return rig.drive([&] { return rig.count(kDec, 0x01) > reads || rig.ended(); });
    };
    const auto lands = [&] {
        rig.mount.set_axis_velocity_counts_per_second(kDec, kFast);
        REQUIRE(rig.drive([&] { return rig.ended(); }));
        INFO("failure: " << rig.failure);
        CHECK(rig.failure.empty());
        CHECK(rig.dec_from_home() == static_cast<std::int32_t>(counts(20.0)));
    };

    SECTION("a 2.4 degree coast, as failure 0014 recorded from full rate: the slew lands") {
        REQUIRE(at_next_poll(counts(41.2)));
        REQUIRE(at_next_poll(counts(42.4)));
        lands();
    }
    SECTION("a coast past the allowance: the slew is stopped") {
        REQUIRE(at_next_poll(counts(40.0) + static_cast<std::uint32_t>(kCoastBound + 10)));
        REQUIRE(rig.drive([&] { return rig.ended(); }));
        INFO("failure: " << rig.failure);
        CHECK(rig.stopped_away());
    }
    SECTION("once it turns, the bound counts from the farthest point of the coast") {
        REQUIRE(at_next_poll(counts(41.0)));
        REQUIRE(at_next_poll(counts(40.5)));  // closing
        SECTION("backing off less than the bound past that point: the slew lands") {
            REQUIRE(at_next_poll(counts(41.0) + static_cast<std::uint32_t>(kWrongWayBound - 10)));
            lands();
        }
        SECTION("backing off past the bound: the slew is stopped") {
            REQUIRE(at_next_poll(counts(41.0) + static_cast<std::uint32_t>(kWrongWayBound + 10)));
            REQUIRE(rig.drive([&] { return rig.ended(); }));
            INFO("failure: " << rig.failure);
            CHECK(rig.stopped_away());
        }
    }
}

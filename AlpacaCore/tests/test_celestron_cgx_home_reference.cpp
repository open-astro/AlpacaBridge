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

// #2640: on the bare northern CGX a home-route slew interrupted by Park, then FindHome, then a same-side GOTO aimed the
// mount at the far side of the pier. The home reference is written only from a home whose search was seen running, a
// Park or AbortSlew during a home route leaves the reference unknown, the home pose reads Normal a few counts either
// side of the Dec home, and a GOTO whose encoder target puts the telescope below the horizon is refused before any
// command goes out. Every case uses a generic northern site and a fixed UTC, so each hour angle below is fixed, and
// runs on the fake task clock with the state poll held (decision 0005 rule 2).

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
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {
using namespace std::chrono_literals;
namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::require_alpaca_error;
namespace cgx = alpacacore::vendor::celestron;

constexpr std::uint8_t kRa = 0x10;
constexpr std::uint8_t kDec = 0x11;
constexpr double kLatitude = 30.0;  // generic northern site
constexpr std::uint32_t kRaHome = cgx::kCgxEncoderStepsPerRevolution / 4;
constexpr std::uint32_t kDecHome = cgx::kCgxEncoderStepsPerRevolution / 2;

std::uint32_t counts(double degrees) { return cgx::encode_cgx_encoder_degrees(degrees); }

// True when `call` throws an AlpacaException with `code`; any other outcome is false.
template <typename Call>
bool refuses(Call call, int code) {
    try {
        call();
    } catch (const alpacacore::AlpacaException& error) {
        return error.error_code() == code;
    }
    return false;
}

struct HomeRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    HomeRig() {
        REQUIRE(mount.ok());
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = kLatitude;
        info.site_longitude_degrees = -75.0;
        const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, now);
        alpacacore::test::set_cgx_connected(*driver, clock, true);
    }

    // Waits up to 5 s of real time for `done`, advancing the clock 20 ms whenever the driver waits on it.
    bool drive(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (clock.wait_for_waiters(1, 1ms)) {
                clock.advance(20ms);
                if (!clock.wait_for_woken_settled(2s)) return false;
            }
        }
        return true;
    }
    // Runs a command that may wait on the clock; true when it threw an AlpacaException.
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

    // A home whose search runs: both axes read searching until the case completes them at the physical home.
    void complete_home() {
        REQUIRE(drive([&] { return mount.axis_homing(kRa) && mount.axis_homing(kDec); }));
        mount.complete_axis_home(kRa);
        mount.complete_axis_home(kDec);
    }
    void find_home() {
        REQUIRE_FALSE(call([&] { driver->find_home(); }));
        complete_home();
        REQUIRE(drive([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    }
    void park() {
        REQUIRE_FALSE(call([&] { driver->park(); }));
        complete_home();
        REQUIRE(drive([&] { return driver->get_at_park() && !driver->get_slewing(); }));
    }
    void complete_slew() {
        mount.complete_axis_slew(kRa);
        mount.complete_axis_slew(kDec);
        REQUIRE(drive([&] { return idle(); }));
    }
    bool idle() const {
        try {
            return !driver->get_slewing();
        } catch (const alpacacore::AlpacaException&) {
            return true;  // a failed motion raises through Slewing once it ended
        }
    }
    double ra_at_hour_angle(double hour_angle) const {
        return std::fmod(driver->get_sidereal_time() - hour_angle + 48.0, 24.0);
    }
    int count(std::uint8_t axis, std::uint8_t command) const { return mount.count_command(axis, command); }
    bool reference_unknown() const {
        return refuses([&] { static_cast<void>(driver->get_right_ascension()); }, AlpacaError::DriverException);
    }
    // Where the axes physically point: the fake's physical home is 0, the counterweight-down home of the CGX.
    cgx::CgxSkyPosition physical_pose() const {
        return cgx::cgx_encoder_position_to_sky((mount.axis_physical_position(kRa) + kRaHome) & 0x00ffffffU,
                                                (mount.axis_physical_position(kDec) + kDecHome) & 0x00ffffffU,
                                                driver->get_sidereal_time(), cgx::CgxHemisphere::North);
    }
    double physical_altitude() const {
        const auto pose = physical_pose();
        constexpr double kRad = std::numbers::pi / 180.0;
        const double ha = (driver->get_sidereal_time() - pose.right_ascension_hours) * 15.0 * kRad;
        return std::asin(std::sin(pose.declination_degrees * kRad) * std::sin(kLatitude * kRad) +
                         std::cos(pose.declination_degrees * kRad) * std::cos(kLatitude * kRad) * std::cos(ha)) /
               kRad;
    }
    // Starts the home route to the other pointing side (HA -0.5 h, Dec -10, as the rig did) and leaves its search
    // running, the axes where the search took them: Dec 150 degrees the wrong way (a Park on 2026-10-28 left Dec more
    // than 160 degrees from its switch, docs/failures/0016-cgx-home-travel.md), RA 30 degrees.
    void start_interrupted_home_route() {
        REQUIRE_FALSE(call([&] { driver->slew_to_coordinates_async(ra_at_hour_angle(-0.5), -10.0); }));
        REQUIRE(drive([&] { return mount.axis_homing(kRa) && mount.axis_homing(kDec); }));
        mount.set_physical_position(kRa, counts(30.0));
        mount.set_physical_position(kDec, counts(-150.0));
        CHECK(reference_unknown());
    }
};

}  // namespace

TEST_CASE("Celestron CGX home reference - the #2640 rig sequence never GOTOs on a reference from a home that ran no "
          "search",
          "[celestron][cgx][telescope][home-reference]") {
    HomeRig rig;
    rig.park();
    REQUIRE_FALSE(rig.call([&] { rig.driver->unpark(); }));
    REQUIRE(rig.drive([&] { return rig.idle() && !rig.driver->get_at_park(); }));
    rig.driver->set_tracking(true);

    // Step 2: the home route to the east target, then Park mid-route. Park's home start runs no search: the mount
    // answers home done at once with the axes still where the route left them.
    rig.start_interrupted_home_route();
    rig.mount.set_inert_home_starts(kRa, 1);
    rig.mount.set_inert_home_starts(kDec, 1);
    const int ra_labels = rig.count(kRa, 0x04);
    const int dec_labels = rig.count(kDec, 0x04);
    static_cast<void>(rig.call([&] { rig.driver->park(); }));
    REQUIRE(rig.drive([&] { return rig.idle(); }));
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.reference_unknown());
    CHECK(rig.count(kRa, 0x04) == ra_labels);
    CHECK(rig.count(kDec, 0x04) == dec_labels);

    // Step 3: FindHome, whose home start also runs no search ("done in 3 s with no visible index search").
    static_cast<void>(rig.call([&] { rig.driver->unpark(); }));
    REQUIRE(rig.drive([&] { return rig.idle(); }));
    rig.mount.set_inert_home_starts(kRa, 1);
    rig.mount.set_inert_home_starts(kDec, 1);
    static_cast<void>(rig.call([&] { rig.driver->find_home(); }));
    REQUIRE(rig.drive([&] { return rig.idle(); }));
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK(rig.reference_unknown());
    CHECK(rig.count(kRa, 0x04) == ra_labels);
    CHECK(rig.count(kDec, 0x04) == dec_labels);

    // Step 6: the same-side GOTO (HA +0.5 h, Dec -10). No GOTO goes out on that reference; one that did would have
    // landed where the labels written at the off-home pose put it.
    const int gotos = rig.count(kDec, 0x02);
    static_cast<void>(rig.call([&] {
        rig.driver->set_tracking(true);
        rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(0.5), -10.0);
    }));
    if (rig.count(kDec, 0x02) > gotos) {
        rig.complete_slew();
        INFO("physical pose: side " << static_cast<int>(rig.physical_pose().pointing_side) << " altitude "
                                    << rig.physical_altitude());
        CHECK(rig.physical_pose().pointing_side == cgx::CgxPointingSide::Normal);
        CHECK(rig.physical_altitude() > 0.0);
    }
    CHECK(rig.count(kDec, 0x02) == gotos);

    // A FindHome whose search runs restores the reference, though Dec settles a count short of its home label.
    rig.mount.set_label_read_offsets(kDec, std::vector<std::int32_t>(64, -1));
    rig.find_home();
    cgx::poll_state_now_for_testing(*rig.driver);
    CHECK(rig.driver->get_side_of_pier() == 0);
    CHECK(std::abs(std::remainder(rig.driver->get_right_ascension() - rig.ra_at_hour_angle(6.0), 24.0)) < 1e-3);
    CHECK(rig.driver->get_declination() > 89.999);

    // The same-side GOTO now goes to the Normal side, above the horizon, with no home route.
    rig.driver->set_tracking(true);
    const int homes = rig.count(kDec, 0x0b);
    const int ra_gotos = rig.count(kRa, 0x02);
    REQUIRE_FALSE(rig.call([&] { rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(0.5), -10.0); }));
    REQUIRE(rig.drive([&] { return rig.count(kRa, 0x02) > ra_gotos && rig.count(kDec, 0x02) > gotos; }));
    CHECK(rig.count(kDec, 0x0b) == homes);
    rig.complete_slew();
    CHECK(rig.mount.axis_position(kRa) == ((kRaHome - counts(82.5)) & 0x00ffffffU));
    CHECK(rig.mount.axis_position(kDec) == ((kDecHome + counts(100.0)) & 0x00ffffffU));
    CHECK(rig.physical_pose().pointing_side == cgx::CgxPointingSide::Normal);
    CHECK(rig.physical_altitude() > 45.0);
}

TEST_CASE("Celestron CGX home reference - a Park during a home route never reads AtPark on an unknown reference",
          "[celestron][cgx][telescope][home-reference]") {
    HomeRig rig;
    rig.find_home();
    rig.driver->set_tracking(true);
    rig.start_interrupted_home_route();

    SECTION("Park's home start runs no search") {
        rig.mount.set_inert_home_starts(kRa, 1);
        rig.mount.set_inert_home_starts(kDec, 1);
        static_cast<void>(rig.call([&] { rig.driver->park(); }));
        REQUIRE(rig.drive([&] { return rig.idle(); }));
        CHECK_FALSE(rig.driver->get_at_park());
        CHECK_FALSE(rig.driver->get_at_home());
        CHECK(rig.reference_unknown());
    }
    SECTION("Park's search runs") {
        REQUIRE_FALSE(rig.call([&] { rig.driver->park(); }));
        REQUIRE(rig.drive([&] { return rig.mount.axis_homing(kRa) && rig.mount.axis_homing(kDec); }));
        CHECK_FALSE(rig.driver->get_at_park());
        CHECK(rig.reference_unknown());
        rig.mount.complete_axis_home(kRa);
        rig.mount.complete_axis_home(kDec);
        REQUIRE(rig.drive([&] { return rig.driver->get_at_park() && !rig.driver->get_slewing(); }));
        CHECK(rig.driver->get_at_home());
        CHECK_FALSE(rig.reference_unknown());
    }
}

TEST_CASE("Celestron CGX home reference - FindHome, Park and the home route write no reference from a home start "
          "that runs no search",
          "[celestron][cgx][telescope][home-reference]") {
    HomeRig rig;
    rig.find_home();
    // Off home, so a label write there would be a wrong reference.
    rig.driver->set_tracking(true);
    REQUIRE_FALSE(rig.call([&] { rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(2.0), 40.0); }));
    rig.complete_slew();
    rig.mount.set_inert_home_starts(kRa, 1);
    rig.mount.set_inert_home_starts(kDec, 1);
    const int ra_labels = rig.count(kRa, 0x04);
    const int dec_labels = rig.count(kDec, 0x04);
    const int ra_gotos = rig.count(kRa, 0x02);
    const int dec_gotos = rig.count(kDec, 0x02);
    const auto start = [&](const std::function<void()>& command) {
        static_cast<void>(rig.call(command));
        REQUIRE(rig.drive([&] { return rig.idle(); }));
    };

    SECTION("FindHome") { start([&] { rig.driver->find_home(); }); }
    SECTION("Park") { start([&] { rig.driver->park(); }); }
    SECTION("the home route of a slew to the other pointing side") {
        start([&] { rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(-2.0), 40.0); });
        CHECK(rig.count(kRa, 0x02) == ra_gotos);
        CHECK(rig.count(kDec, 0x02) == dec_gotos);
    }
    CHECK(rig.count(kRa, 0x04) == ra_labels);
    CHECK(rig.count(kDec, 0x04) == dec_labels);
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.reference_unknown());
}

TEST_CASE("Celestron CGX home reference - AbortSlew during a home route leaves the reference unknown",
          "[celestron][cgx][telescope][home-reference]") {
    HomeRig rig;
    rig.find_home();
    rig.driver->set_tracking(true);
    rig.start_interrupted_home_route();
    REQUIRE_FALSE(rig.call([&] { rig.driver->abort_slew(); }));
    REQUIRE(rig.drive([&] { return rig.idle(); }));
    CHECK(rig.reference_unknown());
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_tracking());
    CHECK(refuses([&] { rig.driver->set_tracking(true); }, AlpacaError::InvalidOperation));
    // FindHome establishes it again.
    rig.find_home();
    CHECK_FALSE(rig.reference_unknown());
}

TEST_CASE("Celestron CGX home reference - FindHome whose Dec settles a count short of home reads the home pose",
          "[celestron][cgx][telescope][home-reference]") {
    HomeRig rig;
    for (const std::int32_t offset : {-1, -2, -4, 1}) {
        INFO("Dec reads " << offset << " counts from its home label");
        rig.mount.set_label_read_offsets(kDec, std::vector<std::int32_t>(64, offset));
        rig.find_home();
        cgx::poll_state_now_for_testing(*rig.driver);
        CHECK(rig.driver->get_side_of_pier() == 0);
        CHECK(std::abs(std::remainder(rig.driver->get_right_ascension() - rig.ra_at_hour_angle(6.0), 24.0)) < 1e-3);
        CHECK(rig.driver->get_declination() > 89.999);
    }
}

TEST_CASE("Celestron CGX home reference - a GOTO below the horizon is refused on either pointing side, nothing sent",
          "[celestron][cgx][telescope][home-reference]") {
    HomeRig rig;
    rig.find_home();
    rig.driver->set_tracking(true);
    const auto ledger = [&] {
        std::vector<int> sent;
        for (const auto axis : {kRa, kDec})
            for (const std::uint8_t command : {0x02, 0x04, 0x06, 0x07, 0x0b, 0x17, 0x24})
                sent.push_back(rig.count(axis, command));
        return sent;
    };
    // HA +0.5 h is the Normal side, HA -0.5 h BeyondPole; Dec -70 is about 10 degrees below the horizon at 30 N.
    for (const double hour_angle : {0.5, -0.5}) {
        INFO("HA " << hour_angle << " h, Dec -70");
        const auto before = ledger();
        CHECK(refuses([&] { rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(hour_angle), -70.0); },
                      AlpacaError::InvalidValue));
        CHECK(ledger() == before);
        CHECK(rig.driver->get_tracking());
        CHECK_FALSE(rig.driver->get_slewing());
    }
    // The rig run's safe targets (HA -/+0.5 h, Dec -10, about 49 degrees up) and a target 4.5 degrees up on each side
    // (HA -/+3 h, Dec -45) are allowed: the floor is the horizon, so ConformU's targets halfway to it pass.
    rig.mount.set_auto_complete_motions(true);
    for (const auto& [hour_angle, dec] :
         std::vector<std::pair<double, double>>{{0.5, -10.0}, {-0.5, -10.0}, {3.0, -45.0}, {-3.0, -45.0}}) {
        INFO("HA " << hour_angle << " h, Dec " << dec);
        const int gotos = rig.count(kDec, 0x02);
        REQUIRE_FALSE(rig.call([&] { rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(hour_angle), dec); }));
        REQUIRE(rig.drive([&] { return rig.idle(); }));
        CHECK(rig.count(kDec, 0x02) > gotos);
        CHECK(std::abs(rig.driver->get_declination() - dec) < 1e-3);
    }
}

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

// #2411: the CGX motion fails closed. Sync is bounded to 5 degrees on the sky from the served position, the explicit
// AbortSlew and Unpark stops are verified by the encoders as well as the status, and DestinationSideOfPier predicts the
// side a slew lands on, the home-route flip included. Every case runs on the fake task clock with the state poll held.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
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

struct GotoSafetyRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    GotoSafetyRig() {
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        // A fixed UTC: the sidereal time, and so each hour angle below, is the same at every read.
        const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, now);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(drive([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    }

    // Waits up to 5 s of real time for `done`, advancing the clock 20 ms whenever a body waits on it, so the virtual
    // time moves only with the driver's own waits (never past the 30 s frame staleness bound by spinning).
    bool drive(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (clock.wait_for_waiters(1, 10ms)) {
                clock.advance(20ms);
                if (!clock.wait_for_woken_settled(2s)) return false;
            }
        }
        return true;
    }

    // Runs a call that waits on the task clock (a Sync's settle reads, a stop's polls) and returns or rethrows.
    void run(const std::function<void()>& call) {
        auto task = std::async(std::launch::async, call);
        alpacacore::test::drain_cgx_clock_task(task, clock);
        task.get();
    }

    void slew(double ra, double dec) {
        driver->slew_to_coordinates_async(ra, dec);
        REQUIRE(drive([&] { return !driver->get_slewing(); }));
        cgx::poll_state_now_for_testing(*driver);
    }

    double ra_at_hour_angle(double hour_angle) const {
        return std::fmod(driver->get_sidereal_time() - hour_angle + 24.0, 24.0);
    }

    // Steps a stop that waits on the clock 20 ms at a time until it no longer waits. `each_step` runs while it waits.
    void step_stop(const std::function<void()>& each_step = {}) {
        while (clock.wait_for_waiters(1, 500ms)) {
            if (each_step) each_step();
            clock.advance(20ms);
            REQUIRE(clock.wait_for_woken_settled(2s));
        }
    }

    void stop(bool unpark) {
        if (unpark)
            driver->unpark();
        else
            driver->abort_slew();
    }

    void park() {
        driver->park();
        REQUIRE(drive([&] { return driver->get_at_park() && !driver->get_slewing(); }));
    }
};

struct SkyPoint {
    double ra;
    double dec;
};

}  // namespace

TEST_CASE("Celestron CGX goto safety - a Sync more than 5 degrees from the mount is refused before any write",
          "[celestron][cgx][telescope][goto-safety]") {
    GotoSafetyRig rig;
    rig.driver->set_tracking(true);
    rig.slew(rig.ra_at_hour_angle(2.0), 40.0);
    REQUIRE(rig.driver->get_tracking());
    const SkyPoint served{rig.driver->get_right_ascension(), rig.driver->get_declination()};
    // The label writes and the tracking writes (on, off, both directions) of both axes, and the stops.
    const auto ledger = [&] {
        std::vector<int> counts;
        for (const auto axis : {kRa, kDec})
            for (const std::uint8_t command : {0x04, 0x06, 0x07, 0x24})
                counts.push_back(rig.mount.count_command(axis, command));
        return counts;
    };
    const auto before = ledger();
    const auto ra_encoder = rig.mount.axis_position(kRa);
    const auto dec_encoder = rig.mount.axis_position(kDec);
    const auto unchanged = [&] {
        CHECK(ledger() == before);
        CHECK(rig.mount.axis_position(kRa) == ra_encoder);
        CHECK(rig.mount.axis_position(kDec) == dec_encoder);
        CHECK(rig.driver->get_tracking());
        CHECK(std::abs(rig.driver->get_right_ascension() - served.ra) < 1e-9);
        CHECK(std::abs(rig.driver->get_declination() - served.dec) < 1e-9);
    };

    // 6 degrees north, and 0.5 h of RA at Dec 40 (5.7 degrees on the sky).
    for (const auto target :
         {SkyPoint{served.ra, served.dec + 6.0}, SkyPoint{std::fmod(served.ra + 0.5, 24.0), served.dec}}) {
        INFO("Sync to RA " << target.ra << " Dec " << target.dec);
        require_alpaca_error([&] { rig.run([&] { rig.driver->sync_to_coordinates(target.ra, target.dec); }); },
                             AlpacaError::InvalidValue);
        unchanged();
    }
    // SyncToTarget takes the same bound.
    rig.driver->set_target_right_ascension(served.ra);
    rig.driver->set_target_declination(served.dec + 6.0);
    require_alpaca_error([&] { rig.run([&] { rig.driver->sync_to_target(); }); }, AlpacaError::InvalidValue);
    unchanged();

    // 4.9 degrees is inside the bound: both labels are written and Tracking is restored.
    rig.run([&] { rig.driver->sync_to_coordinates(served.ra, served.dec + 4.9); });
    CHECK(rig.mount.count_command(kRa, 0x04) == before[0] + 1);
    CHECK(rig.mount.count_command(kDec, 0x04) == before[4] + 1);
    CHECK(rig.driver->get_tracking());
    CHECK(std::abs(rig.driver->get_declination() - (served.dec + 4.9)) < 1e-4);
}

TEST_CASE("Celestron CGX goto safety - AbortSlew and Unpark stay Slewing until the encoders hold still",
          "[celestron][cgx][telescope][goto-safety]") {
    for (const bool unpark : {false, true}) {
        INFO((unpark ? "Unpark" : "AbortSlew"));
        GotoSafetyRig rig;
        if (unpark) rig.park();
        // Dec reports done at the stop, then moves 1000 counts (77 arcsec) on each of its next 10 position reads.
        rig.mount.set_post_stop_motion_reads(kDec, 10, 1000);
        const auto dec_before = rig.mount.axis_physical_position(kDec);
        const int dec_reads = rig.mount.count_command(kDec, 0x01);
        const int dec_stops = rig.mount.count_command(kDec, 0x24);
        const int ra_stops = rig.mount.count_command(kRa, 0x24);
        rig.stop(unpark);
        // A stop that trusts the status alone is done without a wait; this one waits for the encoders.
        REQUIRE(rig.clock.wait_for_waiters(1, 2s));
        auto moved_at = rig.clock.now();
        auto last_position = dec_before;
        bool slewing_until_still = true;
        rig.step_stop([&] {
            const auto position = rig.mount.axis_physical_position(kDec);
            if (position != last_position) {
                last_position = position;
                moved_at = rig.clock.now();
            }
            slewing_until_still = slewing_until_still && rig.driver->get_slewing();
        });
        CHECK_FALSE(rig.driver->get_slewing());
        CHECK(slewing_until_still);
        // Done only once two reads at least 500 ms apart agreed, after the last move.
        CHECK(rig.clock.now() - moved_at >= 500ms);
        CHECK(rig.clock.now() - moved_at < 1s);
        // One stop per axis, every scripted move read, and reads went on after the last move.
        CHECK(rig.mount.count_command(kRa, 0x24) == ra_stops + 1);
        CHECK(rig.mount.count_command(kDec, 0x24) == dec_stops + 1);
        CHECK(rig.mount.count_command(kDec, 0x01) >= dec_reads + 12);
        CHECK(rig.mount.axis_physical_position(kDec) == ((dec_before + 10 * 1000) & 0x00ffffffU));
        CHECK_FALSE(rig.driver->get_at_park());
        CHECK(std::isfinite(rig.driver->get_declination()));
    }
}

TEST_CASE("Celestron CGX goto safety - a stop whose encoders never hold still fails at 3 s and latches the motion",
          "[celestron][cgx][telescope][goto-safety]") {
    GotoSafetyRig rig;
    rig.mount.set_post_stop_motion_reads(kDec, 1000000, 1000);
    const auto dec_before = rig.mount.axis_physical_position(kDec);
    const int dec_reads = rig.mount.count_command(kDec, 0x01);
    const int dec_stops = rig.mount.count_command(kDec, 0x24);
    const int ra_stops = rig.mount.count_command(kRa, 0x24);
    rig.driver->abort_slew();
    REQUIRE(rig.clock.wait_for_waiters(1, 2s));
    const auto started = rig.clock.now();
    rig.step_stop();
    const auto elapsed = rig.clock.now() - started;
    CHECK(elapsed >= 2980ms);
    CHECK(elapsed <= 3020ms);
    try {
        static_cast<void>(rig.driver->get_slewing());
        FAIL_CHECK("Slewing must raise the failed stop");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == AlpacaError::DriverException);
        CHECK(std::string(error.what()).find("did not confirm that both motor axes stopped") != std::string::npos);
    }
    // Unverified motion is latched: the coordinates refuse until a verified stop.
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
    // No stop was resent, and the fake moved exactly once per read the stop made.
    CHECK(rig.mount.count_command(kRa, 0x24) == ra_stops + 1);
    CHECK(rig.mount.count_command(kDec, 0x24) == dec_stops + 1);
    const int reads = rig.mount.count_command(kDec, 0x01) - dec_reads;
    CHECK(reads > 100);
    CHECK(rig.mount.axis_physical_position(kDec) ==
          ((dec_before + static_cast<std::uint32_t>(reads) * 1000) & 0x00ffffffU));
    // Disconnect's own stop finds the axis still, so the cleanup's checkpoint reads settle.
    rig.mount.set_post_stop_motion_reads(kDec, 0, 0);
}

TEST_CASE("Celestron CGX goto safety - a position read that fails during the stop leaves it unverified",
          "[celestron][cgx][telescope][goto-safety]") {
    GotoSafetyRig rig;
    const int dec_reads = rig.mount.count_command(kDec, 0x01);
    rig.mount.drop_next_reply(kDec, 0x01, false);  // the stop's first Dec position read times out (500 ms, real time)
    rig.driver->abort_slew();
    // A stop that trusts the status alone reads no position.
    REQUIRE(rig.drive([&] { return rig.mount.count_command(kDec, 0x01) > dec_reads; }));
    // A state poll's matched reply heals the link the timeout faulted; the kept stop failure still raises.
    const auto slewing_error = [&]() -> std::string {
        cgx::poll_state_now_for_testing(*rig.driver);
        try {
            static_cast<void>(rig.driver->get_slewing());
            return {};
        } catch (const alpacacore::AlpacaException& error) {
            return error.what();
        }
    };
    REQUIRE(rig.drive([&] { return slewing_error().starts_with("Celestron CGX stop failed: "); }));
    // The motion stays unverified: the coordinates refuse until a verified stop.
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
    CHECK(rig.mount.count_command(kDec, 0x24) == 1);
}

TEST_CASE("Celestron CGX goto safety - DestinationSideOfPier predicts the side a direct or home-route slew lands on",
          "[celestron][cgx][telescope][goto-safety]") {
    GotoSafetyRig rig;
    rig.driver->set_tracking(true);
    REQUIRE(rig.driver->get_side_of_pier() == 0);
    const auto homes = [&] { return rig.mount.count_command(kRa, 0x0b) + rig.mount.count_command(kDec, 0x0b); };
    const auto lands_as_predicted = [&](double ra, bool home_route) {
        const int predicted = rig.driver->get_destination_side_of_pier(ra, 40.0);
        const int homes_before = homes();
        rig.slew(ra, 40.0);
        CHECK(rig.driver->get_side_of_pier() == predicted);
        CHECK(homes() == homes_before + (home_route ? 2 : 0));
        CHECK(std::abs(std::remainder(rig.driver->get_right_ascension() - ra, 24.0)) < 1e-4);
        CHECK(std::abs(rig.driver->get_declination() - 40.0) < 1e-4);
        return predicted;
    };
    {
        INFO("west of the meridian (HA +1 h): Normal, direct");
        CHECK(lands_as_predicted(rig.ra_at_hour_angle(1.0), false) == 0);
    }
    {
        INFO("east of the meridian (HA -1 h): BeyondPole, through home");
        CHECK(lands_as_predicted(rig.ra_at_hour_angle(-1.0), true) == 1);
    }
    {
        INFO("on the meridian (HA 0): Normal, back through home");
        CHECK(lands_as_predicted(rig.driver->get_sidereal_time(), true) == 0);
    }
    {
        INFO("a retarget (#2399): a slew still running west is replaced by one east of the meridian");
        rig.mount.set_auto_complete_motions(false);
        rig.driver->slew_to_coordinates_async(rig.ra_at_hour_angle(1.0), 40.0);
        REQUIRE(rig.driver->get_slewing());
        rig.mount.set_auto_complete_motions(true);
        CHECK(lands_as_predicted(rig.ra_at_hour_angle(-1.0), true) == 1);
    }
}

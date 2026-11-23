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
//
// Motion-owner follow-ups (issue #2467): tests that pin the #2263 motion-claim table for this driver.
#ifndef _WIN32

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {
using namespace std::chrono_literals;

alpacacore::vendor::celestron::CelestronCgxConnectionInfo owner_info(const alpacacore::test::FakeCelestronCgxMount& m) {
    alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
    info.port_path = m.slave_path();
    info.site_latitude_degrees = 45.0;
    info.site_longitude_degrees = -75.0;
    return info;
}

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return predicate();
}

bool slew_finished(const alpacacore::TelescopeDriver& driver) {
    try {
        return !driver.get_slewing();
    } catch (const alpacacore::AlpacaException& error) {
        if (error.error_code() != alpacacore::AlpacaError::DriverException) throw;
        return false;
    }
}

struct Thrown {
    int code = 0;
    std::string text;
};

Thrown thrown_by(const std::function<void()>& call) {
    try {
        call();
    } catch (const alpacacore::AlpacaException& error) {
        return {error.error_code(), error.what()};
    }
    return {};
}

void connect_home_and_track(alpacacore::test::FakeCelestronCgxMount& mount, alpacacore::TelescopeDriver& driver) {
    mount.set_auto_complete_motions(true);
    driver.set_connected(true);
    driver.find_home();
    REQUIRE(wait_for([&] { return driver.get_at_home() && !driver.get_slewing(); }));
    driver.set_tracking(true);
}

constexpr const char* kOverlapText = "PulseGuide cannot overlap telescope motion or another guide pulse";

}  // namespace

TEST_CASE("Celestron CGX USB Telescope Driver - a pulse whose first ACK was lost is refused with its own text (#2467)",
          "[celestron][cgx][telescope][motion-owner]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount));
    connect_home_and_track(mount, *driver);

    // The RA packet is applied but its ACK is lost, and the mount keeps it active: the axis stays ambiguous.
    mount.set_hold_guide_until_completed(true);
    mount.drop_next_reply(0x10, 0x26, true);
    CHECK(thrown_by([&] { driver->pulse_guide(2, 100); }).code == alpacacore::AlpacaError::DriverException);
    const int guide_commands = mount.count_command(0x10, 0x26);

    const auto refused = thrown_by([&] { driver->pulse_guide(2, 100); });
    INFO(refused.text);
    CHECK(refused.code == alpacacore::AlpacaError::InvalidOperation);
    CHECK(refused.text != kOverlapText);
    CHECK(refused.text.find("acknowledgement") != std::string::npos);
    CHECK(mount.count_command(0x10, 0x26) == guide_commands);

    mount.complete_axis_guide(0x10);
    driver->abort_slew();
    CHECK(wait_for([&] { return slew_finished(*driver); }));
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - a stale RA pulse failure does not block a Dec pulse (#2467)",
          "[celestron][cgx][telescope][motion-owner]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount), clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    // An RA pulse whose packet the mount never ends fails past its own deadline. The wire stays in sync, so the
    // reference holds and only the kept failure is left.
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(2, 100);
    REQUIRE(wait_for(
        [&] {
            clock.advance(100ms);
            return thrown_by([&] { static_cast<void>(driver->get_is_pulse_guiding()); }).code ==
                   alpacacore::AlpacaError::DriverException;
        },
        10s));

    // The RA axis stays refused; the Dec axis takes a pulse.
    const int ra_guides = mount.count_command(0x10, 0x26);
    const int dec_guides = mount.count_command(0x11, 0x26);
    CHECK(thrown_by([&] { driver->pulse_guide(2, 100); }).code == alpacacore::AlpacaError::DriverException);
    CHECK(mount.count_command(0x10, 0x26) == ra_guides);
    const auto dec = thrown_by([&] { driver->pulse_guide(0, 100); });
    INFO(dec.text);
    CHECK(dec.code == 0);
    CHECK(mount.count_command(0x11, 0x26) == dec_guides + 1);

    // Sync, Tracking and the slew-class commands still refuse on either axis's failure (MotionTransition).
    mount.complete_axis_guide(0x11);
    CHECK(wait_for([&] {
        clock.advance(100ms);
        return mount.count_command(0x11, 0x27) > 0;
    }));
    CHECK(thrown_by([&] { driver->set_tracking(false); }).code == alpacacore::AlpacaError::DriverException);
    mount.complete_axis_guide(0x10);
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a FindHome or Park that superseded a slew and fails its start leaves the "
    "slew's restore unsent (#2467)",
    "[celestron][cgx][telescope][motion-owner]") {
    const bool park = GENERATE(false, true);
    INFO("park=" << park);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount), clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0), 25.0);
    REQUIRE(clock.wait_for_waiters(1, 2s));

    // The home command's first start is refused: the command supersedes the slew, then fails.
    mount.ack_next_with_payload(0x10, 0x0B, {0x00});
    const auto failed = thrown_by([&] { park ? driver->park() : driver->find_home(); });
    INFO(failed.text);
    CHECK(failed.code == alpacacore::AlpacaError::DriverException);

    // The superseded slew's arrival would restore Tracking; it must stay silent, and the failed start's own stop
    // leaves the mount idle with the reference dropped.
    mount.set_auto_complete_motions(true);
    mount.complete_axis_slew(0x10);
    mount.complete_axis_slew(0x11);
    const bool idle = wait_for([&] {
        clock.advance(20ms);
        return slew_finished(*driver);
    });
    CHECK(idle);
    CHECK_FALSE(driver->get_tracking());
    CHECK_FALSE(driver->get_at_home());
    CHECK_FALSE(driver->get_at_park());
}

TEST_CASE("Celestron CGX USB Telescope Driver - a retarget whose GOTO ACK is lost restores Tracking (#2467)",
          "[celestron][cgx][telescope][motion-owner]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount), clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0), 25.0);
    REQUIRE(clock.wait_for_waiters(1, 2s));
    REQUIRE_FALSE(driver->get_tracking());  // the first slew turned it off for its GOTO

    // The retarget supersedes it, then its RA GOTO is refused on the wire: the failed start owes the restore.
    mount.ack_next_with_payload(0x10, 0x02, {0x00});
    const auto failed = thrown_by(
        [&] { driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() - 2.0 + 24.0, 24.0), 30.0); });
    INFO(failed.text);
    CHECK(failed.code == alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_tracking());
    CHECK(wait_for([&] {
        clock.advance(20ms);
        return slew_finished(*driver);
    }));
}

#endif  // !_WIN32

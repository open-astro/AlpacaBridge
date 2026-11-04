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

// Decision 0009 on the CGX (#1940, staleness slice): three failed state polls latch a fault, a fault standing for
// util::kLinkStalenessBound loses the link, and only the client's Connect reopens it. The poll is paused and each
// case runs its polls on the test thread (poll_now), so the fault and the loss land on known task-clock times; the
// real time a muted poll costs is its 500 ms AUX timeout. Mirrors run_staleness_scenario in
// test_skywatcher_link_loss.cpp.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
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

struct LinkLossRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::string path;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    LinkLossRig() {
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        path = info.port_path;
        driver = cgx::create_celestron_cgx_telescope(0, info, clock);
        cgx::set_state_poll_paused_for_testing(*driver, true);  // the poll body waits its ticks and probes nothing
        driver->set_connected(true);
        driver->find_home();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        REQUIRE(driver->get_at_home());
        REQUIRE(driver->get_declination() > 89.99);
    }

    void poll_now() { cgx::poll_state_now_for_testing(*driver); }

    // Three failed polls latch the fault at the current task-clock time.
    void latch_fault() {
        mount.set_muted(true);
        for (int poll = 0; poll < 3; ++poll) poll_now();
        REQUIRE(driver->get_connected());
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }

    // Latches the fault, then lets it stand for the bound; the poll that sees that loses the link.
    alpacacore::util::TaskClock::clock::time_point lose_link() {
        latch_fault();
        clock.advance(alpacacore::util::kLinkStalenessBound);
        const auto lost_at = clock.now();
        poll_now();
        REQUIRE_FALSE(driver->get_connected());
        return lost_at;
    }

    std::size_t requests() const { return mount.requests().size(); }
};

// Every member that needs the mount; while lost each one throws NotConnected (decision 0009 rule 3).
std::vector<std::pair<const char*, std::function<void()>>> operations(alpacacore::TelescopeDriver& d) {
    return {
        {"RightAscension", [&] { static_cast<void>(d.get_right_ascension()); }},
        {"Declination", [&] { static_cast<void>(d.get_declination()); }},
        {"Altitude", [&] { static_cast<void>(d.get_altitude()); }},
        {"Azimuth", [&] { static_cast<void>(d.get_azimuth()); }},
        {"SideOfPier", [&] { static_cast<void>(d.get_side_of_pier()); }},
        {"Tracking", [&] { static_cast<void>(d.get_tracking()); }},
        {"TrackingRate", [&] { static_cast<void>(d.get_tracking_rate()); }},
        {"Slewing", [&] { static_cast<void>(d.get_slewing()); }},
        {"AtHome", [&] { static_cast<void>(d.get_at_home()); }},
        {"AtPark", [&] { static_cast<void>(d.get_at_park()); }},
        {"IsPulseGuiding", [&] { static_cast<void>(d.get_is_pulse_guiding()); }},
        {"SiderealTime", [&] { static_cast<void>(d.get_sidereal_time()); }},
        {"GuideRate", [&] { static_cast<void>(d.get_guide_rate()); }},
        {"Tracking=true", [&] { d.set_tracking(true); }},
        {"TrackingRate=1", [&] { d.set_tracking_rate(1); }},
        {"SiteLatitude=46", [&] { d.set_site_latitude(46.0); }},
        {"AbortSlew", [&] { d.abort_slew(); }},
        {"FindHome", [&] { d.find_home(); }},
        {"Park", [&] { d.park(); }},
        {"Unpark", [&] { d.unpark(); }},
        {"SlewToCoordinatesAsync", [&] { d.slew_to_coordinates_async(6.0, 30.0); }},
        {"SyncToCoordinates", [&] { d.sync_to_coordinates(6.0, 30.0); }},
        {"PulseGuide", [&] { d.pulse_guide(0, 100); }},
    };
}
}  // namespace

TEST_CASE("Celestron CGX link loss - a poll fault standing past the bound loses the link until Connect",
          "[celestron][cgx][link-loss]") {
    LinkLossRig rig;
    rig.latch_fault();
    const auto fault = rig.driver->get_link_fault();
    CHECK(fault.find("no valid reply to the state poll") != std::string::npos);
    // Faulted, not lost (rule 1): Connected stays true and the frame is refused with DriverException.
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_right_ascension()); }, AlpacaError::DriverException);
    rig.clock.advance(alpacacore::util::kLinkStalenessBound - 1s);
    rig.poll_now();
    CHECK(rig.driver->get_connected());
    CHECK(alpacacore::util::is_serial_port_in_use(rig.path));

    rig.clock.advance(1s);
    const auto lost_at = rig.clock.now();
    rig.poll_now();
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    for (const auto& [name, operation] : operations(*rig.driver)) {
        INFO(name << " while the link is lost");
        require_alpaca_error(operation, AlpacaError::NotConnected);
    }
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(rig.path));  // the fd and the port claim are released
    CHECK(rig.driver->get_link_fault() == fault);                    // the reason stays until Connect or Disconnect
    // The first detection wins: a later failed poll does not move the stamp.
    rig.clock.advance(5s);
    rig.poll_now();
    CHECK(cgx::consume_link_lost_at_for_testing(*rig.driver) == lost_at);
    CHECK_FALSE(cgx::consume_link_lost_at_for_testing(*rig.driver).has_value());

    // No autonomous reopen (rule 4): the mount answers again, the poll runs on, and nothing reaches it.
    rig.mount.set_muted(false);
    const auto requests = rig.requests();
    cgx::set_state_poll_paused_for_testing(*rig.driver, false);
    for (int second = 0; second < 120; ++second) {
        rig.clock.advance(1s);
        REQUIRE(rig.clock.wait_for_woken_settled(2s));
    }
    CHECK(rig.clock.waiter_count() == 0);  // the poll body ended at the loss
    CHECK(rig.requests() == requests);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(rig.path));

    // Connect relinks without a Disconnect, re-runs the identity probe and keeps no reference (decision 0011 rule 3).
    const int identity = rig.mount.count_command(0x10, 0xfe);
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
    CHECK(rig.mount.count_command(0x10, 0xfe) == identity + 1);
    CHECK(rig.driver->get_link_fault().empty());
    CHECK_FALSE(rig.driver->get_at_home());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
    rig.driver->find_home();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(rig.driver->get_declination() > 89.99);
}

TEST_CASE("Celestron CGX link loss - the next Connect consumes the loss stamp, a Disconnect keeps it",
          "[celestron][cgx][link-loss]") {
    LinkLossRig rig;
    const auto lost_at = rig.lose_link();
    // Decision 0009 alternative E: a client's Disconnect does not consume the stamp; it clears the reason. Time
    // moves first, so a Disconnect that consumed the stamp and re-stamped the loss would show a later time.
    rig.clock.advance(std::chrono::seconds(1));
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, false);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    // The Disconnect kept the stamp. Reading it consumes it, so the Connect below finds none; the relink case
    // after it shows that the Connect consumes a stamp itself.
    CHECK(cgx::consume_link_lost_at_for_testing(*rig.driver) == lost_at);
    rig.mount.set_muted(false);
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, true);
    CHECK(rig.driver->get_connected());
    CHECK_FALSE(cgx::consume_link_lost_at_for_testing(*rig.driver).has_value());

    // A loss stamped and then relinked over directly is consumed the same way.
    const auto second = rig.lose_link();
    CHECK(second > lost_at);
    rig.mount.set_muted(false);
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
    CHECK_FALSE(cgx::consume_link_lost_at_for_testing(*rig.driver).has_value());
}

TEST_CASE("Celestron CGX link loss - a fault cleared inside the bound keeps the link but not the reference",
          "[celestron][cgx][link-loss]") {
    LinkLossRig rig;
    rig.latch_fault();
    rig.clock.advance(alpacacore::util::kLinkStalenessBound - 1s);
    rig.mount.set_muted(false);
    rig.poll_now();  // the next reply clears the latch
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(alpacacore::util::is_serial_port_in_use(rig.path));
    CHECK_FALSE(cgx::consume_link_lost_at_for_testing(*rig.driver).has_value());
    // A timed-out poll is a session error (decision 0011 rule 2): the stream is back, the reference is not.
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
    rig.driver->find_home();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rig.driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(rig.driver->get_declination() > 89.99);
}

TEST_CASE("Celestron CGX link loss - a stop in flight at the loss never reopens the port",
          "[celestron][cgx][link-loss]") {
    LinkLossRig rig;
    rig.driver->set_tracking(true);
    rig.mount.set_auto_complete_motions(false);
    rig.mount.set_hold_guide_until_completed(true);
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(0, 1000);
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, 2s));  // the guide body waits on the clock
    // A Tracking write takes the transition lock and joins the guide; the cancelled guide sleeps on the task
    // clock while the mount reports it active.
    auto tracking = std::async(std::launch::async, [&] { rig.driver->set_tracking(false); });
    REQUIRE(rig.clock.wait_for_wait_count(waits + 2, 2s));
    // AbortSlew's stop worker waits on the clock for that lock, so it sends nothing until the next clock step.
    rig.driver->abort_slew();
    REQUIRE(rig.clock.wait_for_wait_count(waits + 3, 2s));
    rig.latch_fault();
    // The step wakes the guide (its muted status read fails and the Tracking write ends) and the stop worker,
    // which finds the lock still held and waits again; the poll on the test thread then loses the link.
    rig.clock.advance(alpacacore::util::kLinkStalenessBound);
    rig.poll_now();
    REQUIRE_FALSE(rig.driver->get_connected());
    REQUIRE(tracking.wait_for(5s) == std::future_status::ready);
    try {
        tracking.get();
    } catch (const alpacacore::AlpacaException&) {  // the stop displaced it, or the link was gone
    }
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(rig.path));
    const auto requests = rig.requests();
    // The stop worker runs now, on a closed fd: each failed stop resynchronizes, which never reopens the port.
    rig.clock.advance(20ms);
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    CHECK(rig.requests() == requests);
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(rig.path));
}

namespace {

// A pulled cable (decision 0009 rule 2a, #1940 transport-gone slice): the mount sits behind a by-id style name, so a
// replugged mount (a new fake on a new pty) comes back under the name the driver was configured with. The poll is
// paused, as in LinkLossRig; the case runs a poll on the test thread when it needs one.
struct HangupRig {
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::test::FakeCelestronCgxMount> mount = plug();
    alpacacore::test::ReplugPort port{*mount};
    std::string node = mount->slave_path();  // the pty behind the name, which the port claim is keyed on
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    HangupRig() {
        cgx::CelestronCgxConnectionInfo info;
        info.port_path = port.path.string();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        driver = cgx::create_celestron_cgx_telescope(0, info, clock);
        cgx::set_state_poll_paused_for_testing(*driver, true);
    }

    static std::unique_ptr<alpacacore::test::FakeCelestronCgxMount> plug() {
        auto fake = std::make_unique<alpacacore::test::FakeCelestronCgxMount>();
        REQUIRE(fake->ok());
        fake->set_auto_complete_motions(true);
        return fake;
    }

    // Connected and homed, so the reference is valid until the loss.
    void connect_and_home() {
        driver->set_connected(true);
        driver->find_home();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        REQUIRE(driver->get_at_home());
        REQUIRE(alpacacore::util::is_serial_port_in_use(node));
    }

    // A new mount on a new pty, under the configured name.
    void replug() {
        mount = plug();
        port.replug(*mount);
        node = mount->slave_path();
    }

    // Rule 3: Connected false, every operation NotConnected, DeviceState empty, the fd and the claim released, and
    // the reason names the pulled cable.
    void check_lost() {
        CHECK_FALSE(driver->get_connected());
        CHECK(driver->get_device_state().empty());
        for (const auto& [name, operation] : operations(*driver)) {
            INFO(name << " after the cable was pulled");
            require_alpaca_error(operation, AlpacaError::NotConnected);
        }
        CHECK_FALSE(alpacacore::util::is_serial_port_in_use(node));
        CHECK(driver->get_link_fault().find("Celestron CGX mount serial link lost (") != std::string::npos);
    }

    // Runs `call` on another thread, pulls the cable while its first held reply is in flight, and returns the call's
    // error code (0 for none) and how long it took after the pull.
    std::pair<int, std::chrono::steady_clock::duration> pull_during(const std::function<void()>& call) {
        auto task = std::async(std::launch::async, [&] {
            try {
                call();
                return 0;
            } catch (const alpacacore::AlpacaException& error) {
                return error.error_code();
            }
        });
        REQUIRE(mount->wait_for_reply_fragment(2s));
        const auto pulled = std::chrono::steady_clock::now();
        mount->sever_link();
        REQUIRE(task.wait_for(5s) == std::future_status::ready);
        return {task.get(), std::chrono::steady_clock::now() - pulled};
    }
};

// A Length read waits out the 500 ms AUX timeout; a hangup must end the call well before that.
constexpr auto kAtOnce = 400ms;

}  // namespace

TEST_CASE("Celestron CGX link loss - a cable pulled during a command loses the link at once and Connect reopens it",
          "[celestron][cgx][link-loss][link-lost]") {
    HangupRig rig;
    rig.connect_and_home();
    rig.mount->fragment_next_reply();  // the Tracking write's ACK is in flight when the cable is pulled
    const auto [code, took] = rig.pull_during([&] { rig.driver->set_tracking(true); });
    CHECK(code == AlpacaError::NotConnected);
    CHECK(took < kAtOnce);
    rig.check_lost();
    const auto reason = rig.driver->get_link_fault();
    CHECK(reason.find("(hangup)") != std::string::npos);

    // The next poll tick ends the poll and stamps the loss once (rule 3).
    cgx::poll_state_now_for_testing(*rig.driver);
    const auto lost_at = rig.clock.now();

    // Rule 4: nothing relinks on its own. The poll body ends at its next tick and time reopens nothing.
    rig.replug();
    cgx::set_state_poll_paused_for_testing(*rig.driver, false);
    for (int second = 0; second < 10; ++second) {
        rig.clock.advance(1s);
        REQUIRE(rig.clock.wait_for_woken_settled(2s));
    }
    CHECK(rig.clock.waiter_count() == 0);  // the poll body has ended
    CHECK(rig.mount->requests().empty());
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault() == reason);  // rule 5: kept until the next Connect or Disconnect
    CHECK(cgx::consume_link_lost_at_for_testing(*rig.driver) == lost_at);

    // The client's Connect reopens the replugged port without a Disconnect first, re-runs the identity probe, clears
    // the reason and keeps no reference (decision 0011 rule 3: FindHome again).
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
    CHECK(rig.mount->count_command(0x10, 0xfe) == 1);
    CHECK(rig.driver->get_link_fault().empty());
    CHECK_FALSE(rig.driver->get_at_home());
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_declination()); }, AlpacaError::DriverException);
}

TEST_CASE("Celestron CGX link loss - a cable pulled during the state poll ends the poll in that tick",
          "[celestron][cgx][link-loss][link-lost]") {
    HangupRig rig;
    rig.connect_and_home();
    rig.mount->fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x01});  // the poll's RA position read
    const auto polled_at = rig.clock.now();
    const auto [code, took] = rig.pull_during([&] { cgx::poll_state_now_for_testing(*rig.driver); });
    CHECK(code == 0);  // the poll counts the failure; it does not throw
    CHECK(took < kAtOnce);
    rig.check_lost();
    CHECK(rig.driver->get_link_fault().find("(hangup)") != std::string::npos);
    // on_lost ran in that tick: the loss is stamped now, not at the 30 s staleness bound.
    CHECK(cgx::consume_link_lost_at_for_testing(*rig.driver) == polled_at);
    // A Disconnect clears the reason (rule 5).
    alpacacore::test::set_cgx_connected(*rig.driver, rig.clock, false);
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Celestron CGX link loss - a cable pulled during the connect fails the Connect with NotConnected",
          "[celestron][cgx][link-loss][link-lost]") {
    HangupRig rig;
    SECTION("the identity query") { rig.mount->fragment_next_reply(); }
    SECTION("the first state poll, which the connect retries before failing") {
        rig.mount->fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x01});
    }
    const auto [code, took] = rig.pull_during([&] { rig.driver->set_connected(true); });
    CHECK(code == AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(rig.node));
    rig.replug();
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
}

TEST_CASE("Celestron CGX link loss - a cable pulled mid-operation loses the link and the body ends on its own",
          "[celestron][cgx][link-loss][link-lost]") {
    HangupRig rig;
    rig.connect_and_home();
    rig.driver->set_tracking(true);
    rig.mount->set_auto_complete_motions(false);
    rig.mount->set_hold_guide_until_completed(true);
    const auto idle = rig.clock.waiter_count();  // the paused poll body
    const auto waits = rig.clock.wait_count();
    SECTION("a slew") {
        double target_ra = rig.driver->get_sidereal_time() - 1.0;
        if (target_ra < 0.0) target_ra += 24.0;
        rig.driver->slew_to_coordinates_async(target_ra, 85.0);
    }
    SECTION("a park") { rig.driver->park(); }
    SECTION("a pulse guide") { rig.driver->pulse_guide(0, 1000); }
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, 2s));  // the body is out on the mount and waits on the clock
    rig.mount->sever_link();
    // Its next read meets the pulled cable; each further read fails at once on the released port.
    for (int i = 0; i < 20 && rig.clock.waiter_count() > idle; ++i) {
        rig.clock.advance(250ms);
        REQUIRE(rig.clock.wait_for_woken_settled(2s));
    }
    CHECK(rig.clock.waiter_count() == idle);  // the body ended on its own
    rig.check_lost();
}

#endif  // !_WIN32

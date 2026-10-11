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

// Sky-Watcher getters and DeviceState served from the polled snapshot
// (decision 0010, Sky-Watcher adoption; link-loss policy of decision 0009).
// The driver runs on a FakeTaskClock, so its status poll runs only when a case
// advances the clock, and the fake mount answers on the wall clock with an
// injected latency per transaction (set_reply_latency): a getter that asked the
// board would pay it and show in transactions_served().

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeMountProfile;
using alpacacore::test::FakeSkyWatcherMount;
using alpacacore::test::FakeTaskClock;

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kLatency = milliseconds(100);
constexpr auto kGetterBudget = milliseconds(20);
constexpr auto kRendezvous = milliseconds(2000);

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount, FakeTaskClock& clock) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0, {}, {}, clock);
    driver->set_connected(true);
    return driver;
}

// The seam between "the poll is parked on the clock" and the case moving time:
// a step wakes the poll once; waiting for the woken thread lets it finish (or
// give up on a silent board) before the next one.
void step(FakeTaskClock& clock, std::chrono::nanoseconds d) {
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    clock.advance(d);
    clock.wait_for_woken_settled(std::chrono::seconds(10));
}

template <typename Fn>
std::chrono::microseconds timed(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
}

void require_not_connected(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        return;
    }
    FAIL("expected AlpacaException(NotConnected)");
}

// "2026-10-10T21:46:30.626Z" -> system_clock time point.
std::chrono::system_clock::time_point parse_timestamp(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    REQUIRE(std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi, &s, &ms) == 7);
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm(&tm)) + milliseconds(ms);
}

std::string time_stamp_of(const alpacacore::TelescopeDriver& driver) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == "TimeStamp") {
            return std::get<std::string>(entry.value);
        }
    }
    return {};
}

}  // namespace

TEST_CASE("SkyWatcher snapshot - getters and DeviceState make no device transaction at 100 ms per transaction",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    REQUIRE(driver->get_connected());
    // The connect's first poll is over once it parks on the clock; the fake
    // counts each reply just after sending it, so let the last count land.
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    std::this_thread::sleep_for(milliseconds(50));

    mount.set_reply_latency(kLatency);
    const int before = mount.transactions_served();

    const std::vector<std::pair<const char*, std::function<void()>>> getters = {
        {"RightAscension", [&] { (void)driver->get_right_ascension(); }},
        {"Declination", [&] { (void)driver->get_declination(); }},
        {"Altitude", [&] { (void)driver->get_altitude(); }},
        {"Azimuth", [&] { (void)driver->get_azimuth(); }},
        {"SideOfPier", [&] { (void)driver->get_side_of_pier(); }},
        {"Slewing", [&] { (void)driver->get_slewing(); }},
        {"Tracking", [&] { (void)driver->get_tracking(); }},
        {"AtPark", [&] { (void)driver->get_at_park(); }},
        {"AtHome", [&] { (void)driver->get_at_home(); }},
        {"IsPulseGuiding", [&] { (void)driver->get_is_pulse_guiding(); }},
        {"SiderealTime", [&] { (void)driver->get_sidereal_time(); }},
        {"DeviceState", [&] { (void)driver->get_device_state(); }},
    };
    for (const auto& [name, call] : getters) {
        CAPTURE(name);
        const auto spent = timed(call);
        CHECK(spent < kGetterBudget);
    }
    CHECK(mount.transactions_served() == before);
    mount.set_reply_latency(milliseconds(0));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - a client write reads back before the next poll for every setter ConformU checks",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    REQUIRE(driver->get_connected());

    struct Case {
        const char* name;
        std::function<void()> write;
        std::function<bool()> read_back;
    };
    const std::vector<Case> cases = {
        {"Tracking", [&] { driver->set_tracking(true); }, [&] { return driver->get_tracking(); }},
        {"RightAscensionRate", [&] { driver->set_right_ascension_rate(0.5); },
         [&] { return driver->get_right_ascension_rate() == 0.5; }},
        {"DeclinationRate", [&] { driver->set_declination_rate(2.0); },
         [&] { return driver->get_declination_rate() == 2.0; }},
        {"GuideRate",
         [&] {
             alpacacore::GuideRate rate;
             rate.ra = 0.004;
             rate.dec = 0.003;
             driver->set_guide_rate(rate);
         },
         [&] { return driver->get_guide_rate().ra == 0.004 && driver->get_guide_rate().dec == 0.003; }},
        {"TargetRightAscension", [&] { driver->set_target_right_ascension(5.5); },
         [&] { return driver->get_target_right_ascension() == 5.5; }},
        {"TargetDeclination", [&] { driver->set_target_declination(-12.25); },
         [&] { return driver->get_target_declination() == -12.25; }},
        {"SiteElevation", [&] { driver->set_site_elevation(250.0); },
         [&] { return driver->get_site_elevation() == 250.0; }},
        {"SlewSettleTime", [&] { driver->set_slew_settle_time(3); },
         [&] { return driver->get_slew_settle_time() == 3; }},
        {"DoesRefraction", [&] { driver->set_does_refraction(true); }, [&] { return driver->get_does_refraction(); }},
        {"ApertureDiameter", [&] { driver->set_aperture_diameter(0.2); },
         [&] { return driver->get_aperture_diameter() == 0.2; }},
        {"FocalLength", [&] { driver->set_focal_length(1.2); }, [&] { return driver->get_focal_length() == 1.2; }},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        c.write();  // on a fast link: the write may reach the board
        mount.set_reply_latency(kLatency);
        bool read = false;
        const auto spent = timed([&] { read = c.read_back(); });
        mount.set_reply_latency(milliseconds(0));
        CHECK(read);
        CHECK(spent < kGetterBudget);
    }
    driver->set_declination_rate(0.0);
    driver->set_right_ascension_rate(0.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - a poll that sampled before a rate setter anchored cannot undo the anchor (PR #221)",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_right_ascension_rate(0.0);
    const double before = driver->get_right_ascension();
    const auto started = std::chrono::steady_clock::now();

    // The mount and the model disagree by 72 arcsec. A poll samples the mount
    // (slowly: each transaction takes 100 ms) while the rate write anchors the
    // model in place; that poll must be dropped, or its raw angle re-anchors
    // the model on the jump and ConformU reads a rate error.
    mount.jump_axis_degrees(1, 0.02);
    mount.set_reply_latency(kLatency);
    const int frames = mount.frames_seen('j');
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    clock.advance(milliseconds(150));  // wakes the poll
    REQUIRE(mount.wait_for_frames('j', frames + 1, kRendezvous));
    driver->set_right_ascension_rate(0.5);
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(10)));
    mount.set_reply_latency(milliseconds(0));

    const double after = driver->get_right_ascension();
    // Model motion only: the new offset (0.5 s of RA per sidereal second) runs
    // for the time the setter took, and is far under the jump (1.3e-3 h).
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(std::abs(after - before) < 0.5 / 3600.0 * elapsed + 5e-5);
    driver->set_right_ascension_rate(0.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - a poll that sampled before a Sync cannot put the old position back",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 3.0 + 24.0, 24.0);
    const double target_dec = 50.0;
    REQUIRE(std::abs(driver->get_declination() - target_dec) > 10.0);

    // Park a poll between its sample (the mount still at home) and its publish.
    // The connect's first poll is over once the poll is parked on the clock.
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    std::promise<void> in_hook;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> first{true};
    sw::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        if (first.exchange(false)) {
            in_hook.set_value();
            released.wait();
        }
    });
    clock.advance(milliseconds(150));
    REQUIRE(in_hook.get_future().wait_for(kRendezvous) == std::future_status::ready);

    // The client syncs while that stale frame is parked ...
    std::thread syncer([&] { driver->sync_to_coordinates(target_ra, target_dec); });
    for (int i = 0; i < 400; ++i) {
        if (std::abs(mount.axis_degrees(2)) > 1.0) {
            break;  // the sync's position write has reached the board
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    for (int i = 0; i < 100 && clock.wait_for_waiters(1, milliseconds(20)); ++i) {
        clock.advance(milliseconds(50));  // whatever the sync waits on
        std::this_thread::sleep_for(milliseconds(10));
    }
    syncer.join();

    // ... and the frame is then published: it is dropped, so the read lands on
    // the synced position, not on the pre-sync one.
    release.set_value();
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(10)));
    sw::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    CHECK(std::abs(driver->get_declination() - target_dec) < 1.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - a poll that sampled before a board read cannot put the older position back",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));

    // A slow poll has read both positions (the mount at home) and is still
    // reading the status when the mount moves and SetPark reads it from the
    // board. That direct read is the newer one, though the poll ends after it.
    mount.set_reply_latency(kLatency);
    const int frames = mount.frames_seen('j');
    clock.advance(milliseconds(150));  // wakes the poll
    REQUIRE(mount.wait_for_frames('j', frames + 2, kRendezvous));
    mount.jump_axis_degrees(2, 30.0);
    driver->set_park();
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(10)));
    mount.set_reply_latency(milliseconds(0));

    // 30 deg off the pole, not the home position the poll sampled.
    CHECK(std::abs(driver->get_declination() - 60.0) < 1.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - asynchronous initiators make no position read", "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    // No poll runs after the connect's first one: the clock never moves.
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));
    // The slew bodies never run, so only each initiator's own traffic counts.
    sw::set_slew_spawn_for_testing(*driver, [](std::function<void()>) { return std::thread([] {}); });

    const auto position_reads = [&](const std::function<void()>& initiator) {
        const int before = mount.frames_seen('j');
        initiator();
        return mount.frames_seen('j') - before;
    };
    const double lst = driver->get_sidereal_time();
    CHECK(position_reads([&] { driver->move_axis(1, 0.0); }) == 0);  // nothing moving: the no-op
    CHECK(position_reads([&] { driver->move_axis(1, 1.0); }) == 0);
    CHECK(position_reads([&] { driver->move_axis(1, 0.0); }) == 0);  // the stop of that motion
    CHECK(position_reads([&] { driver->slew_to_coordinates_async(std::fmod(lst - 2.0 + 24.0, 24.0), 40.0); }) == 0);
    CHECK(position_reads([&] { driver->find_home(); }) == 0);
    CHECK(position_reads([&] { driver->park(); }) == 0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - MoveAxis anchors the model where it changes the rate with no board read",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;  // moves in real time, like the model
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    REQUIRE(clock.wait_for_waiters(1, kRendezvous));  // no poll re-anchors the model from here
    constexpr double kRate = 1.0;                     // deg/s on the Dec axis

    // Start: the last anchor is the connect's frame, 400 ms old, and the five
    // start transactions take 50 ms each. The new rate applies from the start,
    // not from that anchor or from the first of those transactions.
    std::this_thread::sleep_for(milliseconds(400));
    const double at_rest = driver->get_declination();
    mount.set_reply_latency(milliseconds(50));
    driver->move_axis(1, kRate);
    mount.set_reply_latency(milliseconds(0));
    const double started = driver->get_declination();
    CHECK(std::abs(started - at_rest) < 0.1);

    // A new rate on the moving axis: the 0.4 deg covered so far is kept.
    std::this_thread::sleep_for(milliseconds(400));
    const double moving = driver->get_declination();
    REQUIRE(std::abs(moving - started) > 0.3);
    driver->move_axis(1, -kRate);
    const double reversed = driver->get_declination();
    CHECK(std::abs(reversed - moving) < 0.1);

    // Stop: the motion up to the stop is kept too.
    std::this_thread::sleep_for(milliseconds(400));
    const double moving_back = driver->get_declination();
    REQUIRE(std::abs(moving_back - reversed) > 0.3);
    driver->move_axis(1, 0.0);
    CHECK(std::abs(driver->get_declination() - moving_back) < 0.1);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - a rate write does not hide a GOTO the poll measured", "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);

    // A GOTO-mode run the driver did not start (one that survived a relink,
    // say): no driver flag is set, so Slewing answers from the frame.
    driver->command_blind(":G200", true);  // Dec axis to GOTO mode
    mount.hold_running(2, true);           // and ":f" reports it running
    step(clock, milliseconds(150));        // a poll measures it
    REQUIRE(driver->get_slewing());

    // The rate setter re-anchors the model in place (PR #221). That drops the
    // poll in flight, but the board did not change: Slewing stays true.
    driver->set_declination_rate(1.0);
    CHECK(driver->get_slewing());
    mount.hold_running(2, false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - DeviceState TimeStamp is when the poll measured, and a stale frame is still served",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);

    // Fresh: the stamp is about now.
    auto stamp = parse_timestamp(time_stamp_of(*driver));
    CHECK(std::abs(std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count()) < 1.0);

    // The poll stops sampling while the task clock moves on 3 s: the connect-time
    // frame is now older than the 200 ms freshness bound.
    sw::set_status_poll_paused_for_testing(*driver, true);
    step(clock, milliseconds(1500));
    step(clock, milliseconds(1500));
    CHECK(driver->get_link_fault().empty());
    CHECK(driver->get_connected());

    // Stale is served, not refused, and the stamp says how old it is.
    CHECK_NOTHROW(driver->get_right_ascension());
    CHECK_FALSE(driver->get_device_state().empty());
    stamp = parse_timestamp(time_stamp_of(*driver));
    const double age = std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count();
    CHECK(age > 2.5);
    CHECK(age < 3.5);
    sw::set_status_poll_paused_for_testing(*driver, false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - a frame older than the link-loss bound with no latch is a lost link",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);

    // A poll that stopped answering the clock publishes nothing: no fault is
    // latched, yet the frame is older than util::kLinkStalenessBound.
    sw::set_status_poll_paused_for_testing(*driver, true);
    step(clock, alpacacore::util::kLinkStalenessBound - seconds(1));
    CHECK_NOTHROW(driver->get_right_ascension());  // 29 s old: stale, served
    step(clock, seconds(2));
    CHECK(driver->get_link_fault().empty());
    require_not_connected([&] { driver->get_right_ascension(); });
    require_not_connected([&] { driver->get_slewing(); });
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    require_not_connected([&] { driver->set_tracking(true); });
    sw::set_status_poll_paused_for_testing(*driver, false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - Connected=true relinks a stale-frame lost link", "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);

    // The poll stops answering the clock: no latch, the frame ages out.
    sw::set_status_poll_paused_for_testing(*driver, true);
    step(clock, alpacacore::util::kLinkStalenessBound + seconds(1));
    REQUIRE_FALSE(driver->get_connected());

    // Connected=true with the poll still paused must not hit the idempotency
    // return: it relinks, re-seeds the frame and restarts the poll.
    driver->set_connected(true);
    sw::set_status_poll_paused_for_testing(*driver, false);
    CHECK(driver->get_connected());
    CHECK_NOTHROW(driver->get_right_ascension());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - stale-frame lost link past the window, then relinked: both axes stop",
          "[skywatcher][snapshot]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    driver->move_axis(1, FakeSkyWatcherMount::kSiderealDegPerSec * 8.0);
    REQUIRE(mount.axis_running(1));
    REQUIRE(mount.axis_running(2));

    // The poll stops answering the clock: no latch, the frame ages out.
    sw::set_status_poll_paused_for_testing(*driver, true);
    step(clock, alpacacore::util::kLinkStalenessBound + seconds(1));
    REQUIRE_FALSE(driver->get_connected());

    // The mount kept turning unattended for longer than the preserve window,
    // counted from when the frame crossed the bound (open-astro#521).
    clock.advance(sw::detail::relink_motion_preserve_window() + seconds(5));
    REQUIRE(mount.axis_running(1));

    driver->set_connected(true);
    sw::set_status_poll_paused_for_testing(*driver, false);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher snapshot - UDP link pulled past the window, then relinked: both axes stop",
          "[skywatcher][snapshot][udp]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    sw::ConnectionInfo info = endpoint(mount);
    info.response_timeout_ms = 100;
    auto driver = sw::create_skywatcher_telescope(0, info, 39.7392, -104.9903, 1609.0, {}, {}, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);                                           // RA axis turns
    driver->move_axis(1, FakeSkyWatcherMount::kSiderealDegPerSec * 8.0);  // and Dec
    REQUIRE(mount.axis_running(1));
    REQUIRE(mount.axis_running(2));

    // The link goes quiet: nothing is asked of the mount by a client, and the
    // status poll alone latches the fault (decision 0009, stage one) ...
    mount.set_silent(true);
    for (int i = 0; i < 6 && driver->get_link_fault().empty(); ++i) {
        step(clock, milliseconds(150));
    }
    REQUIRE_FALSE(driver->get_link_fault().empty());
    CHECK(driver->get_connected());  // faulted, not lost

    // ... and past the staleness bound the link is lost (stage two), with
    // link_lost_at stamped for UDP too.
    step(clock, alpacacore::util::kLinkStalenessBound + seconds(1));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    require_not_connected([&] { driver->get_right_ascension(); });

    // The mount kept turning unattended for longer than the preserve window.
    clock.advance(sw::detail::relink_motion_preserve_window() + seconds(5));
    mount.set_silent(false);
    CHECK(mount.axis_running(1));
    CHECK(mount.axis_running(2));

    driver->set_connected(true);  // only the client's Connect relinks
    REQUIRE(driver->get_connected());
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(driver->get_device_state().empty());
    driver->set_connected(false);
}

#endif  // _WIN32

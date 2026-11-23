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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
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

alpacacore::vendor::celestron::CelestronCgxConnectionInfo owner_info(const alpacacore::test::FakeCelestronCgxMount& m) {
    alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
    info.port_path = m.slave_path();
    info.site_latitude_degrees = 51.477833;
    info.site_longitude_degrees = -0.0015;
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

    // The RA packet is applied but its ACK is lost, and the mount keeps it active: the axis stays ambiguous. Without
    // auto-completion the packet stays active until the case ends it, so the body is still watching it (for up to
    // the 2 s packet plus the 0.5 s margin, real time) when the second pulse arrives.
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    mount.drop_next_reply(0x10, 0x26, true);
    CHECK(thrown_by([&] { driver->pulse_guide(2, 2000); }).code == alpacacore::AlpacaError::DriverException);
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

TEST_CASE("Celestron CGX USB Telescope Driver - a stale RA pulse failure refuses a Dec pulse too (#2467)",
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

    // A kept pulse failure refuses every PulseGuide until a command clears it, so IsPulseGuiding, which raises it,
    // never reports a pulse it cannot describe: the Dec axis is refused as well, and no guide packet goes out.
    const int ra_guides = mount.count_command(0x10, 0x26);
    const int dec_guides = mount.count_command(0x11, 0x26);
    for (const int direction : {2, 0}) {  // East (RA), North (Dec)
        const auto refused = thrown_by([&] { driver->pulse_guide(direction, 100); });
        INFO("direction " << direction << ": " << refused.text);
        // REQUIRE: an accepted pulse would park its body on the clock, and the Tracking write below would join it.
        REQUIRE(refused.code == alpacacore::AlpacaError::DriverException);
    }
    CHECK(mount.count_command(0x10, 0x26) == ra_guides);
    CHECK(mount.count_command(0x11, 0x26) == dec_guides);

    // Tracking refuses on the same failure (MotionTransition).
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

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a pulse superseded before its end-of-pulse read sends that read nothing "
    "(#2467)",
    "[celestron][cgx][telescope][motion-owner]") {
    using alpacacore::vendor::celestron::CgxPulsePreLockPoint;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::Latch at_end_read;  // before the driver: the hook captures it
    std::atomic<bool> held{false};
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount), clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // Hold the RA pulse body where it is about to take the motion-start lock for its end-of-pulse position read.
    alpacacore::vendor::celestron::set_pulse_pre_lock_hook_for_testing(*driver, [&](CgxPulsePreLockPoint point) {
        if (point == CgxPulsePreLockPoint::GuideEndFrame && !held.exchange(true)) at_end_read.block();
    });
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(2, 100);
    REQUIRE(clock.wait_for_waiters(1, 2s));  // the body watches its packet
    mount.complete_axis_guide(0x10);
    clock.advance(20ms);
    REQUIRE(at_end_read.wait_entered());

    // FindHome supersedes the held body, then its home start is refused, so it starts no body of its own. None of
    // its wire traffic (its guide-active read, the tracking write, the refused home and the stop after it) is a
    // position read, and the state poll is held: every position read after this point is the held body's.
    mount.ack_next_with_payload(0x10, 0x0B, {0x00});
    const auto failed = thrown_by([&] { driver->find_home(); });
    INFO(failed.text);
    REQUIRE(failed.code == alpacacore::AlpacaError::DriverException);
    const int ra_reads = mount.count_command(0x10, 0x01);
    const int dec_reads = mount.count_command(0x11, 0x01);

    at_end_read.release();
    // FindHome starts no pulse, so the held body is still its slot's current one: IsPulseGuiding is true until it
    // returns.
    CHECK(wait_for([&] { return !driver->get_is_pulse_guiding(); }));
    CHECK(mount.count_command(0x10, 0x01) == ra_reads);
    CHECK(mount.count_command(0x11, 0x01) == dec_reads);
    alpacacore::vendor::celestron::set_pulse_pre_lock_hook_for_testing(*driver, {});
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a pulse superseded before a guide-active poll sends that poll nothing "
    "(#2467)",
    "[celestron][cgx][telescope][motion-owner]") {
    using alpacacore::vendor::celestron::CgxPulsePreLockPoint;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::Latch at_poll;  // before the driver: the hook captures it
    std::atomic<bool> held{false};
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount), clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // Hold the RA pulse body at its first guide poll, past its unlocked stop check and before its first
    // MC_AUX_GUIDE_ACTIVE read.
    alpacacore::vendor::celestron::set_pulse_pre_lock_hook_for_testing(*driver, [&](CgxPulsePreLockPoint point) {
        if (point == CgxPulsePreLockPoint::GuidePoll && !held.exchange(true)) at_poll.block();
    });
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(2, 100);
    REQUIRE(at_poll.wait_entered());

    // FindHome supersedes the held body. Its own wait reads MC_AUX_GUIDE_ACTIVE on RA too, so the packet is ended
    // first (that wait reads it once and goes on), the home start is refused (no home body runs), and the count is
    // taken only once FindHome has returned: with the state poll held, every RA guide-active read after that point is
    // the held body's.
    mount.complete_axis_guide(0x10);
    mount.ack_next_with_payload(0x10, 0x0B, {0x00});
    const auto failed = thrown_by([&] { driver->find_home(); });
    INFO(failed.text);
    REQUIRE(failed.code == alpacacore::AlpacaError::DriverException);
    const int ra_polls = mount.count_command(0x10, 0x27);

    at_poll.release();
    // FindHome starts no pulse, so the held body is still its slot's current one: IsPulseGuiding is true until it
    // returns.
    CHECK(wait_for([&] { return !driver->get_is_pulse_guiding(); }));
    CHECK(mount.count_command(0x10, 0x27) == ra_polls);
    alpacacore::vendor::celestron::set_pulse_pre_lock_hook_for_testing(*driver, {});
}

namespace {
// Holds each guide-active read of a superseding command's wait (its timeout recorded) until the case has moved the
// task clock for it. Capped at 2 s of real time, like Latch.
struct GuideReadGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<std::optional<std::chrono::milliseconds>> held;  // the held read's timeout

    void hold(std::optional<std::chrono::milliseconds> timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        held = timeout;
        cv.notify_all();
        static_cast<void>(cv.wait_for(lock, 2s, [&] { return !held.has_value(); }));
    }
    std::optional<std::optional<std::chrono::milliseconds>> wait_held(std::chrono::milliseconds real_timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        static_cast<void>(cv.wait_for(lock, real_timeout, [&] { return held.has_value(); }));
        return held;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            held.reset();
        }
        cv.notify_all();
    }
};
}  // namespace

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a superseding command's guide-end wait ends within 3.05 s on the task clock "
    "(#2467)",
    "[celestron][cgx][telescope][motion-owner]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    GuideReadGate gate;  // before the driver: the hook captures it
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, owner_info(mount), clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // A running RA pulse whose packet the mount keeps active: FindHome's wait never sees it end.
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(2, 2000);
    REQUIRE(clock.wait_for_waiters(1, 2s));  // the body watches its packet
    alpacacore::vendor::celestron::set_guide_end_read_hook_for_testing(
        *driver, [&](std::optional<std::chrono::milliseconds> timeout) { gate.hold(timeout); });

    struct Read {
        alpacacore::util::TaskClock::clock::time_point at;
        std::optional<std::chrono::milliseconds> timeout;
    };
    std::vector<Read> reads;
    const auto wait_start = clock.now();
    const auto deadline = wait_start + 3050ms;  // kGuidePacketMax + kGuideEndMargin
    std::uint64_t waits_seen = clock.wait_count();
    auto home = std::async(std::launch::async, [&] {
        try {
            return thrown_by([&] { driver->find_home(); });
        } catch (...) {
            return Thrown{-1, "not an AlpacaException"};
        }
    });
    // Each held read takes its whole timeout on the task clock, at most the 500 ms USB transport timeout: a slow
    // mount. Each 20 ms pause the wait registers between reads is stepped once. After FindHome has superseded the
    // pulse, the body returns without a wait of its own, so every new wait is the wait's pause.
    const auto watchdog = std::chrono::steady_clock::now() + 20s;
    while (home.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < watchdog) {
        if (const auto held = gate.wait_held(1ms)) {
            reads.push_back({clock.now(), *held});
            clock.advance(std::min(held->value_or(500ms), 500ms));
            gate.release();
        } else if (clock.wait_count() > waits_seen) {
            waits_seen = clock.wait_count();
            clock.advance(20ms);
        }
    }
    REQUIRE(home.wait_for(5s) == std::future_status::ready);
    const auto result = home.get();
    alpacacore::vendor::celestron::set_guide_end_read_hook_for_testing(*driver, {});
    mount.complete_axis_guide(0x10);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - wait_start);
    INFO(result.text << "; task clock " << elapsed.count() << " ms after the wait started, " << reads.size()
                     << " reads");
    CHECK(result.code == alpacacore::AlpacaError::DriverException);
    CHECK(result.text.find("guide packet active") != std::string::npos);
    CHECK(elapsed <= 3050ms);
    CHECK_FALSE(reads.empty());
    for (const Read& read : reads) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - read.at);
        INFO("read at " << std::chrono::duration_cast<std::chrono::milliseconds>(read.at - wait_start).count()
                        << " ms, timeout " << (read.timeout ? std::to_string(read.timeout->count()) : "transport"));
        CHECK(read.at < deadline);
        CHECK(read.timeout.has_value());
        if (read.timeout) CHECK(*read.timeout <= left);
    }
}

#endif  // !_WIN32

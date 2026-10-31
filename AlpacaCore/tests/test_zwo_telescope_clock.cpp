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

// ZWO telescope waits on the TaskClock (decision 0005): every timed wait and deadline in
// zwo_telescope_driver.cpp reads the injected clock. Each case drives one wait through
// FakeZwoMount and FakeTaskClock::advance() and never sleeps on the clock under test; a wait
// that fell back to the host clock would not move when the test advances, and the case fails.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::TelescopeDriver;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZwoMount;
using alpacacore::test::require_alpaca_error;
using namespace std::chrono_literals;

constexpr auto kReal = 5s;  // real-time bound on a rendezvous, never a driver timer

struct Rig {
    FakeTaskClock clock;
    FakeZwoMount mount{clock};
    std::unique_ptr<TelescopeDriver> driver;
    std::list<std::future<void>> calls;  // callers started with call(); finished before the driver is destroyed

    Rig() {
        alpacacore::vendor::zwo::ConnectionInfo info;
        info.type = alpacacore::vendor::zwo::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = mount.port();
        info.response_timeout_ms = 500;
        driver = alpacacore::vendor::zwo::create_zwo_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, false,
                                                                         clock);
    }
    // A failed case can leave a caller blocked in a clock wait; advance until every caller has returned, so the
    // teardown never hangs on a future that waits for virtual time.
    ~Rig() {
        mount.release();
        for (auto& call : calls) {
            for (int i = 0; i < 600 && call.valid() && call.wait_for(10ms) != std::future_status::ready; ++i) {
                clock.advance(1min);
            }
        }
        driver.reset();
    }
    // Runs a driver call on its own thread.
    std::future<void>& call(std::function<void()> fn) {
        calls.push_back(std::async(std::launch::async, std::move(fn)));
        return calls.back();
    }
    void connect() {
        REQUIRE(mount.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        REQUIRE(clock.wait_for_waiters(1, kReal));  // the poll loop
    }
    // Advance virtual time, then let every woken waiter finish its step.
    void advance(std::chrono::nanoseconds d) {
        clock.advance(d);
        REQUIRE(clock.wait_for_woken_settled(kReal));
    }
    // A GOTO's setup thread pauses the poll loop until the mount has answered ":MS". Real time, not virtual,
    // decides when that happens (a wire event, not a driver timer), so step the 50 ms paused poll until it reads
    // status again.
    void await_poll_resumed() {
        const int base = mount.count(":GU");
        for (int i = 0; i < 2000 && !mount.wait_for_count(":GU", base + 1, 1ms); ++i) advance(50ms);
        REQUIRE(mount.count(":GU") > base);
    }
    // Steps virtual time until @p done, at most @p limit.
    void advance_until(const std::function<bool()>& done, std::chrono::nanoseconds step,
                       std::chrono::nanoseconds limit) {
        const auto start = clock.now();
        while (!done() && clock.now() - start < limit) advance(step);
    }
    // A caller blocked in a clock wait, besides @p others clock waiters (the poll loop; during a pulse also the
    // pulse worker).
    void wait_for_blocked_caller(std::size_t others = 1) { REQUIRE(clock.wait_for_waiters(others + 1, kReal)); }
};

template <class Future>
bool ready(Future& f, std::chrono::milliseconds real = 50ms) {
    return f.wait_for(real) == std::future_status::ready;
}

}  // namespace

TEST_CASE("ZWO telescope clock - connect retries the guide-rate read after 150 ms on the task clock",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.mount.override_reply(":Ggr", "e1", 1);
    REQUIRE(rig.mount.ok());
    auto& connect = rig.call([&] { rig.driver->set_connected(true); });
    REQUIRE(rig.mount.wait_for_count(":Ggr", 1, kReal));
    REQUIRE(rig.clock.wait_for_waiters(1, kReal));
    CHECK_FALSE(ready(connect));
    rig.clock.advance(149ms);
    CHECK_FALSE(ready(connect));
    CHECK(rig.mount.count(":Ggr") == 1);
    rig.clock.advance(1ms);
    REQUIRE(ready(connect, kReal));
    connect.get();
    CHECK(rig.mount.count(":Ggr") == 2);
}

TEST_CASE("ZWO telescope clock - the poll loop reads status every 250 ms and the mount time every 30 s",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.advance(250ms);
    const int status = rig.mount.count(":GU");
    rig.advance(249ms);
    CHECK(rig.mount.count(":GU") == status);
    rig.advance(1ms);
    CHECK(rig.mount.wait_for_count(":GU", status + 1, kReal));

    const int times = rig.mount.count(":GMTI");
    for (int i = 0; i < 5; ++i) rig.advance(5s);  // 25 s
    CHECK(rig.mount.count(":GMTI") == times);
    rig.advance(5s);  // 30 s since the poll loop started
    CHECK(rig.mount.wait_for_count(":GMTI", times + 1, kReal));
}

TEST_CASE("ZWO telescope clock - IsPulseGuiding holds for the pulse plus 500 ms and the poll pauses meanwhile",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 250);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));  // the worker stamped the pulse end at virtual time 0
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.advance(749ms);
    CHECK(rig.driver->get_is_pulse_guiding());
    const int positions = rig.mount.count(":GD");
    rig.advance(1ms);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.mount.count(":GD") == positions);  // the poll stayed paused until the pulse ended
    // The pulse body's reconciliation read ran when its one 250 ms step wait ended; the poll resumes at its next
    // 50 ms check after the pulse end and reads the position (":GD").
    rig.advance(100ms);
    rig.advance(100ms);
    CHECK(rig.mount.wait_for_count(":GD", positions + 1, kReal));
}

TEST_CASE("ZWO telescope clock - a slew ends IsPulseGuiding at once", "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.driver->find_home();  // resets the pulse end to now; no virtual time passes
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

TEST_CASE("ZWO telescope clock - Tracking on waits for an in-flight pulse that ends within 1500 ms",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);  // pauses the poll, so the cached Tracking state ages past 2 s
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    for (int i = 0; i < 22; ++i) rig.advance(100ms);  // 2200 ms; the pulse ends at 3500 ms
    const int enables = rig.mount.count(":Te");
    auto& write = rig.call([&] { rig.driver->set_tracking(true); });
    rig.wait_for_blocked_caller(2);  // the poll loop and the pulse worker wait on the clock too
    CHECK_FALSE(ready(write));
    for (int i = 0; i < 12; ++i) rig.advance(100ms);  // 1200 ms into the wait
    CHECK_FALSE(ready(write));
    CHECK(rig.mount.count(":Te") == enables);
    for (int i = 0; i < 4 && !ready(write); ++i) rig.advance(100ms);  // the pulse ends 1300 ms in
    REQUIRE(ready(write, kReal));
    write.get();
    CHECK(rig.mount.count(":Te") == enables + 1);
}

TEST_CASE("ZWO telescope clock - Tracking on stops waiting for a longer pulse after 1500 ms",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 6000);  // ends at 6500 ms, past the wait's 1500 ms cap
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    for (int i = 0; i < 22; ++i) rig.advance(100ms);  // 2200 ms: the cached Tracking state is older than 2 s
    const int enables = rig.mount.count(":Te");
    auto& write = rig.call([&] { rig.driver->set_tracking(true); });
    rig.wait_for_blocked_caller(2);  // the poll loop and the pulse worker wait on the clock too
    // The wait sleeps in 200 ms steps, so the cap lands on the first step at or after 1500 ms: 1600 ms.
    for (int i = 0; i < 14; ++i) rig.advance(100ms);  // 1400 ms into the wait
    CHECK_FALSE(ready(write));
    CHECK(rig.mount.count(":Te") == enables);
    rig.advance(100ms);
    rig.advance(100ms);  // 1600 ms
    REQUIRE(ready(write, kReal));
    write.get();
    CHECK(rig.mount.count(":Te") == enables + 1);
    CHECK(rig.driver->get_is_pulse_guiding());  // sent while the pulse was still running
}

TEST_CASE("ZWO telescope clock - SlewToCoordinates polls every 200 ms, then sleeps the settle time",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(6s);  // longer than the 5 s forced window
    rig.driver->set_slew_settle_time(3);
    const auto start = rig.clock.now();
    auto& slew = rig.call([&] { rig.driver->slew_to_coordinates(6.0, 30.0); });
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();
    rig.wait_for_blocked_caller();  // the wait loop's 200 ms sleep is a clock wait, not a host sleep
    std::chrono::milliseconds elapsed = 0ms;
    while (!ready(slew, 5ms) && elapsed < 12s) {
        rig.advance(50ms);
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(rig.clock.now() - start);
    }
    REQUIRE(ready(slew, kReal));
    slew.get();
    CHECK(elapsed >= 9000ms);  // 6 s slew + 3 s settle
    CHECK(elapsed <= 9800ms);  // plus the poll and 200 ms granularity
}

TEST_CASE("ZWO telescope clock - a synchronous slew that never settles times out at 15 minutes",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(60min);  // the mount reports slewing for the whole 15 minutes
    const auto start = rig.clock.now();
    auto& slew = rig.call([&] { rig.driver->slew_to_coordinates(6.0, 30.0); });
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();
    rig.wait_for_blocked_caller();
    // Steps stay under the 5 s snapshot age so the poll keeps the frame fresh (a coarser step lets the wait time out
    // early).
    for (auto left = 899s - (rig.clock.now() - start); left > 0s; left = 899s - (rig.clock.now() - start)) {
        rig.advance(std::min<std::chrono::nanoseconds>(4s, left));
    }
    CHECK_FALSE(ready(slew));
    rig.advance(1s);
    rig.advance(1s);
    REQUIRE(ready(slew, kReal));
    try {
        slew.get();
        FAIL("the synchronous slew returned instead of timing out");
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::DriverException);
        CHECK(std::string(e.what()).find("timed out") != std::string::npos);
    }
}

TEST_CASE("ZWO telescope clock - a GOTO refused as busy backs off 300 ms on the task clock",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.reject_goto("e3", 1);
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(299ms);
    CHECK(rig.mount.count(":MS") == 1);
    rig.advance(1ms);
    CHECK(rig.mount.wait_for_count(":MS", 2, kReal));
    CHECK(rig.mount.count(":Q") >= 1);  // the busy mount was told to stop first
}

TEST_CASE("ZWO telescope clock - disconnect cancels the busy back-off without advancing the clock",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.reject_goto("e3");
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.wait_for_blocked_caller();
    auto& disconnect = rig.call([&] { rig.driver->set_connected(false); });
    REQUIRE(ready(disconnect, kReal));
    disconnect.get();
    CHECK(rig.mount.count(":MS") == 1);
}

TEST_CASE("ZWO telescope clock - a sync refused while the mount settles retries after 200 ms",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 1);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, 30.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(199ms);
    CHECK(rig.mount.count(":SMMC") == 1);
    rig.advance(1ms);
    REQUIRE(ready(sync, kReal));
    sync.get();
    CHECK(rig.mount.count(":SMMC") == 2);
}

TEST_CASE("ZWO telescope clock - a sync refused twice falls back to :CM after a further 300 ms",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, 30.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(200ms);
    REQUIRE(rig.mount.wait_for_count(":SMMC", 2, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(299ms);
    CHECK(rig.mount.count(":CM") == 0);
    rig.advance(1ms);
    REQUIRE(ready(sync, kReal));
    sync.get();
    CHECK(rig.mount.count(":CM") == 1);
}

TEST_CASE("ZWO last-resort Sync fences an older GOTO and serializes its target writes",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.mount.reject_goto("e3", 1);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.mount.wait_for_count(":Q", 1, kReal));
    REQUIRE(rig.clock.wait_for_waiters(2, kReal));
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance_until([&] { return rig.mount.count(":SMMC") == 2; }, 200ms, 1200ms);
    REQUIRE(rig.mount.wait_for_count(":SMMC", 2, kReal));
    rig.wait_for_blocked_caller();
    rig.mount.hold_reply(":Sr");
    rig.clock.advance(300ms);  // do not wait for the caller to settle while its wire reply is held
    REQUIRE(rig.mount.wait_until_held(kReal));
    auto& setter = rig.call([&] { rig.driver->set_target_right_ascension(7.0); });
    const bool finished_early = ready(setter);
    CHECK(rig.driver->get_target_right_ascension() == 6.0);
    rig.mount.release();
    REQUIRE(ready(setter, kReal));
    REQUIRE(ready(sync, kReal));
    setter.get();
    sync.get();
    CHECK_FALSE(finished_early);
    CHECK(rig.mount.count(":CM") == 1);
    CHECK(rig.mount.count(":MS") == 1);  // only the rejected attempt, never an old retry
}

TEST_CASE("ZWO a new GOTO during Sync settling keeps its own target across a retry",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(200ms);
    REQUIRE(rig.mount.wait_for_count(":SMMC", 2, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(100ms);  // Sync's last-resort deadline is 200 ms away; the new GOTO's retry will be later.
    rig.mount.reject_goto("e3", 1);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    REQUIRE(rig.mount.wait_for_count(":Q", 3, kReal));
    REQUIRE(rig.clock.wait_for_waiters(3, kReal));
    rig.advance(200ms);
    REQUIRE(ready(sync, kReal));
    sync.get();
    CHECK(rig.mount.count(":MS") == 1);
    rig.advance(100ms);
    REQUIRE(rig.mount.wait_for_count(":MS", 2, kReal));
    rig.driver->abort_slew();
    std::string ra, dec;
    for (const auto& command : rig.mount.commands()) {
        if (command.starts_with(":Sr")) ra = command;
        if (command.starts_with(":Sd")) dec = command;
        if (command == ":CM") {
            CHECK(ra == ":Sr06:00:00");
            CHECK(dec == ":Sd-15:00:00");
        }
        if (command == ":MS") {
            CHECK(ra == ":Sr05:30:00");
            CHECK(dec == ":Sd+20:00:00");
        }
    }
    CHECK(rig.mount.count(":CM") == 1);
}

TEST_CASE("ZWO an older Sync never aborts a GOTO accepted while it settled",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();  // Sync sent its first ":Q" and waits out the 200 ms settle
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();  // the GOTO is accepted: it owns the motion now
    const int stops_before = rig.mount.count(":Q");
    rig.advance_until([&] { return ready(sync); }, 200ms, 3s);
    REQUIRE(ready(sync, kReal));
    CHECK(rig.mount.count(":Q") == stops_before);  // the older Sync stopped nothing
    CHECK(rig.mount.count(":SMMC") == 1);          // nor retried its sync over the GOTO
    CHECK(rig.mount.count(":CM") == 0);
    CHECK(rig.driver->get_slewing());  // the GOTO still runs
    require_alpaca_error([&] { sync.get(); }, AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO an older Sync sends no last-resort :CM once a newer GOTO is accepted",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(200ms);
    REQUIRE(rig.mount.wait_for_count(":SMMC", 2, kReal));
    rig.wait_for_blocked_caller();  // Sync sent its last-resort ":Q" and waits out the 300 ms settle
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();  // the GOTO is accepted: it owns the motion now
    rig.advance(300ms);
    REQUIRE(ready(sync, kReal));
    CHECK(rig.mount.count(":Sr06:00:00") == 0);  // the GOTO's target registers stay its own
    CHECK(rig.mount.count(":CM") == 0);
    CHECK(rig.driver->get_slewing());  // the GOTO still runs
    require_alpaca_error([&] { sync.get(); }, AlpacaError::InvalidOperation);
}

TEST_CASE("ZWO SyncToCoordinates sends its own pair, not a target set earlier",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.driver->set_target_right_ascension(3.0);
    rig.driver->set_target_declination(10.0);
    rig.driver->sync_to_coordinates(6.0, -15.0);
    bool saw_pair = false;
    for (const auto& command : rig.mount.commands()) {
        if (command.starts_with(":SMMC")) {
            CHECK(command.starts_with(":SMMC06:00:00&-15"));
            saw_pair = true;
        }
    }
    CHECK(saw_pair);
}

TEST_CASE("ZWO telescope clock - FindHome publishes completion only when the forced slew window ends",
          "[zwo][telescope][unit][clock]") {
    const auto duration = GENERATE(0ms, 6000ms);
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(duration);
    rig.advance(250ms);
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->find_home();
    if (duration > 0ms) rig.mount.override_reply(":GU", "NHG", 3);  // stale home/stopped startup replies
    rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_home());
    for (int i = 0; i < 17; ++i) rig.advance(250ms);  // 4.5 s since the command
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_home());
    for (int i = 0; i < 4; ++i) rig.advance(250ms);  // 5.5 s
    CHECK(rig.driver->get_slewing() == (duration > 5500ms));
    CHECK(rig.driver->get_at_home() == (duration <= 5500ms));
    for (int i = 0; i < 4; ++i) rig.advance(250ms);  // 6.5 s
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_at_home());
}

TEST_CASE("ZWO telescope clock - MoveAxis reports Slewing for the 2 s forced window after the jog stops",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.advance(250ms);
    rig.driver->move_axis(0, 1.0);
    rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
    rig.driver->move_axis(0, 0.0);
    rig.mount.set_slew_duration(0ms);
    for (int i = 0; i < 6; ++i) rig.advance(250ms);  // 1.5 s
    CHECK(rig.driver->get_slewing());
    for (int i = 0; i < 4; ++i) rig.advance(250ms);  // 2.5 s
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO telescope clock - moving status cannot restore AtHome before the final pair",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->find_home();
    rig.mount.override_reply(":GU", "HG");  // home flag lags behind the moving flag
    rig.mount.hold_reply(":GR");
    rig.clock.advance(250ms);
    REQUIRE(rig.mount.wait_until_held(kReal));
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_home());
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(kReal));
    CHECK_FALSE(rig.driver->get_at_home());
}

TEST_CASE("ZWO telescope clock - an active jog suppresses a stale Home flag after the guard",
          "[zwo][telescope][unit][clock]") {
    const int axis = GENERATE(0, 1);
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    rig.driver->find_home();
    for (int i = 0; i < 20; ++i) rig.advance(250ms);
    REQUIRE(rig.driver->get_at_home());
    rig.driver->move_axis(axis, 1.0);
    rig.mount.override_reply(":GU", "NHG");          // stopped/home telemetry lags behind the commanded jog
    for (int i = 0; i < 9; ++i) rig.advance(250ms);  // beyond the 2 s startup guard
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_home());
    rig.driver->move_axis(axis, 0.0);
}

TEST_CASE("ZWO telescope clock - Home completion needs a final pair after the startup guard",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    rig.driver->find_home();
    for (int i = 0; i < 19; ++i) rig.advance(250ms);  // 4.75 s
    rig.mount.hold_reply(":GR");
    rig.clock.advance(250ms);  // the startup guard expires while the final pair is held
    REQUIRE(rig.mount.wait_until_held(kReal));
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_home());
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(kReal));
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_at_home());
}

TEST_CASE("ZWO telescope clock - pending Park suppresses a stale Home flag after the guard",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    rig.driver->find_home();
    for (int i = 0; i < 20; ++i) rig.advance(250ms);
    REQUIRE(rig.driver->get_at_home());
    rig.mount.override_reply(":GU", "NHG");
    rig.mount.override_reply(":Gps", "1");
    rig.driver->park();
    for (int i = 0; i < 19; ++i) rig.advance(250ms);  // 4.75 s since Park
    rig.mount.hold_reply(":Gps");
    rig.clock.advance(250ms);  // stopped mandatory pair accepted, Park evaluation still pending
    REQUIRE(rig.mount.wait_until_held(kReal));
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_at_park());
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(kReal));
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO telescope clock - SlewToCoordinatesAsync reports Slewing for the 5 s forced window and not after",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);  // the mount reports idle at once
    const auto start = rig.clock.now();
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();
    rig.advance_until([&] { return rig.clock.now() - start >= 4750ms; }, 50ms, 5s);
    CHECK(rig.driver->get_slewing());
    rig.advance_until([&] { return rig.clock.now() - start >= 5500ms; }, 50ms, 1s);
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO telescope clock - Park publishes completion only when the forced slew window ends",
          "[zwo][telescope][unit][clock]") {
    const auto duration = GENERATE(0ms, 6000ms);
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(duration);
    rig.advance(250ms);
    rig.driver->park();
    if (duration > 0ms) {
        rig.mount.override_reply(":GU", "NG", 3);  // firmware has not published motion yet
        rig.mount.override_reply(":Gps", "2", 3);
    }
    rig.advance(250ms);
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());
    for (int i = 0; i < 17; ++i) rig.advance(250ms);  // 4.5 s since the command
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
    for (int i = 0; i < 4; ++i) rig.advance(250ms);  // 5.5 s
    CHECK(rig.driver->get_slewing() == (duration > 5500ms));
    CHECK(rig.driver->get_at_park() == (duration <= 5500ms));
    for (int i = 0; i < 4; ++i) rig.advance(250ms);  // 6.5 s
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_at_park());
}

TEST_CASE("ZWO telescope clock - inferred Park completion waits for the forced slew window",
          "[zwo][telescope][unit][clock]") {
    const auto reply = GENERATE("0", "1", "x", "");
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    if (*reply) {
        rig.mount.override_reply(":Gps", reply);  // unavailable completion indication, stationary mount
    } else {
        rig.mount.go_silent(":Gps");
    }
    rig.advance(250ms);
    rig.driver->park();
    for (int i = 0; i < 7; ++i) rig.advance(250ms);  // 1.75 s since the command
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());  // the park command is still active
    rig.advance(250ms);
    rig.advance(250ms);  // 2.25 s
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());
    for (int i = 0; i < 13; ++i) rig.advance(250ms);  // 5.5 s
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO telescope clock - Unpark reports Slewing for the 2 s forced window and not after",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.mount.set_park_state(2);
    rig.connect();
    rig.advance(250ms);
    REQUIRE(rig.driver->get_at_park());
    rig.driver->unpark();
    for (int i = 0; i < 7; ++i) rig.advance(250ms);  // 1.75 s since the command
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
    for (int i = 0; i < 2; ++i) rig.advance(250ms);  // 2.25 s
    CHECK_FALSE(rig.driver->get_slewing());
}

// Operation slot, pulse family: the body waits a pulse step as one clock wait that a cancel wakes, not as 100 ms
// chunks. One advance over the whole step therefore ends the step and the body reconciles against the mount.
// Falsified by: run_pulse_task() waiting the pulse in 100 ms clock_.wait_for chunks (pulse_cv_), so one advance
// wakes one chunk and the reconciliation read (":GD") does not follow.
TEST_CASE("ZWO telescope clock - a pulse step is one clock wait, so one advance ends it",
          "[zwo][telescope][unit][clock][operation-slot]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 1000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    const int positions = rig.mount.count(":GD");
    rig.wait_for_blocked_caller();  // the pulse body, besides the poll loop
    rig.advance(1000ms);
    // Real-time bound on a wire event (the reconciliation read), not a driver timer.
    CHECK(rig.mount.wait_for_count(":GD", positions + 1, 500ms));
}

// PIN (passes on the current code; must keep passing once the pulse body moves to an operation slot): an accepted
// PulseGuide is never dropped because a slew superseded the pulse before it. Pulse, slew mid-step, pulse again:
// the second pulse still reaches the mount.
// Falsified by: pulse_guide() leaving a pulse queued behind a superseded drain body that never sends it.
TEST_CASE("ZWO telescope clock - a pulse queued after a slew superseded the previous pulse is still sent (PIN)",
          "[zwo][telescope][unit][clock][operation-slot]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    rig.driver->slew_to_coordinates_async(6.0, 30.0);  // mid-step: supersedes the pulse and clears its queue
    CHECK(rig.mount.wait_for_count(":MS", 1, kReal));  // the slew reached the mount
    rig.driver->pulse_guide(1, 500);
    // Virtual time only: the superseded body wakes and the new pulse is drained.
    rig.advance_until([&] { return rig.mount.count(":Mg") >= 2; }, 100ms, 10s);
    CHECK(rig.mount.wait_for_count(":Mg", 2, kReal));
}

// Round 2 finding 3: the flag that says a pulse drain runs is cleared on disconnect and on connect, so a pulse after
// a reconnect starts a new drain and reaches the mount.
// Falsified by: zwo_telescope_driver.cpp stop_pulse_drain() and the connect reset (reset_session_state_for_connect)
// both keeping pulse_drain_active_ (the two clears removed together), so the pulse after the reconnect waits for a
// drain that no longer runs and no ":Mg" is sent.
TEST_CASE("ZWO telescope clock - a pulse after a reconnect reaches the mount",
          "[zwo][telescope][unit][clock][operation-slot]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    rig.driver->set_connected(false);  // the teardown runs on: the pulse body is still being stopped
    require_alpaca_error([&] { rig.driver->pulse_guide(2, 500); }, AlpacaError::NotConnected);
    rig.connect();
    const int pulses = rig.mount.count(":Mg");
    rig.driver->pulse_guide(2, 500);
    CHECK(rig.mount.wait_for_count(":Mg", pulses + 1, kReal));
}

#endif  // !_WIN32

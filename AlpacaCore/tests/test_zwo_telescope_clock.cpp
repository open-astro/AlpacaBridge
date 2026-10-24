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
#include <future>
#include <memory>
#include <thread>

#include "catch2_compat.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"

namespace {

using alpacacore::AlpacaException;
using alpacacore::TelescopeDriver;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZwoMount;
using namespace std::chrono_literals;

constexpr auto kReal = 5s;  // real-time bound on a rendezvous, never a driver timer

struct Rig {
    FakeTaskClock clock;
    FakeZwoMount mount{clock};
    std::unique_ptr<TelescopeDriver> driver;

    Rig() {
        alpacacore::vendor::zwo::ConnectionInfo info;
        info.type = alpacacore::vendor::zwo::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = mount.port();
        info.response_timeout_ms = 500;
        driver = alpacacore::vendor::zwo::create_zwo_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, false,
                                                                         clock);
    }
    ~Rig() {
        mount.release();
        driver.reset();
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
    // decides when that happens, so advance in small steps until the poll reads status again.
    void await_poll_resumed() {
        const int base = mount.count(":GU");
        for (int i = 0; i < 2000 && mount.count(":GU") == base; ++i) {
            advance(50ms);
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(mount.count(":GU") > base);
    }
    // A caller blocked in a clock wait, besides the poll loop.
    void wait_for_blocked_caller() { REQUIRE(clock.wait_for_waiters(2, kReal)); }
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
    auto connect = std::async(std::launch::async, [&] { rig.driver->set_connected(true); });
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
    // The worker finishes its 100 ms hold steps, then parks on its own condition variable (not a clock wait,
    // so no settle rendezvous); the reconciliation read and the resumed poll both send ":GD".
    for (int i = 0; i < 200 && rig.mount.count(":GD") == positions; ++i) {
        rig.clock.advance(100ms);
        std::this_thread::sleep_for(2ms);
    }
    CHECK(rig.mount.count(":GD") > positions);
}

TEST_CASE("ZWO telescope clock - Tracking on waits out an in-flight pulse for at most 1500 ms",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);  // pauses the poll, so the cached Tracking state ages past 2 s
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    for (int i = 0; i < 22; ++i) rig.advance(100ms);  // 2200 ms; the pulse ends at 3500 ms
    const int enables = rig.mount.count(":Te");
    auto write = std::async(std::launch::async, [&] { rig.driver->set_tracking(true); });
    rig.wait_for_blocked_caller();
    CHECK_FALSE(ready(write));
    for (int i = 0; i < 12; ++i) rig.advance(100ms);  // 1200 ms into the wait
    CHECK_FALSE(ready(write));
    CHECK(rig.mount.count(":Te") == enables);
    for (int i = 0; i < 4 && !ready(write); ++i) rig.advance(100ms);  // the pulse ends 1300 ms in
    REQUIRE(ready(write, kReal));
    write.get();
    CHECK(rig.mount.count(":Te") == enables + 1);
}

TEST_CASE("ZWO telescope clock - SlewToCoordinates polls every 200 ms, then sleeps the settle time",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(6s);  // longer than the 5 s forced window
    rig.driver->set_slew_settle_time(3);
    const auto start = rig.clock.now();
    auto slew = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(6.0, 30.0); });
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
    auto slew = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(6.0, 30.0); });
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();
    // Steps stay under the 5 s snapshot age so the poll keeps the frame fresh (a coarser step lets the wait time out
    // early).
    for (auto left = 899s - (rig.clock.now() - start); left > 0s; left = 899s - (rig.clock.now() - start)) {
        rig.advance(std::min<std::chrono::nanoseconds>(4s, left));
    }
    CHECK_FALSE(ready(slew));
    rig.advance(1s);
    rig.advance(1s);
    REQUIRE(ready(slew, kReal));
    CHECK_THROWS_AS(slew.get(), AlpacaException);
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
    auto disconnect = std::async(std::launch::async, [&] { rig.driver->set_connected(false); });
    REQUIRE(ready(disconnect, kReal));
    disconnect.get();
    CHECK(rig.mount.count(":MS") == 1);
}

TEST_CASE("ZWO telescope clock - a sync refused while the mount settles retries after 200 ms",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 1);
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(6.0, 30.0); });
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
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(6.0, 30.0); });
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

TEST_CASE("ZWO telescope clock - FindHome reports Slewing for the 5 s forced window and not after",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);  // the mount itself reports idle at once
    rig.advance(250ms);
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->find_home();
    rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
    for (int i = 0; i < 17; ++i) rig.advance(250ms);  // 4.5 s since the command
    CHECK(rig.driver->get_slewing());
    for (int i = 0; i < 4; ++i) rig.advance(250ms);  // 5.5 s
    CHECK_FALSE(rig.driver->get_slewing());
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

#endif  // !_WIN32

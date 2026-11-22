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
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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
    FakeZwoMount mount;
    std::unique_ptr<TelescopeDriver> driver;
    std::list<std::future<void>> calls;  // callers started with call(); finished before the driver is destroyed

    explicit Rig(std::string model = "AM5N") : mount(clock, std::move(model)) {
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

// open-astro#2094, #2263: a MoveAxis (stop or jog) ends the queued and running guide pulses on its own axis: after
// it no further ":Mg" on that axis is sent and IsPulseGuiding reads false.
// Falsified by: zwo_telescope_driver.cpp move_axis() not ending its own axis's pulses after fence_goto().
TEST_CASE("ZWO motion owner - MoveAxis ends a running guide pulse on its own axis (#2094, #2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const double rate = GENERATE(0.0, 1.0);
    const int axis = GENERATE(0, 1);
    CAPTURE(rate, axis);
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(axis == 0 ? 2 : 0, 3000);
    rig.driver->pulse_guide(axis == 0 ? 3 : 1, 500);  // queued behind the running pulse
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.driver->move_axis(axis, rate);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    for (int i = 0; i < 10; ++i) rig.advance(500ms);
    CHECK(rig.mount.count(":Mg") == 1);
    if (rate != 0.0) rig.driver->move_axis(axis, 0.0);
}

// #2415: ZWO's MoveAxis stop is |rate| < 1e-9, strictly under. A rate of 1e-9 starts a jog; 1e-10 and the largest
// double under 1e-9 stop the axis. Green before and after the threshold moved into the gate limits; it pins that the
// move doesn't change where the stop starts.
// Falsified by: zwo_telescope_driver.cpp kCommandLimits.move_axis_stop_epsilon set to 1e-9 (1e-9 becomes a stop), or
// left at 0.0 once move_axis() decides the stop through util::check_move_axis() (1e-10 becomes a jog).
TEST_CASE("ZWO telescope - MoveAxis treats 1e-9 as a move and a smaller rate as a stop (#2415)",
          "[zwo][telescope][unit][clock][moveaxis]") {
    const double stop_rate = GENERATE(1e-10, -1e-10, std::nextafter(1e-9, 0.0));
    CAPTURE(stop_rate);
    Rig rig;
    rig.connect();
    rig.driver->move_axis(0, 1e-9);
    CHECK(rig.mount.count(":Me") == 1);
    CHECK(rig.mount.jogging());
    CHECK(rig.mount.count(":Qe") == 0);
    rig.driver->move_axis(0, stop_rate);
    CHECK(rig.mount.count(":Qe") == 1);
    CHECK_FALSE(rig.mount.jogging());
    CHECK(rig.mount.count(":Me") == 1);  // the stop started no jog
}

// #2263: a command owns only the axes it moves. A MoveAxis (stop or jog) on one axis leaves a guide pulse on the other
// axis running: IsPulseGuiding stays true, the pulse's second step still goes out, and its position offset is kept.
// Falsified by: zwo_telescope_driver.cpp move_axis() ending the pulses on both axes (kBothAxes in place of its own).
TEST_CASE("ZWO motion owner - MoveAxis on one axis leaves a pulse on the other axis running (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const double rate = GENERATE(0.0, 1.0);
    const int axis = GENERATE(0, 1);  // the MoveAxis axis; the pulse is on the other one
    CAPTURE(rate, axis);
    Rig rig;
    rig.connect();
    rig.advance(250ms);
    const int direction = axis == 0 ? 0 : 2;  // North (Dec) during an RA MoveAxis, East (RA) during a Dec one
    const std::string guide = axis == 0 ? ":Mgn" : ":Mge";
    rig.driver->pulse_guide(direction, 5000);  // two steps: 3000 ms, then 2000 ms
    REQUIRE(rig.mount.wait_for_count(guide, 1, kReal));
    const double guided = axis == 0 ? rig.driver->get_declination() : rig.driver->get_right_ascension();
    rig.driver->move_axis(axis, rate);
    CHECK(rig.driver->get_is_pulse_guiding());
    CHECK((axis == 0 ? rig.driver->get_declination() : rig.driver->get_right_ascension()) == guided);
    rig.wait_for_blocked_caller();  // the drain waits out the first step, besides the poll loop
    rig.advance(3000ms);
    CHECK(rig.mount.wait_for_count(guide, 2, kReal));  // the second step reached the mount
    CHECK(rig.driver->get_is_pulse_guiding());
    // The stop's tracking restore waits on the clock for the running pulse; the Rig advances until it returns.
    if (rate != 0.0) rig.call([&] { rig.driver->move_axis(axis, 0.0); });
}

// #2263: a MoveAxis on one axis waits only for a ":Mg" on its own axis (#2072). A North step held between its check
// and its send does not hold up an RA jog.
// Falsified by: zwo_telescope_driver.cpp end_pulses(): waiting for the sends in flight on both axes, so the RA jog
// waits for the held Dec ":Mg".
TEST_CASE("ZWO motion owner - MoveAxis does not wait for the other axis's guide pulse send (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    alpacacore::test::Latch latch;
    std::atomic<int> sends{0};
    Rig rig;
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == ":Mg" && sends.fetch_add(1) == 0) latch.block();
    });
    rig.connect();
    rig.driver->pulse_guide(0, 1000);
    REQUIRE(latch.wait_entered());
    auto& jog = rig.call([&] { rig.driver->move_axis(0, 1.0); });
    const bool returned = ready(jog, 1000ms);  // a rendezvous on the wire, within the latch's 2 s self-release
    latch.release();
    REQUIRE(ready(jog, kReal));
    jog.get();
    CHECK(returned);
    rig.call([&] { rig.driver->move_axis(0, 0.0); });  // its tracking restore waits on the clock for the Dec pulse
}

// #2263: the per-axis split. An East and a North pulse run at the same time, each on its own axis, rather than one
// after the other: both ":Mg" go out at once and IsPulseGuiding ends 500 ms after the longer one.
// Falsified by: zwo_telescope_driver.cpp pulse_guide() queueing every pulse on axis 0 (one drain for both axes), so
// the North pulse waits for the East one.
TEST_CASE("ZWO motion owner - an RA and a Dec pulse run at the same time (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(2, 1000);
    rig.driver->pulse_guide(0, 1000);
    CHECK(rig.mount.wait_for_count(":Mge", 1, kReal));
    CHECK(rig.mount.wait_for_count(":Mgn", 1, kReal));
    rig.advance(1499ms);
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.advance(1ms);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// #2263: a pulse on one axis that ends while the other axis jogs reconciles only its own axis. The jogging axis reads
// the mount once the poll resumes, instead of the position it had when the pulse started.
// Falsified by: zwo_telescope_driver.cpp run_pulse_task(): rebasing both offsets against the cached position in the
// reconciliation, so the jogged RA reads its old value.
TEST_CASE("ZWO motion owner - a pulse ending during an other-axis jog leaves that axis reading the mount (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const bool jog_first = GENERATE(true, false);
    CAPTURE(jog_first);
    Rig rig;
    rig.connect();
    rig.advance(250ms);
    REQUIRE(rig.driver->get_right_ascension() == Catch::Approx(12.0).margin(1e-6));
    if (jog_first) rig.driver->move_axis(0, 1.0);
    rig.driver->pulse_guide(0, 1000);
    REQUIRE(rig.mount.wait_for_count(":Mgn", 1, kReal));
    if (!jog_first) rig.driver->move_axis(0, 1.0);
    rig.mount.set_position(13.0, 45.0);  // the jog moved RA
    rig.wait_for_blocked_caller();       // the drain waits out its step, besides the poll loop
    const int pairs = rig.mount.count(":GR");
    rig.advance(1000ms);
    REQUIRE(rig.mount.wait_for_count(":GR", pairs + 1, kReal));  // the drain's reconciliation read
    for (int i = 0; i < 4; ++i) rig.advance(250ms);              // the pulse window ends and the poll reads again
    CHECK(rig.driver->get_right_ascension() == Catch::Approx(13.0).margin(1e-6));
    rig.driver->move_axis(0, 0.0);
}

namespace {

// The slew, park and home commands the PulseGuide refusal covers (#2263).
void start_slew(TelescopeDriver& driver, const std::string& name) {
    if (name == "GOTO") driver.slew_to_coordinates_async(6.0, 30.0);
    if (name == "FindHome" || name == "FindHome, window closed") driver.find_home();
    if (name == "Park") driver.park();
}

}  // namespace

// #2263, ITelescopeV4: a PulseGuide while a slew, park or home runs is refused with InvalidOperation and changes
// nothing: no ":Mg", IsPulseGuiding false, the motion still runs and nothing stops it. A FindHome still counts once its
// 5 s slew window has closed, while the mount reports it moving.
// Falsified by: zwo_telescope_driver.cpp pulse_guide(): never taking the slew_running() refusal branch (GOTO and
// FindHome; a Park is already refused by the Tracking check, since Park sends ":Td"); for the closed-window leg,
// find_home() claiming without its home flag.
TEST_CASE("ZWO motion owner - PulseGuide during a slew, park or home is refused and changes nothing (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const std::string slew = GENERATE(as<std::string>{}, "GOTO", "FindHome", "FindHome, window closed", "Park");
    CAPTURE(slew);
    Rig rig("AM5");
    rig.connect();
    rig.mount.set_slew_duration(30s);
    start_slew(*rig.driver, slew);
    if (slew == "GOTO") {
        REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
        rig.await_poll_resumed();
    }
    if (slew == "FindHome, window closed") {
        for (int i = 0; i < 24; ++i) rig.advance(250ms);  // 6 s: the window closed, the mount still homes
    }
    REQUIRE(rig.driver->get_slewing());
    const auto before = rig.mount.commands().size();
    require_alpaca_error([&] { rig.driver->pulse_guide(0, 1000); }, AlpacaError::InvalidOperation);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    for (int i = 0; i < 8; ++i) rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount.count(":Mg") == 0);
    const auto sent = rig.mount.commands();
    for (std::size_t i = before; i < sent.size(); ++i) {
        INFO("after the refusal: " << sent[i]);
        CHECK(sent[i].rfind(":Q", 0) != 0);
    }
}

namespace {

// The slew, park or home commands by their initiator seam name, for the claim-gap cases (#2263).
void start_slew_command(TelescopeDriver& driver, const std::string& name) {
    if (name == "SlewToTargetAsync") driver.slew_to_coordinates_async(6.0, 30.0);
    if (name == "FindHome") driver.find_home();
    if (name == "Park") driver.park();
}

}  // namespace

// #2263: a slew, park or home counts as running from its claim, not only once it has set its own state (goto_active_,
// park_command_active_, the slew window). The initiator seam runs a PulseGuide in that gap, right after the claim:
// before Park's ":GU" and ":Td", before FindHome arms its window, and where a SlewAsync waits for initiator_mutex_. It
// is refused and no ":Mg" goes out.
// Falsified by: zwo_telescope_driver.cpp claim_slew() not counting the pending command, so
// slew_motion_running_locked() reads "not running" and finished() ends the claim.
TEST_CASE("ZWO motion owner - PulseGuide between a slew's claim and its own state is refused (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const std::string slew = GENERATE(as<std::string>{}, "SlewToTargetAsync", "FindHome", "Park");
    CAPTURE(slew);
    Rig rig("AM5");
    bool ran = false;  // the hook runs on the initiator's own thread
    int code = 0;
    alpacacore::vendor::zwo::set_initiator_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name != slew || ran) return;
        ran = true;
        try {
            rig.driver->pulse_guide(0, 1000);
        } catch (const AlpacaException& ex) {
            code = ex.error_code();
        }
    });
    rig.connect();
    rig.mount.set_slew_duration(30s);
    start_slew_command(*rig.driver, slew);
    CHECK(ran);
    CHECK(code == AlpacaError::InvalidOperation);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    for (int i = 0; i < 8; ++i) rig.advance(250ms);
    CHECK(rig.mount.count(":Mg") == 0);
}

// #2263: a slew, park or home whose call throws between its claim and its own state gives the claim up, so the next
// PulseGuide is accepted.
// Falsified by: zwo_telescope_driver.cpp PendingCommand ending only when its call returns (not on a throw), so
// every later PulseGuide is refused.
TEST_CASE("ZWO motion owner - a slew that throws after its claim leaves PulseGuide accepted (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const std::string slew = GENERATE(as<std::string>{}, "SlewToTargetAsync", "FindHome", "Park");
    CAPTURE(slew);
    Rig rig("AM5");
    alpacacore::vendor::zwo::set_initiator_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == slew) throw std::runtime_error("seam");
    });
    rig.connect();
    CHECK_THROWS_AS(start_slew_command(*rig.driver, slew), std::runtime_error);
    REQUIRE_NOTHROW(rig.driver->pulse_guide(0, 1000));
    CHECK(rig.mount.wait_for_count(":Mgn", 1, kReal));
    CHECK(rig.driver->get_is_pulse_guiding());
}

// #2263: the slew, park or home claim ends with the motion on every exit path, so a PulseGuide is accepted again: a
// GOTO that landed, a GOTO the mount rejected, an AbortSlew, a FindHome that landed, and a jog that took an axis from
// a GOTO (a pulse on the other axis).
// Falsified by: zwo_telescope_driver.cpp pulse_guide(): removing the motion_owner_.finished() call, so the first slew
// keeps every later PulseGuide refused.
TEST_CASE("ZWO motion owner - PulseGuide is accepted once the slew, park or home has ended (#2263)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const std::string exit = GENERATE(as<std::string>{}, "landed", "rejected", "AbortSlew", "home landed", "jog");
    CAPTURE(exit);
    Rig rig("AM5");
    rig.connect();
    rig.mount.set_slew_duration(exit == "landed" || exit == "home landed" ? 1s : 30s);
    if (exit == "rejected") rig.mount.reject_goto("2");
    if (exit == "home landed") {
        rig.driver->find_home();
    } else {
        rig.driver->slew_to_coordinates_async(6.0, 30.0);
        REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
        rig.await_poll_resumed();
    }
    if (exit == "rejected") {
        rig.advance(250ms);  // two poll cycles publish the idle mount
        rig.advance(250ms);
    }
    if (exit == "AbortSlew") rig.driver->abort_slew();
    if (exit == "jog") rig.driver->move_axis(0, 1.0);
    if (exit == "landed" || exit == "home landed") {
        rig.advance_until([&] { return !rig.driver->get_slewing(); }, 250ms, 10s);
        REQUIRE_FALSE(rig.driver->get_slewing());
    }
    REQUIRE_NOTHROW(rig.driver->pulse_guide(0, 1000));
    CHECK(rig.mount.wait_for_count(":Mgn", 1, kReal));
    CHECK(rig.driver->get_is_pulse_guiding());
    // The stop's tracking restore waits on the clock for the Dec pulse; the Rig advances until it returns.
    if (exit == "jog") rig.call([&] { rig.driver->move_axis(0, 0.0); });
}

// #2459: a jog that lands between a SlewAsync's claim and its initiator_mutex_ takes an axis from the slew claim. The
// pulse on the other axis is still refused there, and for the whole flight of the GOTO that call then starts (which
// holds no slew claim at all): the refusal reads the driver's own record, not only the claim.
// Falsified by: zwo_telescope_driver.cpp pulse_guide(): dropping the slew_commands_pending_ term from the refusal (the
// gap leg) or the own_goto_in_flight_locked() term (the flight leg).
TEST_CASE("ZWO motion owner - a jog in a GOTO's claim gap leaves PulseGuide refused while that GOTO runs (#2459)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const int jog_axis = GENERATE(0, 1);
    CAPTURE(jog_axis);
    const int direction = jog_axis == 0 ? 0 : 2;  // North during an RA jog, East during a Dec one
    Rig rig("AM5");
    bool ran = false;  // the hook runs on the slew's own thread, after its claim
    bool jogged = false;
    int code = 0;
    alpacacore::vendor::zwo::set_initiator_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name != "SlewToTargetAsync" || ran) return;
        ran = true;
        rig.driver->move_axis(jog_axis, 1.0);  // initiator_mutex_ is free: the slew takes it after this hook
        jogged = true;
        try {
            rig.driver->pulse_guide(direction, 1000);
        } catch (const AlpacaException& ex) {
            code = ex.error_code();
        }
    });
    rig.connect();
    rig.mount.set_slew_duration(30s);
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(ran);
    REQUIRE(jogged);
    CHECK(code == AlpacaError::InvalidOperation);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();
    REQUIRE(rig.driver->get_slewing());
    const auto before = rig.mount.commands().size();
    require_alpaca_error([&] { rig.driver->pulse_guide(direction, 1000); }, AlpacaError::InvalidOperation);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    for (int i = 0; i < 8; ++i) rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount.count(":Mg") == 0);
    const auto sent = rig.mount.commands();
    for (std::size_t i = before; i < sent.size(); ++i) {
        INFO("after the refusal: " << sent[i]);
        CHECK(sent[i].rfind(":Q", 0) != 0);
    }
    rig.driver->abort_slew();
}

// #2459, #2467 (maintainer, 2026-11-21: "A pulse on the axis that is jogging stays refused"): a PulseGuide on the axis
// a MoveAxis is driving is refused with InvalidOperation and changes nothing: no ":Mg", no stop, IsPulseGuiding false,
// the jog still runs. It is refused from the jog's claim, before its ":Rv" and jog command (the "MoveAxis" seam runs
// the pulse there, on the jog's own thread), not only once the jog's flag is set. A pulse on the other axis is still
// accepted, and the refusal ends with the jog.
// Falsified by: zwo_telescope_driver.cpp pulse_guide(): dropping the jog refusal (every leg), or refusing on
// manual_axis_slewing_ alone (the claim-gap leg).
TEST_CASE("ZWO motion owner - PulseGuide on the axis a MoveAxis is driving is refused and changes nothing (#2459)",
          "[zwo][telescope][unit][clock][motion-owner]") {
    const int axis = GENERATE(0, 1);
    const std::string when = GENERATE(as<std::string>{}, "jogging", "claim gap");
    CAPTURE(axis, when);
    const int direction = axis == 0 ? 2 : 0;        // East on RA, North on Dec
    const int other_direction = axis == 0 ? 0 : 2;  // North on Dec, East on RA
    const std::string guide = axis == 0 ? ":Mge" : ":Mgn";
    const std::string other_guide = axis == 0 ? ":Mgn" : ":Mge";
    Rig rig("AM5");
    bool ran = false;  // the hook runs on the jog's own thread, after its claim
    int code = 0;
    alpacacore::vendor::zwo::set_initiator_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name != "MoveAxis" || when != "claim gap" || ran) return;
        ran = true;
        try {
            rig.driver->pulse_guide(direction, 1000);
        } catch (const AlpacaException& ex) {
            code = ex.error_code();
        }
    });
    rig.connect();
    rig.advance(250ms);
    rig.driver->move_axis(axis, 1.0);
    if (when == "jogging") {
        try {
            rig.driver->pulse_guide(direction, 1000);
        } catch (const AlpacaException& ex) {
            code = ex.error_code();
        }
    } else {
        REQUIRE(ran);
    }
    CHECK(code == AlpacaError::InvalidOperation);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    for (int i = 0; i < 8; ++i) rig.advance(250ms);
    CHECK(rig.mount.count(":Mg") == 0);
    CHECK(rig.mount.count(":Q") == 0);  // nothing stopped the jog
    CHECK(rig.driver->get_slewing());
    REQUIRE_NOTHROW(rig.driver->pulse_guide(other_direction, 1000));  // the other axis is not refused
    CHECK(rig.mount.wait_for_count(other_guide, 1, kReal));
    CHECK(rig.mount.count(guide) == 0);
    // The stop's tracking restore waits on the clock for the other axis's pulse; the Rig advances until it returns.
    auto& stop = rig.call([&] { rig.driver->move_axis(axis, 0.0); });
    rig.advance_until([&] { return ready(stop, 1ms); }, 250ms, 5s);
    REQUIRE(ready(stop, kReal));
    stop.get();
    REQUIRE_NOTHROW(rig.driver->pulse_guide(direction, 1000));  // the refusal ended with the jog
    CHECK(rig.mount.wait_for_count(guide, 1, kReal));
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

// open-astro#1834 PIN: the Tracking-on wait sleeps to the pulse's exact end, not to the next 200 ms step: the pulse
// ends 1300 ms into the wait, so the write is done at 1300 ms, not 1400 ms.
// Falsified by: zwo_telescope_driver.cpp set_tracking() wait_for_pulse_completion: sleeping a fixed 200 ms step
// instead of min(remaining, 200 ms), so the write returns at 1400 ms.
TEST_CASE("ZWO telescope clock - Tracking on returns at the pulse end, not at the next 200 ms step (#1834 PIN)",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);  // pauses the poll, so the cached Tracking state ages past 2 s
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    for (int i = 0; i < 22; ++i) rig.advance(100ms);  // 2200 ms; the pulse ends at 3500 ms
    auto& write = rig.call([&] { rig.driver->set_tracking(true); });
    rig.wait_for_blocked_caller(2);                   // the poll loop and the pulse worker wait on the clock too
    for (int i = 0; i < 12; ++i) rig.advance(100ms);  // 1200 ms into the wait
    CHECK_FALSE(ready(write));
    rig.advance(100ms);  // 1300 ms: the pulse ends
    CHECK(ready(write, kReal));
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

// #2408 (#2396 AB-8): the poll loop's slew bound stops the GOTO at 15 minutes, and the synchronous wait reports that
// stop.
TEST_CASE("ZWO telescope clock - a synchronous slew that never settles is stopped at 15 minutes",
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
        CHECK(std::string(e.what()).find("did not end within 15 min; the driver stopped it") != std::string::npos);
    }
    CHECK(rig.mount.count(":Q") == 1);
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
    // The Sync's fence cancels the GOTO's back-off, but that body can still hold its clock registration while it
    // leaves the wait, so a waiter count cannot tell it from the Sync's own park: with the Sync still on the wire
    // (the e4 reply, then ":Q"), the step budget below ran out in microseconds and the second ":SMMC" never came
    // (#2051). The Sync's 200 ms settle sleep is the next registration after the GOTO's; wait for that one.
    const auto waits = rig.clock.wait_count();
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, kReal));
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

// #2093: a PulseGuide accepted while an older Sync waits out an "e4" reply is a newer motion command, as a GOTO is:
// the Sync sends nothing after the pulse's ":Mg" (no ":Q" that would stop the pulse, no retried sync).
// Falsified by: zwo_telescope_driver.cpp pulse_guide() without the ++slew_error_epoch_ in its queue section.
TEST_CASE("ZWO an older Sync never stops a newer PulseGuide (#2093)", "[zwo][telescope][unit][clock][sync-fence]") {
    const bool last_resort = GENERATE(false, true);  // the pulse lands in the 200 ms settle, or the 300 ms one
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();  // Sync sent its first ":Q" and waits out the 200 ms settle
    if (last_resort) {
        rig.advance(200ms);
        REQUIRE(rig.mount.wait_for_count(":SMMC", 2, kReal));
        rig.wait_for_blocked_caller();  // Sync sent its last-resort ":Q" and waits out the 300 ms settle
    }
    rig.driver->pulse_guide(0, 1000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    const auto before = rig.mount.commands();
    const auto pulse_at = static_cast<std::size_t>(
        std::find_if(before.begin(), before.end(), [](const std::string& c) { return c.rfind(":Mg", 0) == 0; }) -
        before.begin());
    rig.advance_until([&] { return ready(sync); }, 100ms, 3s);
    REQUIRE(ready(sync, kReal));
    const auto sent = rig.mount.commands();
    for (std::size_t i = pulse_at + 1; i < sent.size(); ++i) {
        INFO("after the pulse: " << sent[i]);
        CHECK(sent[i] != ":Q");
        CHECK(sent[i].rfind(":SMMC", 0) != 0);
        CHECK(sent[i] != ":CM");
        CHECK(sent[i] != ":Sr06:00:00");
    }
    require_alpaca_error([&] { sync.get(); }, AlpacaError::InvalidOperation);
}

// #2093: a PulseGuide held before it queues (after its checks) while a Sync passes its reset and reads its epoch. The
// pulse then queues and moves the epoch, so the Sync, settling after an "e4", sends nothing after the pulse's ":Mg".
// Falsified by: zwo_telescope_driver.cpp moving the epoch bump out of pulse_guide()'s queue section to before the
// "PulseGuideQueue" seam (the Sync reads the moved epoch as its own and stops the pulse).
TEST_CASE("ZWO a Sync whose reset runs before a PulseGuide queues never stops that pulse (#2093)",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    std::promise<void> entered;
    std::promise<void> go;
    auto go_future = go.get_future().share();
    std::atomic<bool> armed{true};
    alpacacore::vendor::zwo::set_initiator_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == "PulseGuideQueue" && armed.exchange(false)) {
            entered.set_value();
            (void)go_future.wait_for(kReal);
        }
    });
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto pulse = std::async(std::launch::async, [&] { rig.driver->pulse_guide(0, 1000); });
    REQUIRE(entered.get_future().wait_for(kReal) == std::future_status::ready);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();  // Sync passed its reset, sent its first ":Q" and waits out the 200 ms settle
    go.set_value();
    REQUIRE_NOTHROW(pulse.get());
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    const auto before = rig.mount.commands();
    const auto pulse_at = static_cast<std::size_t>(
        std::find_if(before.begin(), before.end(), [](const std::string& c) { return c.rfind(":Mg", 0) == 0; }) -
        before.begin());
    rig.advance_until([&] { return ready(sync); }, 100ms, 3s);
    REQUIRE(ready(sync, kReal));
    const auto sent = rig.mount.commands();
    for (std::size_t i = pulse_at + 1; i < sent.size(); ++i) {
        INFO("after the pulse: " << sent[i]);
        CHECK(sent[i] != ":Q");
        CHECK(sent[i].rfind(":SMMC", 0) != 0);
        CHECK(sent[i] != ":CM");
    }
    require_alpaca_error([&] { sync.get(); }, AlpacaError::InvalidOperation);
}

// open-astro#1925: an older Sync settling on "e4" must not stop a newer MoveAxis jog, FindHome, Park or Unpark. The
// initiator seam holds each one right after its wire command, where the old code had not yet moved the Sync epoch;
// the Sync's settle timer runs meanwhile, so its ownership check lands in that window. It must throw, with the
// neutral message, and send nothing (":Q", ":SMMC", ":Sr", ":CM") after the newer command.
// Falsified by: zwo_telescope_driver.cpp park() (and find_home(), move_axis(), unpark()): removing the
// bump_motion_epoch() call before the wire command, so the epoch moves only after it and the Sync still stops it.
TEST_CASE("ZWO an older Sync never stops a newer MoveAxis, FindHome, Park or Unpark (#1925)",
          "[zwo][telescope][unit][clock][sync-fence]") {
    const auto initiator_and_command = GENERATE(
        std::pair<std::string, std::string>{"Park", ":hP"}, std::pair<std::string, std::string>{"FindHome", ":hC"},
        std::pair<std::string, std::string>{"MoveAxis", ":Me"}, std::pair<std::string, std::string>{"Unpark", ":Spu"});
    const std::string& label = initiator_and_command.first;
    const std::string& command = initiator_and_command.second;
    CAPTURE(label);
    alpacacore::test::Latch latch;
    Rig rig;
    alpacacore::vendor::zwo::set_initiator_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == label + " sent") latch.block();
    });
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();  // Sync sent its first ":Q" and waits out the 200 ms settle

    auto& initiator = rig.call([&] {
        if (label == "Park") rig.driver->park();
        if (label == "FindHome") rig.driver->find_home();
        if (label == "MoveAxis") rig.driver->move_axis(0, 1.0);
        if (label == "Unpark") rig.driver->unpark();
    });
    const bool held = latch.wait_entered();
    const bool command_sent = rig.mount.wait_for_count(command, 1, kReal);
    if (held && command_sent) rig.advance_until([&] { return ready(sync); }, 200ms, 3s);
    latch.release();  // before any REQUIRE can unwind, so the Rig never waits on the latch
    REQUIRE(held);
    REQUIRE(command_sent);
    REQUIRE(ready(sync, kReal));
    REQUIRE(ready(initiator, kReal));
    initiator.get();

    const auto all = rig.mount.commands();
    const auto command_at =
        std::find_if(all.begin(), all.end(), [&](const std::string& c) { return c.starts_with(command); });
    REQUIRE(command_at != all.end());
    const auto from_sync = [](const std::string& c) {
        return c == ":Q" || c.starts_with(":SMMC") || c.starts_with(":Sr") || c == ":CM";
    };
    CHECK(std::none_of(command_at + 1, all.end(), from_sync));
    try {
        sync.get();
        FAIL("an older Sync must not complete over a newer command");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()) == "Sync superseded by a newer command");
    }
    if (label == "MoveAxis") rig.driver->move_axis(0, 0.0);
}

// open-astro#1925 item 2: an AbortSlew during the settle starts no motion, so the superseded text must not say it did.
// Falsified by: zwo_telescope_driver.cpp sync_to_pair(): restoring the message "Sync superseded by a newer motion
// command".
TEST_CASE("ZWO an older Sync superseded by AbortSlew says so without claiming motion (#1925)",
          "[zwo][telescope][unit][clock][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e4", 2);
    auto& sync = rig.call([&] { rig.driver->sync_to_coordinates(6.0, -15.0); });
    REQUIRE(rig.mount.wait_for_count(":SMMC", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.driver->abort_slew();
    rig.advance_until([&] { return ready(sync); }, 200ms, 3s);
    REQUIRE(ready(sync, kReal));
    CHECK(rig.mount.count(":SMMC") == 1);
    try {
        sync.get();
        FAIL("a Sync superseded by AbortSlew must fail");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::InvalidOperation);
        CHECK(std::string(ex.what()) == "Sync superseded by a newer command");
    }
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

// decision 0022 rule 2 (#1834): the stop's ":Q" is an abort, so it ends the jog's 2 s window; Slewing then reads the
// mount. Until #1834 this case pinned Slewing true for the rest of the 2 s window after the stop.
// Falsified by: zwo_telescope_driver.cpp move_axis(): removing the slew_hold_.clear() in the stop branch, so Slewing
// stays true for 2 s after the jog stops.
TEST_CASE("ZWO telescope clock - a MoveAxis stop ends the jog's 2 s forced window",
          "[zwo][telescope][unit][clock][hold-window]") {
    Rig rig;
    rig.connect();
    rig.advance(250ms);
    rig.driver->move_axis(0, 1.0);
    rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
    rig.driver->move_axis(0, 0.0);
    rig.mount.set_slew_duration(0ms);
    rig.advance(250ms);
    rig.advance(250ms);  // 0.5 s: a poll has read the stopped mount, inside the old 2 s window
    CHECK_FALSE(rig.driver->get_slewing());
    for (int i = 0; i < 8; ++i) rig.advance(250ms);  // 2.5 s
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
    const std::string model = GENERATE("AM5N", "AM5");  // explicit :Gps=2 completion holds on every model
    Rig rig(model);
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
    // "e1" is the AM5N's answer to the unsupported query; "" is a mount that never answers it. Park stops tracking
    // first (#2097), so a silent ":Gps#" runs with tracking off (#2103). It goes silent after Park's own status read:
    // with this rig's 500 ms timeout, a ":GU#" inside a silent ":Gps#"'s owed window times out.
    const std::string reply = GENERATE("0", "1", "x", "e1", "");
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    if (!reply.empty()) rig.mount.override_reply(":Gps", reply);  // unavailable completion indication, stationary
    rig.advance(250ms);
    rig.driver->park();
    if (reply.empty()) rig.mount.go_silent(":Gps");
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

// #2103: a ":Gps#" that never answers leaves a reply owed; the owed-reply fence then spends the next ":GU#"'s whole
// timeout. With tracking off its first byte is 'n' (moving), which must not be read as the status.
TEST_CASE("ZWO telescope clock - a silent :Gps with tracking off never reads as slewing (#2103)",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.driver->set_tracking(false);
    rig.mount.go_silent(":Gps");
    rig.advance(250ms);
    const int gps_before = rig.mount.count(":Gps");
    for (int i = 0; i < 12; ++i) {  // 3 s
        rig.advance(250ms);
        CHECK_FALSE(rig.driver->get_slewing());
        CHECK(rig.driver->get_link_fault().empty());
    }
    CHECK(rig.mount.count(":Gps") >= gps_before + 2);
    const auto sent = rig.mount.commands();
    const auto gps = std::find(sent.begin(), sent.end(), ":Gps");
    REQUIRE(gps != sent.end());
    CHECK(std::find(gps, sent.end(), ":GU") != sent.end());  // a status query followed the silent one
}

// #2097: an AM5 on firmware 1.8.8 ignores ":hP#": no motion, ":Gps#" stays 0. Only the AM5N infers a park from
// stillness; elsewhere, a Park the mount says it has not done fails, and both completion properties keep the error.
TEST_CASE("ZWO telescope clock - a Park the mount ignores fails instead of inferring park (#2097)",
          "[zwo][telescope][unit][clock][park]") {
    const std::string model = GENERATE("AM3", "AM3N", "AM5", "AM7", "AM9");  // #2204: AM3N and unlisted models too
    const std::string reply = GENERATE("0", "1");  // not parked, or a stale in-progress
    Rig rig(model);
    rig.connect();
    rig.mount.set_slew_duration(0ms);  // ":hP" lands at once: ":GU" never reports motion
    rig.mount.override_reply(":Gps", reply);
    rig.advance(250ms);
    rig.driver->park();
    for (int i = 0; i < 19; ++i) {  // to 4.75 s: the start guard keeps the Park pending
        rig.advance(250ms);
        CHECK_FALSE(rig.driver->get_at_park());
        CHECK(rig.driver->get_slewing());
    }
    const int gps_before = rig.mount.count(":Gps");
    for (int i = 0; i < 3; ++i) rig.advance(250ms);  // 5.5 s: the guard has ended
    CHECK(rig.mount.count(":Gps") > gps_before);     // the poll judged the Park after the guard
    CHECK_FALSE(rig.mount.slewing());                // and the mount never moved
    for (int read = 0; read < 2; ++read) {
        try {
            rig.driver->get_at_park();
            FAIL("AtPark must report the failed Park");
        } catch (const AlpacaException& ex) {
            CHECK(ex.error_code() == AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("Park failed") != std::string::npos);
        }
        try {
            rig.driver->get_slewing();
            FAIL("Slewing must report the failed Park");
        } catch (const AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("Park failed") != std::string::npos);
        }
        rig.advance(250ms);
    }
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_at_park());
    rig.advance(250ms);
    CHECK_FALSE(rig.driver->get_slewing());
    // A new Park clears the old failure before it sends ":hP".
    rig.driver->park();
    CHECK(rig.mount.count(":hP") == 2);
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());
}

// #2097 root cause on the rig: an AM5 on firmware 1.8.8 ignores ":hP#" while tracking is on, and parks after ":Td#".
// Park stops tracking before it sends ":hP#".
TEST_CASE("ZWO telescope clock - Park stops tracking before :hP so a tracking AM5 parks (#2097)",
          "[zwo][telescope][unit][clock][park]") {
    const std::string model = GENERATE("AM5N", "AM5");
    Rig rig(model);
    rig.mount.set_ignore_park_while_tracking(true);
    rig.mount.set_slew_duration(3000ms);
    rig.connect();
    rig.driver->set_tracking(true);
    rig.advance(250ms);
    REQUIRE(rig.driver->get_tracking());
    rig.driver->park();
    const auto sent = rig.mount.commands();
    const auto td = std::find(sent.begin(), sent.end(), ":Td");
    const auto hp = std::find(sent.begin(), sent.end(), ":hP");
    REQUIRE(hp != sent.end());
    CHECK(td < hp);  // tracking stopped before the park command
    rig.advance(250ms);
    CHECK(rig.mount.slewing());                       // the mount moves to park
    for (int i = 0; i < 22; ++i) rig.advance(250ms);  // 5.75 s
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_tracking());
}

// #2097 keeps the stillness inference where nothing else can tell: a mount that does not answer ":Gps#".
TEST_CASE("ZWO telescope clock - an AM5 without a usable :Gps still infers park from stillness",
          "[zwo][telescope][unit][clock][park]") {
    const std::string reply = GENERATE("x", "e1", "");  // "": never answers (#2103), from after Park's status read
    Rig rig("AM5");
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    if (!reply.empty()) rig.mount.override_reply(":Gps", reply);
    rig.advance(250ms);
    rig.driver->park();
    if (reply.empty()) rig.mount.go_silent(":Gps");
    for (int i = 0; i < 22; ++i) rig.advance(250ms);  // 5.5 s
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
}

// #2097 scopes the failure to a Park that never moved: motion, then a stop with ":Gps#" 0, still infers park.
TEST_CASE("ZWO telescope clock - an AM5 Park that moved and stopped still infers park on :Gps 0",
          "[zwo][telescope][unit][clock][park]") {
    Rig rig("AM5");
    rig.connect();
    rig.mount.set_slew_duration(3000ms);
    rig.mount.override_reply(":Gps", "0");
    rig.advance(250ms);
    rig.driver->park();
    rig.advance(250ms);
    REQUIRE(rig.mount.slewing());                     // the poll sees motion
    for (int i = 0; i < 21; ++i) rig.advance(250ms);  // 5.5 s: stopped since 3 s
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
}

// #2178: an AM5 does not move on Unpark, and ConformU reads Slewing 1.8 s after it.
// Falsified by: zwo_telescope_driver.cpp unpark() arming the Slewing window after ":Spu#".
TEST_CASE("ZWO telescope clock - Slewing is false within one poll of an Unpark that starts no motion (#2178)",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.mount.set_park_state(2);
    rig.connect();
    rig.advance(250ms);
    REQUIRE(rig.driver->get_at_park());
    rig.driver->unpark();
    rig.advance(250ms);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
}

// #2178: dropping the window must not hide a mount that really moves during Unpark.
// Falsified by: zwo_telescope_driver.cpp get_slewing() no longer reading the polled ":GU#" moving status.
TEST_CASE("ZWO telescope clock - Slewing is true after Unpark when the mount reports motion (#2178)",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.mount.set_park_state(2);
    rig.connect();
    rig.advance(250ms);
    REQUIRE(rig.driver->get_at_park());
    rig.driver->unpark();
    rig.mount.override_reply(":Gps", "0");
    rig.mount.override_reply(":GU", "nGM040001895");
    rig.advance(250ms);
    rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
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

// PIN: an accepted PulseGuide is never dropped because a command superseded the pulse before it. Pulse, a MoveAxis
// on the same axis mid-step, pulse again: the second pulse still reaches the mount. (Before #2263 the superseding
// command was a slew; a PulseGuide during a slew is now refused. Since #2459 a pulse on the jogging axis is refused
// too, so the jog is stopped before the second pulse.)
// Falsified by: pulse_guide() leaving a pulse queued behind a superseded drain body that never sends it.
TEST_CASE("ZWO telescope clock - a pulse queued after a MoveAxis superseded the previous pulse is still sent (PIN)",
          "[zwo][telescope][unit][clock][operation-slot][motion-owner]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 3000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    rig.driver->move_axis(1, 1.0);                     // mid-step: supersedes the Dec pulse and clears its queue
    CHECK(rig.mount.wait_for_count(":Mn", 1, kReal));  // the jog reached the mount
    rig.driver->move_axis(1, 0.0);
    rig.driver->pulse_guide(1, 500);
    // Virtual time only: the superseded body wakes and the new pulse is drained.
    rig.advance_until([&] { return rig.mount.count(":Mg") >= 2; }, 100ms, 10s);
    CHECK(rig.mount.wait_for_count(":Mg", 2, kReal));
}

// Round 2 finding 3: the flag that says a pulse drain runs is cleared on disconnect and on connect, so a pulse after
// a reconnect starts a new drain and reaches the mount.
// Falsified by: zwo_telescope_driver.cpp stop_pulse_drain() and the connect reset (reset_session_state_for_connect)
// both keeping drain_active (the two clears removed together), so the pulse after the reconnect waits for a
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

// #2095: a redundant Connected=true drops the queued pulses but leaves the running drain in charge, so the next
// PulseGuide queues behind the step in flight instead of sending its ":Mg" over it. IsPulseGuiding covers the
// running pulse and B's wait behind it.
// Falsified by: zwo_telescope_driver.cpp reset_session_state_for_connect() clearing drain_active on the
// redundant path too, so the next pulse starts a new drain that supersedes the running one; or zeroing
// the pulse window and queue_end there while the drain is kept, so IsPulseGuiding reads false while A runs.
TEST_CASE("ZWO telescope clock - a redundant connect never overlaps the running pulse (#2095)",
          "[zwo][telescope][unit][clock][operation-slot]") {
    Rig rig;
    rig.connect();
    rig.driver->pulse_guide(0, 6000);  // A: two 3000 ms steps
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    rig.wait_for_blocked_caller();    // the drain waits out A's first step
    rig.driver->pulse_guide(1, 500);  // C: queued behind A, on the same axis
    auto& redundant = rig.call([&] { rig.driver->set_connected(true); });
    for (int i = 0; i < 200 && redundant.wait_for(10ms) != std::future_status::ready; ++i) rig.advance(10ms);
    REQUIRE(redundant.wait_for(0ms) == std::future_status::ready);
    redundant.get();
    CHECK(rig.driver->get_is_pulse_guiding());               // A still runs
    rig.driver->pulse_guide(1, 1000);                        // B
    CHECK_FALSE(rig.mount.wait_for_count(":Mg", 2, 200ms));  // B waits for A's step
    bool guiding_until_b = true;
    rig.advance_until(
        [&] {
            const bool guiding = rig.driver->get_is_pulse_guiding();
            if (!guiding && rig.mount.count(":Mgs") == 0) guiding_until_b = false;
            return rig.mount.count(":Mg") >= 3;
        },
        100ms, 10s);
    CHECK(guiding_until_b);  // never false before B's ":Mg"

    std::vector<std::string> pulses;
    for (const auto& command : rig.mount.commands()) {
        if (command.rfind(":Mg", 0) == 0) pulses.push_back(command);
    }
    CHECK(pulses == std::vector<std::string>{":Mgn3000", ":Mgn3000", ":Mgs1000"});  // C was dropped, B came last
}

// open-astro#2165: a client write or MoveAxis stop issued while the telemetry poll owns the link (its ":GR" held in
// flight) reaches the wire right after that exchange, not after the rest of the poll cycle (":GD", ":GZ", ...).
// Falsified by: zwo_mount_protocol_wrapper.cpp lock_exchange() taking mutex_ for a background exchange without
// waiting for the client exchanges already waiting, so the poll thread re-locks it first for every remaining query.
TEST_CASE("ZWO telescope clock - a client command waits for at most one poll exchange (#2165)",
          "[zwo][telescope][unit][clock]") {
    const bool stop = GENERATE(false, true);  // a guide-rate write, or a MoveAxis stop
    const std::string expected = stop ? ":Qe" : ":Rg";
    Rig rig;
    rig.connect();
    if (stop) rig.driver->move_axis(0, 1.0);  // a jog to stop
    rig.mount.hold_reply(":GR");
    rig.clock.advance(250ms);  // the poll starts a cycle: ":GU", then its ":GR" is held
    REQUIRE(rig.mount.wait_until_held(kReal));
    const int poll_queries = static_cast<int>(rig.mount.commands().size());
    auto& client = rig.call([&] {
        if (stop) {
            rig.driver->move_axis(0, 0.0);
        } else {
            rig.driver->set_guide_rate_right_ascension(0.002089);  // 0.5x sidereal
        }
    });
    // Real time for the client to reach the I/O lock behind the held exchange (a rendezvous, not a driver timer).
    CHECK_FALSE(ready(client, 200ms));
    rig.mount.release();
    REQUIRE(ready(client, kReal));
    client.get();
    const auto sent = rig.mount.commands();
    REQUIRE(static_cast<int>(sent.size()) > poll_queries);
    INFO("first command after the held :GR: " << sent[static_cast<std::size_t>(poll_queries)]);
    CHECK(sent[static_cast<std::size_t>(poll_queries)].rfind(expected, 0) == 0);
}

// ---- decision 0022 rule 2: arm before the command, give the window back when the command fails (#1834) ----------

namespace {

// The motion initiators that arm the slew window, with the send-hook label of their command.
struct HoldInitiator {
    std::string name;
    std::string label;
};

void run_initiator(TelescopeDriver& driver, const std::string& name) {
    if (name == "MoveAxis") driver.move_axis(0, 1.0);
    if (name == "FindHome") driver.find_home();
    if (name == "Park") driver.park();
}

AlpacaException injected_send_failure() {
    return AlpacaException("injected send failure", AlpacaError::DriverException);
}

std::thread refuse_pulse_thread(std::function<void()>) {
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
}

}  // namespace

// The slew window is armed before the command goes out, so a Slewing read while it is written is true.
// Falsified by: zwo_telescope_driver.cpp arm_slew_hold(): removing slew_hold_.arm(clock_.now(), hold), so the window
// is armed only after the command returns and the read during the send is false.
TEST_CASE("ZWO hold window - MoveAxis, FindHome and Park read Slewing true while their command is sent (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    const auto initiator =
        GENERATE(HoldInitiator{"MoveAxis", ":Rv"}, HoldInitiator{"FindHome", ":hC"}, HoldInitiator{"Park", ":hP"});
    CAPTURE(initiator.name);
    Rig rig;
    std::atomic<int> slewing_at_send{-1};
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == initiator.label) slewing_at_send.store(rig.driver->get_slewing() ? 1 : 0);
    });
    rig.connect();
    rig.advance(250ms);
    REQUIRE_FALSE(rig.driver->get_slewing());
    run_initiator(*rig.driver, initiator.name);
    CHECK(slewing_at_send.load() == 1);
}

// A command that fails to send starts no motion, so its slew window goes: Slewing reads false at once, not for the
// 2 s / 5 s hold.
// Falsified by: zwo_telescope_driver.cpp restore_slew_hold(): removing slew_hold_ = arm.before, so the window armed
// before the failed command stays and Slewing reads true.
TEST_CASE("ZWO hold window - a MoveAxis, FindHome or Park whose command fails leaves Slewing false (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    const auto initiator =
        GENERATE(HoldInitiator{"MoveAxis", ":Rv"}, HoldInitiator{"FindHome", ":hC"}, HoldInitiator{"Park", ":hP"});
    CAPTURE(initiator.name);
    Rig rig;
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == initiator.label) throw injected_send_failure();
    });
    rig.connect();
    rig.advance(250ms);
    REQUIRE_THROWS(run_initiator(*rig.driver, initiator.name));
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.mount.count(initiator.label) == 0);
}

// The failed command gives back the window it replaced, not a cleared one: an accepted GOTO the fence does not stop
// still owns it, so Slewing stays true for the GOTO's hold although the mount already reports idle.
// Falsified by: zwo_telescope_driver.cpp restore_slew_hold(): slew_hold_.clear() in place of slew_hold_ = arm.before,
// so the failed command also ends the accepted GOTO's window.
TEST_CASE("ZWO hold window - a failed MoveAxis, FindHome or Park keeps an accepted GOTO's window (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    const auto initiator =
        GENERATE(HoldInitiator{"MoveAxis", ":Rv"}, HoldInitiator{"FindHome", ":hC"}, HoldInitiator{"Park", ":hP"});
    CAPTURE(initiator.name);
    Rig rig;
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == initiator.label) throw injected_send_failure();
    });
    rig.connect();
    rig.mount.set_slew_duration(0ms);  // the mount reports idle at once: only the window says Slewing
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();  // the GOTO body has accepted ":MS" and armed its 5 s window
    REQUIRE(rig.driver->get_slewing());
    REQUIRE_THROWS(run_initiator(*rig.driver, initiator.name));
    // Two poll cycles publish a fresh frame from the idle mount: only an open window still forces it to moving.
    rig.advance(250ms);
    rig.advance(250ms);
    CHECK(rig.driver->get_slewing());
}

// A rejected ":MS" started no slew, so the GOTO body ends its window when it keeps the failure, as a stopped body
// does: the next poll publishes the mount's own Home flag instead of a forced "moving" frame for 5 s.
// Falsified by: zwo_telescope_driver.cpp run_goto() fail(): removing slew_hold_.clear() in the branch that keeps the
// failure, so the poll forces at_home false until the 5 s window ends.
TEST_CASE("ZWO hold window - a rejected GOTO leaves no slew window behind (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);
    rig.driver->find_home();
    for (int i = 0; i < 24; ++i) rig.advance(250ms);  // 6 s: home landed, its window closed
    REQUIRE(rig.driver->get_at_home());
    rig.mount.reject_goto("2");
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();  // the body kept the failure and released the poll
    rig.advance(250ms);        // one full poll cycle after the failure, 0.5 s or less into the 5 s hold
    require_alpaca_error([&] { static_cast<void>(rig.driver->get_slewing()); }, AlpacaError::DriverException);
    CHECK(rig.driver->get_at_home());
}

// A ":Mg" that fails to send starts no pulse; with nothing queued behind it, IsPulseGuiding is false at once rather
// than for the pulse plus 500 ms.
// Falsified by: zwo_telescope_driver.cpp release_failed_pulse_window(): removing the pulse.hold.arm(...) statement,
// so the window armed for the 3000 ms pulse stays.
TEST_CASE("ZWO hold window - a failed :Mg ends IsPulseGuiding at once (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    Rig rig;
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == ":Mg") throw injected_send_failure();
    });
    rig.connect();
    rig.driver->pulse_guide(0, 3000);
    REQUIRE(rig.clock.wait_for_waiters(2, kReal));  // the poll loop and the drain's hold after its last task
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.mount.count(":Mg") == 0);
}

// One window covers an axis's queue (#2263: each axis has its own): a failed ":Mg" gives up only its own share. The
// pulse queued behind it on that axis still owns the window (true while the drain reconciles, before the queued task
// starts), and the window then ends at that pulse plus 500 ms, with no time counted for the failed 3000 ms pulse.
// Falsified by: zwo_telescope_driver.cpp release_failed_pulse_window(): pulse.hold.clear() in place of the
// pulse.hold.arm(...) that covers the queue, so IsPulseGuiding reads false while the queued pulse waits.
TEST_CASE("ZWO hold window - a failed :Mg keeps the window for the pulse queued behind it (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    Rig rig;
    std::atomic<int> pulses_seen{0};
    std::promise<void> first_entered;
    std::promise<void> second_queued;
    auto queued = second_queued.get_future().share();
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&, queued](std::string_view name) {
        if (name != ":Mg" || pulses_seen.fetch_add(1) != 0) return;
        first_entered.set_value();
        static_cast<void>(queued.wait_for(kReal));  // bounded, so a failed case never wedges the drain
        throw injected_send_failure();
    });
    rig.connect();
    rig.mount.hold_reply(":GR");  // the drain's reconciliation read after the failed step (the poll is paused)
    rig.driver->pulse_guide(0, 3000);
    REQUIRE(first_entered.get_future().wait_for(kReal) == std::future_status::ready);
    rig.driver->pulse_guide(1, 1000);  // queued on the same axis before the first ":Mg" fails
    second_queued.set_value();
    const bool held = rig.mount.wait_until_held(kReal);
    const bool guiding_while_queued = rig.driver->get_is_pulse_guiding();
    rig.mount.release();
    REQUIRE(held);
    CHECK(guiding_while_queued);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));  // the queued pulse is sent
    rig.advance(1499ms);
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.advance(1ms);  // 1000 ms + 500 ms: the failed pulse counts for nothing
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.mount.count(":Mgn") == 0);
}

// A ":Mg" step that went out keeps its completion hold when a later ":Mg" fails: the second step of the same pulse,
// or the first step of the pulse queued behind it (the pulse before it skipped its own hold because it was queued).
// IsPulseGuiding stays true until the sent step's end plus 500 ms, not only until the failure.
// Falsified by: zwo_telescope_driver.cpp release_failed_pulse_window(): arming to queue_until alone in place of
// std::max(queue_until, sent_until), so the window ends at the failure.
TEST_CASE("ZWO hold window - a failed :Mg keeps the completion hold of the step sent before it (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    const bool queued = GENERATE(false, true);
    CAPTURE(queued);
    Rig rig;
    std::atomic<int> pulses_seen{0};
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*rig.driver, [&](std::string_view name) {
        if (name == ":Mg" && pulses_seen.fetch_add(1) == 1) throw injected_send_failure();
    });
    rig.connect();
    // One 5000 ms pulse sends a 3000 ms step, then fails its second; or a 1000 ms pulse is sent and the 1000 ms pulse
    // queued behind it fails its first.
    const auto sent_step = queued ? 1000ms : 3000ms;
    rig.driver->pulse_guide(0, queued ? 1000 : 5000);
    if (queued) rig.driver->pulse_guide(1, 1000);  // the same axis
    // The sent step's end is stamped at virtual time 0; the drain then waits it out, besides the poll loop.
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    rig.wait_for_blocked_caller();
    rig.advance(sent_step);  // the drain wakes and the next ":Mg" fails
    REQUIRE(pulses_seen.load() == 2);
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.advance(499ms);
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.advance(1ms);  // the sent step's end plus 500 ms
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.mount.count(":Mg") == 1);
}

// A MoveAxis stop with the other axis idle sends ":Q", which stops an accepted GOTO, so it ends the GOTO's 5 s window:
// Slewing reads the idle mount at once, as after AbortSlew.
// Falsified by: zwo_telescope_driver.cpp move_axis(): removing the slew_hold_.clear() in the stop branch, so the
// stopped GOTO's window keeps Slewing true.
TEST_CASE("ZWO hold window - a MoveAxis stop ends an accepted GOTO's window (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    const int axis = GENERATE(0, 1);
    CAPTURE(axis);
    Rig rig;
    rig.connect();
    rig.mount.set_slew_duration(0ms);  // the mount reports idle at once: only the window says Slewing
    rig.driver->slew_to_coordinates_async(6.0, 30.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    rig.await_poll_resumed();  // the GOTO body has accepted ":MS" and armed its 5 s window
    REQUIRE(rig.driver->get_slewing());
    rig.driver->move_axis(axis, 0.0);
    // Two poll cycles publish a fresh frame from the idle mount: only an open window would still force it to moving.
    rig.advance(250ms);
    rig.advance(250ms);
    CHECK_FALSE(rig.driver->get_slewing());
}

// A PulseGuide whose drain the slot refuses to start sends nothing, so it gives the pulse window back:
// IsPulseGuiding is false after the refusal, not for the pulse plus 500 ms.
// Falsified by: zwo_telescope_driver.cpp pulse_guide(): removing the pulse.hold = pulse_before restore in the
// refused-start catch, so the window armed for the 1000 ms pulse stays.
TEST_CASE("ZWO hold window - a refused pulse drain start leaves IsPulseGuiding false (#1834)",
          "[zwo][telescope][unit][clock][hold-window]") {
    Rig rig;
    alpacacore::vendor::zwo::set_pulse_spawn_for_testing(*rig.driver, refuse_pulse_thread);
    rig.connect();
    CHECK_THROWS(rig.driver->pulse_guide(0, 1000));
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.mount.count(":Mg") == 0);
}

namespace {
constexpr double kSiderealDegPerSec = 360.0 / 86164.0905;
}  // namespace

// open-astro#2462: a GuideRate write of 0 is refused with InvalidValue, before any connection check, on either axis;
// the cached rate stays and nothing is sent (a stored 0 made every later PulseGuide a silent no-op).
// Falsified by: zwo_telescope_driver.cpp set_guide_rate() range check guarded by `effective > 0.0 &&` (the old 0
// exemption: a 0 is stored unsent, no exception).
TEST_CASE("ZWO telescope clock - a GuideRate of 0 is refused and leaves the rate (2462)",
          "[zwo][telescope][unit][guide-rate]") {
    const bool ra = GENERATE(false, true);
    const auto write_zero = [&](Rig& rig) {
        if (ra)
            rig.driver->set_guide_rate_right_ascension(0.0);
        else
            rig.driver->set_guide_rate_declination(0.0);
    };
    Rig rig;
    require_alpaca_error([&] { write_zero(rig); }, AlpacaError::InvalidValue);  // disconnected: the value first
    rig.connect();
    const auto before = rig.driver->get_guide_rate();
    REQUIRE(before.ra > 0.0);
    const int sent = rig.mount.count(":Rg");
    require_alpaca_error([&] { write_zero(rig); }, AlpacaError::InvalidValue);
    CHECK(rig.driver->get_guide_rate().ra == before.ra);
    CHECK(rig.driver->get_guide_rate().dec == before.dec);
    CHECK(rig.mount.count(":Rg") == sent);
}

// open-astro#2462: a mount that takes only the x15 encoding gets the scaled resend, and GuideRate reads the rate the
// mount reports after it.
// Falsified by: zwo_mount_protocol_wrapper.cpp set_guide_rate() resending the unscaled rate (`send_rate(scaled / 15.0,
// 1)`) instead of `send_rate(scaled, 1)` (the mount keeps 0.50x and the write fails).
TEST_CASE("ZWO telescope clock - GuideRate after the x15 fallback is the mount's readback (2462)",
          "[zwo][telescope][unit][guide-rate]") {
    Rig rig;
    rig.mount.set_guide_rate_encoding(FakeZwoMount::GuideRateEncoding::ScaledOnly);
    rig.connect();
    const int reads = rig.mount.count(":Ggr");
    REQUIRE_NOTHROW(rig.driver->set_guide_rate_right_ascension(0.80 * kSiderealDegPerSec));
    CHECK(rig.mount.guide_rate() == "12.0");
    CHECK(rig.mount.count(":Ggr") == reads + 2);  // one readback per encoding
    CHECK(rig.driver->get_guide_rate().ra / kSiderealDegPerSec == Catch::Approx(0.80).margin(0.005));
}

// open-astro#2462: a mount that takes neither encoding fails the write with DriverException, and GuideRate reads the
// rate the mount still runs, not the request.
// Falsified by: zwo_mount_protocol_wrapper.cpp set_guide_rate() returning `guide_rate` after the x15 send instead of
// `get_guide_rate()` (the write passes and GuideRate reads the unsent 0.80x).
TEST_CASE("ZWO telescope clock - a GuideRate the mount does not take fails and reads the mount's rate (2462)",
          "[zwo][telescope][unit][guide-rate]") {
    Rig rig;
    rig.mount.set_guide_rate_encoding(FakeZwoMount::GuideRateEncoding::None);
    rig.connect();
    require_alpaca_error([&] { rig.driver->set_guide_rate_declination(0.80 * kSiderealDegPerSec); },
                         AlpacaError::DriverException);
    CHECK(rig.mount.guide_rate() == "0.50");
    CHECK(rig.driver->get_guide_rate().ra / kSiderealDegPerSec == Catch::Approx(0.50).margin(0.005));
    CHECK(rig.driver->get_guide_rate().dec / kSiderealDegPerSec == Catch::Approx(0.50).margin(0.005));
}

// open-astro#2492: Sync takes its offset from a position read after the sync command, not from the poll's sample
// taken before it. On a mount that applies ":SMMC" to its own coordinates, RA/Dec read the sync position after the
// next poll instead of the sync correction counted twice.
// Falsified by: zwo_telescope_driver.cpp sync_to_pair() taking `raw` from cached_equatorial_ when set instead of
// protocol.get_current_equatorial() (RA reads 14.0, Dec 47.0).
TEST_CASE("ZWO telescope clock - Sync ignores a position sampled before the sync (2492)",
          "[zwo][telescope][unit][clock][sync]") {
    Rig rig;
    rig.mount.set_sync_applies(true);
    rig.connect();
    rig.advance(250ms);  // a poll publishes the pre-sync position (12h, +45)
    REQUIRE(rig.driver->get_right_ascension() == Catch::Approx(12.0).margin(1e-3));
    rig.driver->sync_to_coordinates(13.0, 46.0);
    REQUIRE(rig.mount.ra_hours() == Catch::Approx(13.0).margin(1e-3));
    const int polls = rig.mount.count(":GD");
    rig.advance_until([&] { return rig.mount.count(":GD") > polls + 1; }, 250ms, 5s);
    REQUIRE(rig.mount.count(":GD") > polls + 1);
    CHECK(rig.driver->get_right_ascension() == Catch::Approx(13.0).margin(1e-3));
    CHECK(rig.driver->get_declination() == Catch::Approx(46.0).margin(1e-3));
}

// open-astro#2492: a no-op Tracking write is skipped only while the frame's mandatory sample is 2 s old or less; a
// client write never renews that age. The poll is paused by a pulse, so the sample ages while the first write
// publishes its own Tracking value into the frame.
// Falsified by: zwo_telescope_driver.cpp set_tracking() skipping on a stamp that the client's own write renews
// instead of the frame's mandatory age (the repeated Tracking=false sends no ":Td", count stays at 1).
TEST_CASE("ZWO telescope clock - a repeated Tracking write is sent once the mandatory sample is over 2 s old (2492)",
          "[zwo][telescope][unit][clock]") {
    Rig rig;
    rig.connect();
    rig.advance(250ms);  // a fresh mandatory sample, Tracking on
    REQUIRE(rig.driver->get_tracking());
    const int enables = rig.mount.count(":Te");
    rig.driver->set_tracking(true);
    CHECK(rig.mount.count(":Te") == enables);  // equal to the frame, sample fresh: nothing sent

    rig.driver->pulse_guide(0, 3000);  // pauses the poll, so the mandatory sample ages
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    for (int i = 0; i < 22; ++i) rig.advance(100ms);  // 2200 ms; the pulse ends at 3500 ms
    const int disables = rig.mount.count(":Td");
    rig.driver->set_tracking(false);  // a client write: the frame now says off
    CHECK(rig.mount.count(":Td") == disables + 1);
    rig.driver->set_tracking(false);  // equal to the frame, but the sample is over 2 s old
    CHECK(rig.mount.count(":Td") == disables + 2);
}

// open-astro#2492: ensure_not_parked reads the frame's parked flag while the mandatory sample is fresh. Pins; both
// pass on the cache-lifetime code too.
TEST_CASE("ZWO telescope clock - a slew after Unpark is allowed at once and refused while the frame says parked (2492)",
          "[zwo][telescope][unit][clock]") {
    SECTION("unparked") {
        Rig rig;
        rig.mount.set_park_state(2);
        rig.connect();
        rig.advance(250ms);
        REQUIRE(rig.driver->get_at_park());
        rig.driver->unpark();
        CHECK_NOTHROW(rig.driver->slew_to_coordinates_async(6.0, 30.0));
    }
    SECTION("parked") {
        Rig rig;
        rig.mount.set_park_state(2);
        rig.connect();
        rig.advance(250ms);
        REQUIRE(rig.driver->get_at_park());
        require_alpaca_error([&] { rig.driver->slew_to_coordinates_async(6.0, 30.0); },
                             AlpacaError::InvalidWhileParked);
    }
}

#endif  // !_WIN32

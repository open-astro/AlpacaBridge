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
// Park is an asynchronous initiator (issue #208): it must return well inside
// the ConformU 4.5 STANDARD 1 s target while the park slew runs in the
// background, Slewing stays true until the mount arrives, and AtPark flips
// true in the same step Slewing drops. A FakeMountServer plays a Celestron
// NexStar handset whose GOTO takes ~1.5 s, so the whole lifecycle runs hardware-free.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_celestron_handset.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;
using alpacacore::test::error_code_of;
using alpacacore::test::require_alpaca_error;

using alpacacore::test::CelestronTracking;
using alpacacore::test::FakeCelestronHandset;
using alpacacore::test::wait_until;

// This file's handset: tracking off until a case turns it on, the mount at 12AB0500,20000500, and the model and pier
// bytes these cases have always run with (what the old "0#" fallback answered): an id in no model table row and an
// undefined pier byte, as an Alt-Az HC gives. A GEM's 'W' would send a slew to a fixed RA through the meridian-flip
// path for half of every day (no_flip_ra_hours() in test_celestron_async_slew_failure.cpp).
std::shared_ptr<FakeCelestronHandset> make_handset(
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    auto handset = std::make_shared<FakeCelestronHandset>(clock, CelestronTracking::Off, "12AB0500,20000500#");
    handset->model_id.store('0');
    handset->pier_side.store('0');
    return handset;
}

alpacacore::vendor::celestron::ConnectionInfo endpoint(int port, int response_timeout_ms = 200) {
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = response_timeout_ms;
    return info;
}

bool read_slewing_error(alpacacore::TelescopeDriver& driver, std::string* message = nullptr) {
    try {
        (void)driver.get_slewing();
        return false;
    } catch (const alpacacore::AlpacaException& ex) {
        if (message) *message = ex.what();
        return ex.error_code() == alpacacore::AlpacaError::DriverException;
    }
}

// The driver on a caller-owned task clock, so a case steps its waits with
// advance() instead of sleeping.
std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int port, alpacacore::util::TaskClock& clock,
                                                         int response_timeout_ms = 200) {
    return alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(port, response_timeout_ms), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
}

// Steps the fake clock one `step` at a time, once the driver's task is parked
// on it, until `pred` holds or `max_steps` steps pass. Each step waits for the
// woken task to settle, so no step lands between two of its waits.
// Each step first waits for `waiters` tasks on the clock (the status poll and
// the task under test): on the poll alone, a task that reaches the clock late
// finds the steps used up (#1949). Returns pred() when they do not come in 5 s.
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred, std::size_t waiters,
                   std::chrono::milliseconds step = std::chrono::milliseconds(250), int max_steps = 200) {
    for (int i = 0; i < max_steps; ++i) {
        if (pred()) return true;
        if (!clock.wait_for_waiters(waiters, std::chrono::seconds(5))) break;
        clock.advance(step);
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return pred();
}

// #2409: after AbortSlew of a moving mount, Slewing holds true while the verified stop runs on the task clock; steps
// the clock, each step waiting for the status poll and the stop check on it, until Slewing reads false or throws (the
// case then checks which).
bool abort_verified(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver) {
    return advance_until(
        clock,
        [&] {
            try {
                return !driver.get_slewing();
            } catch (const std::exception&) {
                return true;
            }
        },
        2, std::chrono::milliseconds(100));
}

}  // namespace

TEST_CASE("Celestron async - Park returns immediately, AtPark flips when the slew ends",
          "[celestron][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_at_park());

    const auto t0 = Clock::now();
    driver->park();
    const auto park_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(park_ms < 1000);           // ConformU 4.5 STANDARD target for an async initiator
    REQUIRE(driver->get_slewing());  // parking reports Slewing until AtPark
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO was dispatched
    // Park while a park is in flight is a no-op: the running slew is neither
    // cancelled nor restarted (still exactly one GOTO on the wire).
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_at_park());
    // The task is parked on its first 250 ms poll (the status poll is the other waiter): a second GOTO would be on the
    // wire by now.
    REQUIRE(clock.wait_for_waiters(2, std::chrono::seconds(5)));
    CHECK(st->goto_count.load() == 1);

    REQUIRE(advance_until(clock, [&] { return driver->get_at_park(); }, 2));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_tracking());  // park stops tracking

    driver->park();  // second Park on a parked mount is harmless
    REQUIRE(driver->get_at_park());
    REQUIRE_FALSE(driver->get_slewing());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Celestron async - Unpark during a park cancels it", "[celestron][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(abort_verified(clock, *driver));  // #2409: Slewing holds until the Unpark's stop verifies
    REQUIRE_FALSE(driver->get_slewing());
    // The cancelled park task must never flip AtPark afterwards.
    // Virtual time well past the GOTO and the start grace.
    CHECK_FALSE(advance_until(clock, [&] { return driver->get_at_park(); }, 1, std::chrono::milliseconds(250), 12));
    REQUIRE_FALSE(driver->get_at_park());

    // A park in flight gates motion members like a completed park does, so a
    // slew or axis jog cannot silently clobber it (ParkedException, 0x408).
    driver->park();
    REQUIRE(driver->get_slewing());
    require_alpaca_error([&] { driver->move_axis(0, 0.5); }, alpacacore::AlpacaError::InvalidWhileParked);
    // Tracking is off on this fake: the parked refusal comes first (open-astro#1274).
    try {
        driver->slew_to_coordinates_async(5.0, 20.0);
        FAIL("a slew during a park in flight must be refused");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    require_alpaca_error([&] { driver->sync_to_coordinates(5.0, 20.0); }, alpacacore::AlpacaError::InvalidWhileParked);
    // #2257: a bad argument is InvalidValue even while parked; the argument check comes first.
    require_alpaca_error([&] { driver->slew_to_coordinates_async(25.0, 20.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates(5.0, 91.0); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE(driver->get_slewing());  // neither touched the park in flight
    try {
        driver->move_axis(0, 0.5);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    REQUIRE(driver->get_slewing());  // the park is still in flight
    // #2287: a bad sync argument is InvalidValue even while parked; the argument check comes first.
    require_alpaca_error([&] { driver->sync_to_coordinates(25.0, 20.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->sync_to_coordinates(5.0, 91.0); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE(driver->get_slewing());  // the refused syncs left the park in flight
    driver->abort_slew();            // ...but AbortSlew may cancel it
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(abort_verified(clock, *driver));
    REQUIRE_FALSE(driver->get_slewing());

    // Disconnect with a park in flight joins the task cleanly.
    driver->park();
    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
}

TEST_CASE("Celestron FindHome - refuses during Park without replacing the park task",
          "[celestron][telescope][async][home]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    try {
        driver->find_home();
        FAIL("FindHome was accepted while Park was in progress");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    CHECK(st->level_start_count.load() == 0);
    CHECK(driver->get_slewing());
    CHECK(advance_until(clock, [&] { return driver->get_at_park(); }, 2));
    CHECK(st->goto_count.load() == 1);
    driver->unpark();
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - dispatch failure persists through Slewing and AtHome",
          "[celestron][telescope][async][homefailure]") {
    auto st = make_handset();
    st->reject_level_start.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    try {
        driver->find_home();
        FAIL("FindHome returned normally although MC_LEVEL_START was unanswered");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("FindHome dispatch failed") != std::string::npos);
    }
    CHECK(st->level_start_count.load() == 1);
    CHECK(read_slewing_error(*driver));
    try {
        static_cast<void>(driver->get_at_home());
        FAIL("AtHome returned normally after its FindHome dispatch failed");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("FindHome dispatch failed") != std::string::npos);
    }

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_FALSE(driver->get_at_home());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// #742: the three stops Unpark sends to cancel a park in flight (cancel GOTO,
// then both axes to rate 0) sat in one empty catch, and Unpark then set
// Slewing false and returned success. With a handset that answers nothing,
// Unpark must throw once the park task is joined, and Slewing must not read
// false: the park slew may still be running.
TEST_CASE("Celestron async - Unpark reports stops a silent handset never answered", "[celestron][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the park slew is on the wire
    st->silent.store(true);

    try {
        driver->unpark();
        FAIL("Unpark returned success although no stop was answered");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).rfind("Unpark could not stop the park slew: ", 0) == 0);
    }
    CHECK_FALSE(driver->get_at_park());
    CHECK_FALSE(driver->get_at_home());
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Slewing must surface the failed Unpark stop");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Unpark could not stop") != std::string::npos);
    }

    driver->set_connected(false);
}

// #742: when the park task fails (here the GOTO itself goes unanswered) it
// stops the mount; those stops were swallowed and Slewing set false. A stop
// that fails must be logged at ERROR, and Slewing must not read false while
// the handset answers nothing.
TEST_CASE("Celestron async - a failed park logs the stops a silent handset never answered",
          "[celestron][telescope][async]") {
    std::atomic<int> stop_errors{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Error && component == "Celestron" &&
                message.find("stop after park failure failed: ") != std::string_view::npos &&
                message.find("the mount may still be moving") != std::string_view::npos) {
                ++stop_errors;
            }
        });

    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->silent_from_goto.store(true);
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return stop_errors.load() > 0; }, 10000));
    CHECK(stop_errors.load() == 1);
    CHECK(st->silent.load());  // the handset is still silent
    CHECK_FALSE(driver->get_at_park());
    CHECK_FALSE(driver->get_at_home());
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Slewing must surface the failed Park");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Park slew dispatch failed") != std::string::npos);
        CHECK(std::string(ex.what()).find("stopping after failure failed") != std::string::npos);
    }

    driver->set_connected(false);
}

TEST_CASE("Celestron async - Park completion poll failure persists through Slewing",
          "[celestron][telescope][async][parkfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->silent.store(true);
    std::string message;
    // The poll's link fault may be latched before the park body stores its own error: wait for the stored one.
    REQUIRE(advance_until(
        clock,
        [&] {
            return read_slewing_error(*driver, &message) &&
                   message.find("Park slew status polling failed") != std::string::npos;
        },
        2, std::chrono::milliseconds(250), 48));
    CHECK(read_slewing_error(*driver));
    // The failed polls also latched the link fault (decision 0009 rule 1): AtPark refuses with its text.
    require_alpaca_error([&] { (void)driver->get_at_park(); }, alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - invalid home-status replies persist through Slewing and AtHome",
          "[celestron][telescope][async][home]") {
    auto st = make_handset();
    st->invalid_home_status.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The status poll completes a FindHome (getters only serve its result): pause its own ticks and drive it.
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE_NOTHROW(driver->find_home());
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_at_home());  // first failed reply is treated as a transient
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(driver->get_slewing());  // the second consecutive failure still keeps homing in progress
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    try {
        static_cast<void>(driver->get_at_home());
        FAIL("AtHome returned normally after three consecutive completion polls failed");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("FindHome completion polling failed") != std::string::npos);
    }
    CHECK(st->home_status_polls.load() == 3);
    CHECK(st->cancel_goto_count.load() == 1);
    CHECK(read_slewing_error(*driver));
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - a transient home-status failure recovers", "[celestron][telescope][async][home]") {
    auto st = make_handset();
    st->invalid_next_home_status.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE_NOTHROW(driver->find_home());
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_at_home());
    CHECK(st->cancel_goto_count.load() == 0);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(driver->get_at_home());  // a valid reply clears the consecutive failure count
    CHECK(st->home_status_polls.load() == 3);
    CHECK(st->cancel_goto_count.load() == 0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - a failed home-status check ends the tick", "[celestron][telescope][async][home]") {
    auto st = make_handset();
    st->invalid_next_home_status.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE_NOTHROW(driver->find_home());
    const int before = st->transactions.load();
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    // On a dead link the sample's own exchanges would add timeouts past the one the failed check already paid.
    CHECK(st->transactions.load() - before == 1);
    driver->set_connected(false);
}

// #832: a synchronous SlewToCoordinates must notice another client taking the
// mount (AbortSlew, Park, MoveAxis) and throw InvalidOperation instead of
// returning success, and must not run its completion tail (the T2 tracking
// restore) over the superseding motion.
TEST_CASE("Celestron sync slew - superseded by AbortSlew, Park or MoveAxis throws InvalidOperation",
          "[celestron][telescope][async]") {
    using alpacacore::AlpacaException;
    struct Case {
        const char* name;
        std::function<void(alpacacore::TelescopeDriver&)> supersede;
    };
    const Case cases[] = {
        {"AbortSlew", [](alpacacore::TelescopeDriver& d) { d.abort_slew(); }},
        {"Park", [](alpacacore::TelescopeDriver& d) { d.park(); }},
        {"MoveAxis", [](alpacacore::TelescopeDriver& d) { d.move_axis(0, 0.5); }},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        st->tracking_on.store(true);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        std::atomic<bool> done{false};
        std::atomic<int> code{-1};
        std::atomic<int> t_at_exit{-1};
        std::thread slewer([&] {
            try {
                driver->slew_to_coordinates(5.0, 20.0);
                code.store(0);
            } catch (const AlpacaException& e) {
                code.store(static_cast<int>(e.error_code()));
            }
            t_at_exit.store(st->t_count.load());
            done.store(true);
        });
        REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO is on the wire
        c.supersede(*driver);
        const int t_after_supersede = st->t_count.load();
        // The slewer sleeps on the task clock between polls; step it until it has noticed.
        const bool finished = advance_until(clock, [&] { return done.load(); }, 2);
        if (!finished &&
            !clock.drain_until([&] { return done.load(); }, std::chrono::seconds(1), std::chrono::seconds(10))) {
            std::terminate();  // the slewer is stuck off the clock: joining it would hang
        }
        slewer.join();
        REQUIRE(finished);
        CHECK(code.load() == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        CHECK(t_at_exit.load() == t_after_supersede);  // no tracking restore after the supersede
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron sync slew - uncontended slew returns and runs the tracking restore",
          "[celestron][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->tracking_on.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int t_before = st->t_count.load();
    std::atomic<bool> done{false};
    std::atomic<bool> threw{false};
    std::thread slewer([&] {
        try {
            driver->slew_to_coordinates(5.0, 20.0);
        } catch (const std::exception&) {
            threw.store(true);
        }
        done.store(true);
    });
    const bool finished = advance_until(clock, [&] { return done.load(); }, 2);
    if (!finished &&
        !clock.drain_until([&] { return done.load(); }, std::chrono::seconds(1), std::chrono::seconds(10))) {
        std::terminate();  // the slewer is stuck off the clock: joining it would hang
    }
    slewer.join();
    REQUIRE(finished);
    REQUIRE_FALSE(threw.load());
    CHECK(st->goto_count.load() == 1);
    CHECK(st->t_count.load() > t_before);  // the completion tail restored tracking
    driver->set_connected(false);
}

// ---- Initiator supersede (decision 0006, "Claim"; upstream #1406) ----
//
// A new motion initiator replaces the body in flight instead of cancelling and joining it. The protocol wrapper holds
// its lock across each exchange, so an initiator that sends a command of its own (MoveAxis, AbortSlew, Unpark, and a
// slew or Park for their J read) waits out the one exchange in flight whatever the driver does. What it must not wait
// for is the old body. That cannot be seen from the exchange alone, because a joined body returns right after the
// held exchange does, so the cases below make the old body's thread outlive its body: the slot's spawn seam runs the
// body and then sleeps kTailMs. A join waits out that tail; a claim does not. The exchange is held kHoldMs as well,
// so the cases also cover an old body that is inside the wire when the new initiator arrives.
namespace cel = alpacacore::vendor::celestron;

namespace {

constexpr int kHoldMs = 800;
constexpr int kTailMs = 2500;
// An initiator may wait out the held exchange and its own few commands (the margin covers a slow or sanitized
// runner), never the old body's tail.
constexpr long kPastHoldMarginMs = 1200;

long ms_since(Clock::time_point t0) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

struct SupersedeRig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeCelestronHandset> st = make_handset(clock);
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    std::atomic<int> bodies_done{0};  // slew-slot bodies that have returned (counted by the spawn seam)
    explicit SupersedeRig(int response_timeout_ms = 6000) {
        st->tracking_on.store(true);  // a slew needs tracking on
        server = std::make_unique<alpacacore::test::FakeMountServer>(st->responder());
        REQUIRE(server->ok());
        driver = make_driver(server->port(), clock, response_timeout_ms);  // default: longer than the hold
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        cel::set_status_poll_paused_for_testing(*driver, true);  // the poll sends no position read of its own
    }
    // The first body the slew slot starts keeps its thread alive tail_ms after the body returned; every body that
    // returns is counted in bodies_done.
    void keep_first_body_thread_alive(int tail_ms) {
        auto spawns = std::make_shared<std::atomic<int>>(0);
        cel::set_slew_spawn_for_testing(*driver, [this, spawns, tail_ms](std::function<void()> body) {
            const bool first = spawns->fetch_add(1) == 0;
            return std::thread([this, body = std::move(body), first, tail_ms] {
                body();
                bodies_done.fetch_add(1);
                if (first && tail_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(tail_ms));
            });
        });
    }
    ~SupersedeRig() { driver->set_connected(false); }
};

// Runs `first`, waits until the fake is inside the held GOTO exchange, then times `second` from another thread.
// Returns how long `second` took past the end of the hold.
long time_second_initiator(const char* name, const std::function<void(alpacacore::TelescopeDriver&)>& first,
                           const std::function<void(alpacacore::TelescopeDriver&)>& second) {
    SupersedeRig rig;
    rig.keep_first_body_thread_alive(kTailMs);
    rig.st->gate_ms.store(kHoldMs);
    first(*rig.driver);
    REQUIRE(wait_until([&] { return rig.st->gate_entered.load() != 0; }, 5000));
    std::atomic<long> past_hold{-1};
    std::atomic<bool> threw{false};
    std::thread t([&] {
        try {
            second(*rig.driver);
        } catch (...) {
            threw = true;
        }
        const auto returned = Clock::now();
        const auto left = rig.st->gate_left.load();
        if (left != 0) {
            past_hold = std::chrono::duration_cast<std::chrono::milliseconds>(returned -
                                                                              Clock::time_point(Clock::duration(left)))
                            .count();
        } else {
            past_hold = 0;  // returned inside the hold
        }
    });
    t.join();
    std::printf(
        "initiator %-16s hold=%d ms, old body thread alive %d ms past its body, returned %ld ms past the hold, "
        "threw=%d\n",
        name, kHoldMs, kTailMs, past_hold.load(), threw.load() ? 1 : 0);
    std::fflush(stdout);
    CHECK_FALSE(threw.load());
    return past_hold.load();
}

}  // namespace

TEST_CASE("Celestron initiator supersede - MoveAxis does not join the old slew body",
          "[celestron][telescope][initiator-supersede]") {
    CHECK(time_second_initiator(
              "slew->MoveAxis", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
              [](auto& d) { d.move_axis(0, 0.5); }) < kPastHoldMarginMs);
}

TEST_CASE("Celestron initiator supersede - AbortSlew does not join the old slew body",
          "[celestron][telescope][initiator-supersede]") {
    CHECK(time_second_initiator(
              "slew->AbortSlew", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
              [](auto& d) { d.abort_slew(); }) < kPastHoldMarginMs);
}

TEST_CASE("Celestron initiator supersede - Unpark does not join the old Park body",
          "[celestron][telescope][initiator-supersede]") {
    CHECK(time_second_initiator(
              "park->Unpark", [](auto& d) { d.park(); }, [](auto& d) { d.unpark(); }) < kPastHoldMarginMs);
}

TEST_CASE("Celestron initiator supersede - AbortSlew does not join the old Park body",
          "[celestron][telescope][initiator-supersede]") {
    CHECK(time_second_initiator(
              "park->AbortSlew", [](auto& d) { d.park(); }, [](auto& d) { d.abort_slew(); }) < kPastHoldMarginMs);
}

// Disconnect keeps cancel-and-join (decision 0006, rules 3 and 5): the body's thread must be gone when it returns.
TEST_CASE("Celestron initiator supersede - disconnect still joins the old slew body",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    rig.keep_first_body_thread_alive(kTailMs);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    const auto t0 = Clock::now();
    rig.driver->set_connected(false);
    CHECK(ms_since(t0) >= kTailMs - 200);
}

// The new owner's flags survive the old body (decision 0006, rule 6). The old slew body is parked on its first poll
// wait; once MoveAxis owns the axes the clock steps past the GOTO's end, which would take the old body to its tail:
// the tracking restore ('T'), the position read and a Slewing flip. A superseded body does none of these. (Passes
// today too, since the join stops the body before MoveAxis; it guards the claim.)
TEST_CASE("Celestron initiator supersede - a superseded slew body sends nothing after MoveAxis",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    rig.keep_first_body_thread_alive(0);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    rig.driver->move_axis(0, 0.5);
    const int reads = rig.st->position_reads.load();
    const int tracking_writes = rig.st->t_count.load();
    const int gotos = rig.st->goto_count.load();
    for (int i = 0; i < 12; ++i) {  // 3 s of virtual time: past the GOTO and the start grace
        rig.clock.advance(std::chrono::milliseconds(250));
        rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    REQUIRE(wait_until([&] { return rig.bodies_done.load() == 1; }, 5000));  // the old body has returned
    CHECK(rig.st->position_reads.load() == reads);
    CHECK(rig.st->t_count.load() == tracking_writes);
    CHECK(rig.st->goto_count.load() == gotos);
}

// A second slew owns the target and the Slewing flag; the first body, held inside its GOTO exchange when the second
// slew arrived, must not write over them when its exchange returns.
TEST_CASE("Celestron initiator supersede - a held slew body does not overwrite the next slew's flags",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    rig.st->gate_ms.store(kHoldMs);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->gate_entered.load() != 0; }, 5000));
    rig.driver->slew_to_coordinates_async(6.5, 25.0);
    REQUIRE(wait_until([&] { return rig.st->gate_left.load() != 0; }, 5000));
    for (int i = 0; i < 4; ++i) {  // the new body's first polls; the old body, if it ran on, would write here
        rig.clock.advance(std::chrono::milliseconds(250));
        rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    CHECK(rig.driver->get_target_right_ascension() == Catch::Approx(6.5));
    CHECK(rig.driver->get_target_declination() == Catch::Approx(25.0));
    CHECK(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
}

// ---- A body held after its GOTO exchange (the body hook) ----
//
// `start()` joins a body that has already returned, so a tail after the body proves nothing for an initiator that
// starts a body of its own (a slew, a Park). These cases hold the body itself, inside the hook that runs right after
// its GOTO exchange, with mutex_ free. An initiator that joins the body cannot return until the hook is released; one
// that claims the slot returns at once. After the release the body must see that it was Superseded (never Cancelled)
// and write nothing.
namespace {

constexpr int kInitiatorReturnMs = 2500;

struct HeldBody {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    std::atomic<int> reason{-1};  // the body's StopReason once released
    void release() {
        {
            std::lock_guard<std::mutex> lock(m);
            released = true;
        }
        cv.notify_all();
    }
    bool wait_entered(int ms) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return entered; });
    }
    ~HeldBody() { release(); }
};

// Releases the held body when a case ends early (a failed REQUIRE), so the rig's disconnect cannot hang on it.
struct ReleaseOnExit {
    std::shared_ptr<HeldBody> held;
    ~ReleaseOnExit() { held->release(); }
};

// Only the first body that enters the hook is held and records its reason; a later body (the new owner's) returns at
// once, so it cannot overwrite the result.
void hold_slew_body(SupersedeRig& rig, const std::shared_ptr<HeldBody>& held) {
    auto entries = std::make_shared<std::atomic<int>>(0);
    cel::set_slew_body_hook_for_testing(*rig.driver, [held, entries](const cel::SlewBodyStopReason& stop_reason) {
        if (entries->fetch_add(1) != 0) return;
        std::unique_lock<std::mutex> lock(held->m);
        held->entered = true;
        held->cv.notify_all();
        held->cv.wait(lock, [&] { return held->released; });
        held->reason.store(static_cast<int>(stop_reason()));
    });
}

// Runs `first`, waits until its body is held after the GOTO, runs `second` on another thread and checks that it
// returned while the body was still held; then releases the body and returns what it saw as its stop reason.
int second_initiator_with_held_body(const std::function<void(alpacacore::TelescopeDriver&)>& first,
                                    const std::function<void(alpacacore::TelescopeDriver&)>& second, SupersedeRig& rig,
                                    const std::shared_ptr<HeldBody>& held) {
    rig.keep_first_body_thread_alive(0);
    hold_slew_body(rig, held);
    ReleaseOnExit release_on_exit{held};
    first(*rig.driver);
    REQUIRE(held->wait_entered(5000));
    std::atomic<bool> returned{false};
    std::atomic<bool> threw{false};
    std::thread t([&] {
        try {
            second(*rig.driver);
        } catch (...) {
            threw = true;
        }
        returned = true;
    });
    const bool in_time = wait_until([&] { return returned.load(); }, kInitiatorReturnMs);
    CHECK(in_time);  // false: the initiator waited for the body
    held->release();
    t.join();
    CHECK_FALSE(threw.load());
    REQUIRE(wait_until([&] { return rig.bodies_done.load() >= 1; }, 5000));
    return held->reason.load();
}

constexpr int kSuperseded = static_cast<int>(alpacacore::util::StopReason::Superseded);

}  // namespace

TEST_CASE("Celestron initiator supersede - a new slew supersedes the held slew body without joining it",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
                                          [](auto& d) { d.slew_to_coordinates_async(6.5, 25.0); }, rig,
                                          held) == kSuperseded);
}

TEST_CASE("Celestron initiator supersede - Park supersedes the held slew body without joining it",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
                                          [](auto& d) { d.park(); }, rig, held) == kSuperseded);
}

TEST_CASE("Celestron initiator supersede - MoveAxis ends the held slew body Superseded",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
                                          [](auto& d) { d.move_axis(0, 0.5); }, rig, held) == kSuperseded);
}

TEST_CASE("Celestron initiator supersede - AbortSlew ends the held slew body Superseded",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
                                          [](auto& d) { d.abort_slew(); }, rig, held) == kSuperseded);
}

TEST_CASE("Celestron initiator supersede - Unpark ends the held Park body Superseded",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.park(); }, [](auto& d) { d.unpark(); }, rig, held) ==
          kSuperseded);
}

TEST_CASE("Celestron initiator supersede - AbortSlew ends the held Park body Superseded",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.park(); }, [](auto& d) { d.abort_slew(); }, rig, held) ==
          kSuperseded);
}

// A Park body superseded inside its GOTO exchange writes no target and clears no home state afterwards (the dispatch
// writes them after the exchange; decision 0006, rule 6). The target checks alone also pass on the base (AbortSlew
// clears them after the join); the case is red on the in-time and stop-reason checks and guards the writes once the
// join is gone.
TEST_CASE("Celestron initiator supersede - AbortSlew during a held Park GOTO leaves the targets unset",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto held = std::make_shared<HeldBody>();
    CHECK(second_initiator_with_held_body([](auto& d) { d.park(); }, [](auto& d) { d.abort_slew(); }, rig, held) ==
          kSuperseded);
    // The superseded dispatch wrote no target: the getters throw ValueNotSet, not some other failure.
    CHECK(error_code_of([&] { (void)rig.driver->get_target_right_ascension(); }) ==
          alpacacore::AlpacaError::ValueNotSet);
    CHECK(error_code_of([&] { (void)rig.driver->get_target_declination(); }) == alpacacore::AlpacaError::ValueNotSet);
}

// ---- The Park body's dispatch writes state around two reads (decision 0006, rule 6) ----
//
// do_slew_to_coordinates() reads J and the pier side with mutex_ free, then publishes Slewing, clears the stored
// errors and sends the GOTO. The dispatch hook sits after both reads and the publish, just before the GOTO. An
// initiator that takes the Park over while the body is there owns the state: after the release the body sends no GOTO
// and leaves Slewing, AtPark and the stored error as the claimer left them.
namespace {

void park_dispatch_held_then(const std::function<void(alpacacore::TelescopeDriver&)>& takeover) {
    SupersedeRig rig;
    rig.keep_first_body_thread_alive(0);
    auto held = std::make_shared<HeldBody>();
    ReleaseOnExit release_on_exit{held};
    cel::set_slew_dispatch_hook_for_testing(*rig.driver, [held] {
        std::unique_lock<std::mutex> lock(held->m);
        held->entered = true;
        held->cv.notify_all();
        held->cv.wait(lock, [&] { return held->released; });
    });
    rig.driver->park();
    REQUIRE(held->wait_entered(5000));
    std::atomic<bool> returned{false};
    std::thread t([&] {
        try {
            takeover(*rig.driver);
        } catch (...) {
        }
        returned = true;
    });
    CHECK(wait_until([&] { return returned.load(); }, kInitiatorReturnMs));
    held->release();
    t.join();
    REQUIRE(wait_until([&] { return rig.bodies_done.load() >= 1; }, 5000));
    CHECK(rig.st->goto_count.load() == 0);
    CHECK_FALSE(rig.driver->get_at_park());
    (void)abort_verified(rig.clock, *rig.driver);  // an AbortSlew takeover holds Slewing until its stop verifies
    bool slewing = true;
    CHECK_NOTHROW(slewing = rig.driver->get_slewing());  // a stored error would make this throw
    CHECK_FALSE(slewing);
}

}  // namespace

TEST_CASE("Celestron initiator supersede - AbortSlew over a Park body in its dispatch keeps its state",
          "[celestron][telescope][initiator-supersede]") {
    park_dispatch_held_then([](auto& d) { d.abort_slew(); });
}

TEST_CASE("Celestron initiator supersede - Unpark over a Park body in its dispatch keeps its state",
          "[celestron][telescope][initiator-supersede]") {
    park_dispatch_held_then([](auto& d) { d.unpark(); });
}

// An Unpark with no park in flight sends nothing and starts nothing, so it claims nothing: a slew body that failed
// and is inside its stops finishes its own failure, and Slewing reads false once the stops are answered. (An Unpark
// that superseded the failing body would lose the body's Slewing=false write and leave Slewing true with no owner.)
TEST_CASE("Celestron initiator supersede - Unpark does not take over a failing slew body",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig(1200);  // the swallowed GOTO times out after 1.2 s; the held stop (0.8 s) is answered in time
    rig.st->reject_goto.store(true);
    rig.st->stop_gate_ms.store(800);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->stop_gate_entered.load() != 0; }, 15000));  // the body is in its stop
    rig.st->reject_goto.store(false);
    REQUIRE_NOTHROW(rig.driver->unpark());
    CHECK(rig.st->stop_gate_left.load() == 0);  // returned while the stop was still held: no join
    REQUIRE(wait_until([&] { return rig.st->stop_gate_left.load() != 0; }, 5000));
    CHECK(wait_until(
        [&] {
            try {
                return !rig.driver->get_slewing();
            } catch (...) {
                return false;
            }
        },
        5000));
}

// AbortSlew supersedes the Park body, which then leaves the park state alone, so a failed stop must end the park
// itself: nothing is left to complete it (the #1484 review-bot defect A). The link drops once the park GOTO is out,
// so every stop fails.
TEST_CASE("Celestron initiator supersede - a failed AbortSlew during a Park ends the park",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    rig.driver->park();
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    REQUIRE(rig.server->drop_connections());
    rig.st->silent.store(true);  // the reconnect, if the wrapper makes one, gets no answer either
    REQUIRE(error_code_of([&] { rig.driver->abort_slew(); }) == alpacacore::AlpacaError::DriverException);
    // A park that is still recorded as in flight refuses a slew with InvalidWhileParked; on the dropped link it fails
    // on the wire instead.
    CHECK(error_code_of([&] { rig.driver->slew_to_coordinates_async(5.5, 20.0); }) !=
          alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(error_code_of([&] { rig.driver->move_axis(0, 0.5); }) != alpacacore::AlpacaError::InvalidWhileParked);
}

// ---- The Park body inside its J read (decision 0006, rule 6) ----
//
// The J read is the first thing do_slew_to_coordinates() does, before it publishes Slewing and clears the stored
// errors. A claim that lands there must leave all of that alone: after the J answer the body checks it still owns the
// park and returns without a write or a GOTO. The hold is on the wire (the fake answers the second J read late), so
// the takeover waits out the one exchange in flight and nothing else.
namespace {

struct JHeldOutcome {
    bool takeover_threw = false;
    int goto_count = -1;
    bool at_park = true;
    bool slewing_threw = false;
    bool slewing = true;
    std::string slewing_message;
};

JHeldOutcome park_held_in_j_read_then(const std::function<void(alpacacore::TelescopeDriver&)>& takeover,
                                      bool stops_fail) {
    SupersedeRig rig(1200);
    rig.keep_first_body_thread_alive(0);
    rig.st->j_hold_skip.store(1);  // the initiator's own J read is the first; the body's is the held one
    rig.st->j_hold_ms.store(kHoldMs);
    rig.driver->park();
    REQUIRE(wait_until([&] { return rig.st->j_hold_entered.load() != 0; }, 5000));
    rig.st->reject_stop.store(stops_fail);
    JHeldOutcome out;
    try {
        takeover(*rig.driver);
    } catch (...) {
        out.takeover_threw = true;
    }
    REQUIRE(wait_until([&] { return rig.bodies_done.load() >= 1; }, 5000));
    out.goto_count = rig.st->goto_count.load();
    out.at_park = rig.driver->get_at_park();
    (void)abort_verified(rig.clock, *rig.driver);  // an AbortSlew takeover holds Slewing until its stop verifies
    try {
        out.slewing = rig.driver->get_slewing();
    } catch (const std::exception& ex) {
        out.slewing_threw = true;
        out.slewing_message = ex.what();
    }
    return out;
}

}  // namespace

TEST_CASE("Celestron initiator supersede - AbortSlew over a Park body in its J read keeps the stop error",
          "[celestron][telescope][initiator-supersede]") {
    const auto out = park_held_in_j_read_then([](auto& d) { d.abort_slew(); }, true);
    CHECK(out.takeover_threw);  // the stops failed
    CHECK(out.goto_count == 0);
    CHECK_FALSE(out.at_park);
    CHECK(out.slewing_threw);  // the body did not clear the stored error
    CHECK(out.slewing_message.find("AbortSlew stop failed") != std::string::npos);
}

TEST_CASE("Celestron initiator supersede - AbortSlew over a Park body in its J read leaves Slewing false",
          "[celestron][telescope][initiator-supersede]") {
    const auto out = park_held_in_j_read_then([](auto& d) { d.abort_slew(); }, false);
    CHECK_FALSE(out.takeover_threw);
    CHECK(out.goto_count == 0);
    CHECK_FALSE(out.slewing_threw);
    CHECK_FALSE(out.slewing);  // the body did not set Slewing true again
}

TEST_CASE("Celestron initiator supersede - Unpark over a Park body in its J read leaves Slewing false",
          "[celestron][telescope][initiator-supersede]") {
    const auto out = park_held_in_j_read_then([](auto& d) { d.unpark(); }, false);
    CHECK_FALSE(out.takeover_threw);
    CHECK(out.goto_count == 0);
    CHECK_FALSE(out.slewing_threw);
    CHECK_FALSE(out.slewing);
}

// ---- The claim protocol around the send gate (decision 0006 "Claim", Celestron adoption) ----

// The pier read is the Park body's second read in do_slew_to_coordinates(); it sets flip_in_progress_ right after,
// which switches the status poll from the HC "L" query to the per-axis slew-done query. An AbortSlew that lands during
// the read must leave that flag alone. The poll is the public view of it: after the body returns, a tick must still use
// "L". One of the two pier sides needs a flip whatever the time of day, so both are run.
TEST_CASE("Celestron initiator supersede - AbortSlew over a Park body in its pier read leaves no flip state",
          "[celestron][telescope][initiator-supersede]") {
    for (const char side : {'W', 'E'}) {
        SupersedeRig rig(1200);
        rig.keep_first_body_thread_alive(0);
        rig.st->pier_side.store(side);
        rig.st->pier_hold_ms.store(kHoldMs);
        rig.driver->park();
        REQUIRE(wait_until([&] { return rig.st->pier_hold_entered.load() != 0; }, 5000));
        REQUIRE_NOTHROW(rig.driver->abort_slew());
        REQUIRE(wait_until([&] { return rig.bodies_done.load() >= 1; }, 5000));
        CHECK(rig.st->goto_count.load() == 0);
        REQUIRE(abort_verified(rig.clock, *rig.driver));  // the verified stop's reads end before the counts below
        cel::set_status_poll_paused_for_testing(*rig.driver, false);
        const int slew_done_before = rig.st->slew_done_count.load();
        const int l_before = rig.st->l_count.load();
        CHECK(cel::poll_status_now_for_testing(*rig.driver));
        CHECK(rig.st->slew_done_count.load() == slew_done_before);  // a stale flip would poll the axes
        CHECK(rig.st->l_count.load() > l_before);
        CHECK_FALSE(rig.driver->get_slewing());
    }
}

// The send gate keeps a queued GOTO from passing a later stop: whichever way the race between the body and the
// AbortSlew falls, the GOTO is either never sent or sent before the cancel (16 short trials).
TEST_CASE("Celestron initiator supersede - a queued goto never reaches the mount after AbortSlew",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    rig.keep_first_body_thread_alive(0);
    for (int trial = 0; trial < 16; ++trial) {
        rig.st->goto_last_seq.store(0);
        rig.st->cancel_first_seq.store(0);
        rig.driver->slew_to_coordinates_async(5.5 + 0.01 * trial, 20.0);
        REQUIRE_NOTHROW(rig.driver->abort_slew());
        REQUIRE(wait_until([&] { return rig.bodies_done.load() >= trial + 1; }, 5000));
        INFO("trial " << trial);
        REQUIRE(rig.st->cancel_first_seq.load() > 0);
        CHECK((rig.st->goto_last_seq.load() == 0 || rig.st->goto_last_seq.load() < rig.st->cancel_first_seq.load()));
    }
}

// A superseded body whose GOTO fails (the handset swallows it; the claim lands during the wait) sends no failure stop
// and stores no failure: the new owner has the mount. The only cancels that go out are the new owner's: AbortSlew
// sends one, MoveAxis none.
namespace {

void superseded_body_fails_then(const std::function<void(alpacacore::TelescopeDriver&)>& first,
                                const std::function<void(alpacacore::TelescopeDriver&)>& takeover,
                                int expected_cancels) {
    SupersedeRig rig(1200);
    rig.keep_first_body_thread_alive(0);
    rig.st->reject_goto.store(true);
    first(*rig.driver);
    REQUIRE(wait_until([&] { return rig.st->goto_requests.load() >= 1; }, 5000));
    REQUIRE_NOTHROW(takeover(*rig.driver));  // the claim lands while the swallowed GOTO times out
    REQUIRE(wait_until([&] { return rig.bodies_done.load() >= 1; }, 8000));
    if (expected_cancels > 0) (void)abort_verified(rig.clock, *rig.driver);  // AbortSlew's stop verifies (#2409)
    CHECK(rig.st->cancel_goto_count.load() == expected_cancels);
    bool slewing = true;
    CHECK_NOTHROW(slewing = rig.driver->get_slewing());  // a stored failure would make this throw
    CHECK(slewing == (expected_cancels == 0));           // MoveAxis leaves its axis moving; AbortSlew ends it
}

}  // namespace

TEST_CASE("Celestron initiator supersede - a superseded Park body's failure stops nothing and stores nothing",
          "[celestron][telescope][initiator-supersede]") {
    superseded_body_fails_then([](auto& d) { d.park(); }, [](auto& d) { d.abort_slew(); }, 1);
}

TEST_CASE("Celestron initiator supersede - a superseded slew body whose dispatch throws leaves the error alone",
          "[celestron][telescope][initiator-supersede]") {
    superseded_body_fails_then([](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
                               [](auto& d) { d.move_axis(0, 0.5); }, 0);
}

// A refused start after the claim superseded a body in flight stops the mount and publishes the AbortSlew state: the
// old motion may run on with nobody left to stop it.
TEST_CASE("Celestron initiator supersede - a refused start after a claim stops the mount",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    auto spawns = std::make_shared<std::atomic<int>>(0);
    cel::set_slew_spawn_for_testing(*rig.driver, [spawns](std::function<void()> body) -> std::thread {
        if (spawns->fetch_add(1) == 0) return std::thread(std::move(body));
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    CHECK_THROWS(rig.driver->slew_to_coordinates_async(6.5, 25.0));
    CHECK(rig.st->cancel_goto_count.load() >= 1);  // the old body's motion was stopped
    CHECK_FALSE(rig.driver->get_slewing());
}

// learn_ra_offset() commits a measurement after a read that ran with mutex_ free; a body that lost the slot meanwhile
// must not commit it. The offset is visible through the GOTO text of the next slew to the same target: slew 1 learns
// (control: the text changes), slew 2 is superseded during the learning read (the text stays), slew 3 shows it.
TEST_CASE("Celestron initiator supersede - a superseded slew body does not learn the RA offset",
          "[celestron][telescope][initiator-supersede]") {
    SupersedeRig rig;
    rig.keep_first_body_thread_alive(0);
    constexpr double kTargetRa = 1.750129 + 0.0003;  // 0.0003 h from what the fake mount reports: within the 30" limit
    const auto finish = [&](int bodies) {
        for (int i = 0; i < 60 && rig.bodies_done.load() < bodies; ++i) {
            // The paused poll and the body: the poll alone lets the steps run out before a late body (#1949).
            if (!rig.clock.wait_for_waiters(2, std::chrono::seconds(5))) break;
            rig.clock.advance(std::chrono::milliseconds(250));
            rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
        }
        return rig.bodies_done.load() >= bodies;
    };
    rig.driver->slew_to_coordinates_async(kTargetRa, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_requests.load() >= 1; }, 5000));
    const std::string before_learning = rig.st->goto_text();
    REQUIRE(finish(1));  // the tail learns the offset
    rig.driver->slew_to_coordinates_async(kTargetRa, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_requests.load() >= 2; }, 5000));
    const std::string after_learning = rig.st->goto_text();
    CHECK(after_learning != before_learning);
    // Slew 2: hold its learning read (the first position read after the tail starts), take the slot with MoveAxis.
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // the paused poll and slew 2's body (#1949)
    rig.st->e_hold_ms.store(1500);
    std::thread stepper([&] {  // steps the clock to the tail; its settle wait outlasts the hold, so it runs apart
        for (int i = 0; i < 60 && rig.st->e_hold_ms.load() != 0; ++i) {
            rig.clock.wait_for_waiters(1, std::chrono::milliseconds(100));
            rig.clock.advance(std::chrono::milliseconds(250));
            rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
        }
    });
    const bool in_flight = wait_until([&] { return rig.st->e_hold_ms.load() == 0; }, 15000);
    REQUIRE(in_flight);  // the learning read is in flight, held
    REQUIRE_NOTHROW(rig.driver->move_axis(0, 0.5));
    stepper.join();
    REQUIRE(wait_until([&] { return rig.bodies_done.load() >= 2; }, 5000));
    rig.driver->slew_to_coordinates_async(kTargetRa, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_requests.load() >= 3; }, 5000));
    CHECK(rig.st->goto_text() == after_learning);  // slew 2's measurement was dropped
}

// Case A: park GOTO acked, mount never moves, 3 h of RA (45 deg) from the saved park position.
TEST_CASE("Celestron case A - refused park GOTO must not report AtPark", "[park][celestron]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_park();        // park target = position variant 0
    st->set_position("32AB0500,20000500#");  // mount now reads 3 h of RA away from it
    st->no_motion.store(true);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }, 2));
    const bool err = read_slewing_error(*driver);
    bool at_park = false;
    try {
        at_park = driver->get_at_park();
    } catch (const alpacacore::AlpacaException&) {
    }
    CHECK(st->goto_count.load() == 1);
    CHECK_FALSE(at_park);
    CHECK(err);
    driver->set_connected(false);
}

// Case B (control): mount already at the saved park position, GOTO acked, never slews.
TEST_CASE("Celestron case B - already at park target still parks", "[park][celestron]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_park();
    st->no_motion.store(true);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }, 2));
    bool at_park = false;
    try {
        at_park = driver->get_at_park();
    } catch (const alpacacore::AlpacaException&) {
    }
    CHECK(at_park);
    CHECK_FALSE(read_slewing_error(*driver));
    driver->set_connected(false);
}

// ---- Motion owner (#2263, decision 0006 rule 3) ----
//
// A motion command owns only the axes it moves, and it supersedes the body it replaces instead of cancelling and
// joining it: a MoveAxis takes the pulse on its own axis, a PulseGuide the older pulse on its axis, a slew, Park or
// FindHome both pulses. A pulse body that is parked between chunks returns as soon as it is woken, so a join cannot
// be seen from the call alone; as in the initiator-supersede cases above, the pulse slot's spawn seam keeps the old
// body's thread alive kTailMs after its body returned. A join waits out that tail; a supersede does not.

namespace {

// The first pulse body started keeps its thread alive tail_ms after the body returned.
void keep_first_pulse_thread_alive(alpacacore::TelescopeDriver& driver, int tail_ms) {
    auto spawns = std::make_shared<std::atomic<int>>(0);
    cel::set_pulse_spawn_for_testing(driver, [spawns, tail_ms](std::function<void()> body) {
        const bool first = spawns->fetch_add(1) == 0;
        return std::thread([body = std::move(body), first, tail_ms] {
            body();
            if (first) std::this_thread::sleep_for(std::chrono::milliseconds(tail_ms));
        });
    });
}

// Steps the clock `span` in 250 ms steps, each settled.
void step_virtual(alpacacore::test::FakeTaskClock& clock, std::chrono::milliseconds span) {
    for (auto done = std::chrono::milliseconds(0); done < span; done += std::chrono::milliseconds(250)) {
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
}

// A long RA pulse: 255 cs now, the chain body then waits 2.55 s on the clock for its next chunk.
std::size_t start_long_ra_pulse(SupersedeRig& rig) {
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the paused poll, parked
    const std::size_t idle = rig.clock.waiter_count();
    keep_first_pulse_thread_alive(*rig.driver, kTailMs);
    rig.driver->pulse_guide(2, 25500);
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, std::chrono::seconds(5)));  // the chain body, between chunks
    REQUIRE(rig.st->guide_command_count(0) == 1);
    return idle;
}

}  // namespace

TEST_CASE("Celestron motion owner - MoveAxis supersedes the pulse on its axis without joining it (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    SupersedeRig rig;
    start_long_ra_pulse(rig);
    const auto t0 = Clock::now();
    rig.driver->move_axis(0, 0.5);
    const long took = ms_since(t0);
    std::printf("MoveAxis over an RA pulse took %ld ms (old pulse thread alive %d ms past its body)\n", took, kTailMs);
    CHECK(took < kPastHoldMarginMs);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());  // the pulse ended when the MoveAxis took RA
    CHECK(rig.driver->get_slewing());
    // Past the chain's next chunk (2.55 s): the superseded body sent RA nothing more.
    step_virtual(rig.clock, std::chrono::milliseconds(3000));
    CHECK(rig.st->guide_command_count(0) == 1);
    rig.driver->move_axis(0, 0.0);
}

TEST_CASE("Celestron motion owner - a second PulseGuide supersedes the older pulse on its axis without a join (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    SupersedeRig rig;
    start_long_ra_pulse(rig);
    const auto t0 = Clock::now();
    rig.driver->pulse_guide(2, 1000);  // one chunk of 100 cs, no chain
    const long took = ms_since(t0);
    std::printf("PulseGuide over an RA pulse took %ld ms (old pulse thread alive %d ms past its body)\n", took,
                kTailMs);
    CHECK(took < kPastHoldMarginMs);
    CHECK(rig.driver->get_is_pulse_guiding());
    // Past the old chain's next chunk: only the two first chunks went out, and the newer pulse (1 s plus the 1 s
    // completion delay) has ended; the older 25.5 s window ended with it.
    step_virtual(rig.clock, std::chrono::milliseconds(3000));
    CHECK(rig.st->guide_command_count(0) == 2);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

TEST_CASE("Celestron motion owner - a slew, Park or FindHome supersedes a pulse without joining it (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    const int operation = GENERATE(0, 1, 2);
    CAPTURE(operation);
    SupersedeRig rig;
    start_long_ra_pulse(rig);
    const auto t0 = Clock::now();
    if (operation == 0) {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
    } else if (operation == 1) {
        rig.driver->park();
    } else {
        rig.driver->find_home();
    }
    const long took = ms_since(t0);
    std::printf("operation %d over an RA pulse took %ld ms (old pulse thread alive %d ms past its body)\n", operation,
                took, kTailMs);
    CHECK(took < kPastHoldMarginMs);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.driver->get_slewing());
    rig.driver->abort_slew();
    step_virtual(rig.clock, std::chrono::milliseconds(3000));
    CHECK(rig.st->guide_command_count(0) == 1);  // the superseded chain sent nothing more
}

// The policy table (#2263, the maintainer's ruling of 2026-11-16): a PulseGuide while a slew, Park or FindHome runs is
// refused and changes nothing. InvalidOperation for the slew and FindHome; a Park keeps its older InvalidWhileParked
// refusal.
TEST_CASE("Celestron motion owner - PulseGuide during a slew, Park or FindHome is refused and changes nothing (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    const int operation = GENERATE(0, 1, 2);
    CAPTURE(operation);
    SupersedeRig rig;
    REQUIRE(rig.driver->get_can_pulse_guide());
    if (operation == 0) {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
    } else if (operation == 1) {
        rig.driver->park();
    } else {
        rig.driver->find_home();
    }
    REQUIRE(rig.driver->get_slewing());
    for (const int direction : {0, 2}) {  // a Dec pulse and an RA pulse
        CAPTURE(direction);
        bool refused = false;
        try {
            rig.driver->pulse_guide(direction, 500);
        } catch (const alpacacore::AlpacaException& ex) {
            refused = true;
            if (operation == 1) {
                CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
            } else {
                CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
                const std::string text = ex.what();
                CHECK(text.rfind("PulseGuide is not allowed while a slew, park or home is running; ", 0) == 0);
                CHECK(text.find("a failed stop keeps Slewing true") != std::string::npos);
            }
        }
        CHECK(refused);
    }
    CHECK(rig.st->guide_command_count(0) == 0);
    CHECK(rig.st->guide_command_count(1) == 0);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.driver->get_slewing());
    rig.driver->abort_slew();
}

// The refusal ends with the operation, on every way it ends: a landing, an AbortSlew, a FindHome the poll completes,
// a slew start the slot refuses, a reconnect, and a MoveAxis that takes an axis from the slew.
TEST_CASE("Celestron motion owner - PulseGuide is accepted again once the slew, Park or FindHome has ended (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    const int leg = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(leg);
    SupersedeRig rig;
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    if (leg == 0) {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
        REQUIRE(advance_until(rig.clock, [&] { return !rig.driver->get_slewing(); }, 2));
    } else if (leg == 1) {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
        rig.driver->abort_slew();
        // #2409: Slewing, and with it the PulseGuide refusal, holds until the verified stop sees the mount still.
        REQUIRE(abort_verified(rig.clock, *rig.driver));
    } else if (leg == 2) {
        rig.driver->find_home();
        REQUIRE(cel::poll_status_now_for_testing(*rig.driver));  // MC_LEVEL_DONE answers done
        REQUIRE(rig.driver->get_at_home());
    } else if (leg == 3) {
        cel::set_slew_spawn_for_testing(*rig.driver, [](std::function<void()>) -> std::thread {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
        require_alpaca_error([&] { rig.driver->slew_to_coordinates_async(5.5, 20.0); },
                             alpacacore::AlpacaError::DriverException);
    } else if (leg == 4) {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
        rig.driver->set_connected(false);
        REQUIRE(alpacacore::test::settle_connected(*rig.driver, true, std::chrono::seconds(10)));
        cel::set_status_poll_paused_for_testing(*rig.driver, true);
    } else {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
        rig.driver->move_axis(0, 0.5);  // takes RA, and with it the slew, which owned both axes
    }
    CHECK_NOTHROW(rig.driver->pulse_guide(0, 100));
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.driver->abort_slew();
}

// The other axis is not the MoveAxis's: a Dec pulse keeps its chain while RA moves.
TEST_CASE("Celestron motion owner - MoveAxis on one axis leaves a pulse on the other axis running (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    SupersedeRig rig;
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    const std::size_t idle = rig.clock.waiter_count();
    rig.driver->pulse_guide(0, 5000);  // Dec: 255 cs now, 245 cs chained at 2.55 s
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, std::chrono::seconds(5)));
    rig.driver->move_axis(0, 0.5);
    step_virtual(rig.clock, std::chrono::milliseconds(3000));
    REQUIRE(wait_until([&] { return rig.st->guide_command_count(1) == 2; }, 2000));  // the server logs the chunk
    CHECK(rig.st->guide_command_count(0) == 0);
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.driver->move_axis(0, 0.0);
}

namespace {

// Rate-0 frames (fixed 36/37 or variable 6/7, both data bytes zero) sent to the RA (axis 0) or DEC (axis 1) motor. One
// stop can send more than one frame (a variable-rate stop zeroes both directions), so cases compare against a baseline.
int zero_rate_stops(FakeCelestronHandset& st, int axis) {
    const auto device = static_cast<unsigned char>(axis == 0 ? 0x10 : 0x11);
    std::lock_guard<std::mutex> lock(st.command_mutex);
    int n = 0;
    for (const auto& c : st.commands) {
        if (c.size() < 6 || c[0] != 'P' || static_cast<unsigned char>(c[2]) != device) continue;
        const auto op = static_cast<unsigned char>(c[3]);
        if ((op == 6 || op == 7 || op == 36 || op == 37) && c[4] == '\0' && c[5] == '\0') ++n;
    }
    return n;
}

}  // namespace

// #2458: the mount's GOTO drives both axes, so MoveAxis(axis, 0) during an async slew stops both, as AbortSlew does.
// A rate 0 on its own axis alone left the other axis under the GOTO while Slewing read false.
TEST_CASE("Celestron MoveAxis - a one-axis stop during an async slew stops both axes (#2458)",
          "[celestron][telescope][motion-owner]") {
    const int axis = GENERATE(0, 1);
    CAPTURE(axis);
    SupersedeRig rig;
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    REQUIRE(rig.driver->get_slewing());
    const int cancels = rig.st->cancel_goto_count.load();
    const int ra_stops = zero_rate_stops(*rig.st, 0);
    const int dec_stops = zero_rate_stops(*rig.st, 1);
    rig.driver->move_axis(axis, 0.0);
    CHECK(rig.st->cancel_goto_count.load() == cancels + 1);
    CHECK(zero_rate_stops(*rig.st, 0) > ra_stops);
    CHECK(zero_rate_stops(*rig.st, 1) > dec_stops);
    CHECK(abort_verified(rig.clock, *rig.driver));  // #2409: Slewing holds until the stop verifies
    CHECK_FALSE(rig.driver->get_slewing());
}

// A blocking SlewToCoordinates waits with no slew body; its claim on both axes is what marks the GOTO as running, so a
// one-axis stop then stops both axes too, and the waiting slew reports it was superseded.
TEST_CASE("Celestron MoveAxis - a one-axis stop during a blocking slew stops both axes (#2458)",
          "[celestron][telescope][motion-owner]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->tracking_on.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    std::atomic<bool> done{false};
    std::atomic<int> code{-1};
    std::thread slewer([&] {
        try {
            driver->slew_to_coordinates(5.0, 20.0);
            code.store(0);
        } catch (const alpacacore::AlpacaException& e) {
            code.store(static_cast<int>(e.error_code()));
        }
        done.store(true);
    });
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO is on the wire
    const int cancels = st->cancel_goto_count.load();
    const int ra_stops = zero_rate_stops(*st, 0);
    const int dec_stops = zero_rate_stops(*st, 1);
    driver->move_axis(1, 0.0);
    CHECK(st->cancel_goto_count.load() == cancels + 1);
    CHECK(zero_rate_stops(*st, 0) > ra_stops);
    CHECK(zero_rate_stops(*st, 1) > dec_stops);
    const bool finished = advance_until(clock, [&] { return done.load(); }, 2);
    if (!finished &&
        !clock.drain_until([&] { return done.load(); }, std::chrono::seconds(1), std::chrono::seconds(10))) {
        std::terminate();  // the slewer is stuck off the clock: joining it would hang
    }
    slewer.join();
    REQUIRE(finished);
    CHECK(code.load() == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
    CHECK(abort_verified(clock, *driver));  // #2409: Slewing holds until the stop verifies
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// With no GOTO running, a stop is still its own axis's: a jog on the other axis keeps running.
TEST_CASE("Celestron MoveAxis - a one-axis stop with no slew running leaves a jog on the other axis alone (#2458)",
          "[celestron][telescope][motion-owner]") {
    SupersedeRig rig;
    rig.driver->move_axis(1, 0.5);
    const int cancels = rig.st->cancel_goto_count.load();
    const int ra_stops = zero_rate_stops(*rig.st, 0);
    const int dec_stops = zero_rate_stops(*rig.st, 1);
    rig.driver->move_axis(0, 0.0);
    CHECK(rig.st->cancel_goto_count.load() == cancels);
    CHECK(zero_rate_stops(*rig.st, 0) > ra_stops);
    CHECK(zero_rate_stops(*rig.st, 1) == dec_stops);
    CHECK(rig.driver->get_slewing());  // the DEC jog still runs
    rig.driver->move_axis(1, 0.0);
}

// #2503 (the maintainer's decision of 2026-11-22): a PulseGuide on the axis a non-zero MoveAxis drives is refused with
// InvalidOperation before anything goes to the wire, and the jog keeps running. A pulse on the other axis is accepted
// and leaves the jog alone.
TEST_CASE(
    "Celestron motion owner - PulseGuide on the axis a MoveAxis is driving is refused and the jog runs on (#2503)",
    "[celestron][telescope][pulseguide][motion-owner][goto-safety]") {
    const int jog_axis = GENERATE(0, 1);
    CAPTURE(jog_axis);
    const int other_axis = 1 - jog_axis;
    const int jog_direction = jog_axis == 1 ? 0 : 2;  // North moves Dec, East moves RA
    const int other_direction = jog_axis == 1 ? 2 : 0;
    SupersedeRig rig;
    REQUIRE(rig.driver->get_can_pulse_guide());
    rig.driver->move_axis(jog_axis, 0.5);
    const int stops = zero_rate_stops(*rig.st, jog_axis);
    const int requests = rig.st->requests.load();

    std::string message;
    CHECK(error_code_of([&] { rig.driver->pulse_guide(jog_direction, 500); }, &message) ==
          alpacacore::AlpacaError::InvalidOperation);
    CHECK(message == "PulseGuide is not allowed on an axis MoveAxis is moving");
    CHECK(rig.st->requests.load() == requests);  // nothing reached the wire, not even the pier read
    CHECK(rig.st->guide_command_count(jog_axis) == 0);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(zero_rate_stops(*rig.st, jog_axis) == stops);
    CHECK(rig.driver->get_slewing());  // the jog still runs

    CHECK_NOTHROW(rig.driver->pulse_guide(other_direction, 500));
    CHECK(rig.st->guide_command_count(other_axis) == 1);
    CHECK(rig.st->guide_command_count(jog_axis) == 0);
    CHECK(rig.driver->get_is_pulse_guiding());
    CHECK(zero_rate_stops(*rig.st, jog_axis) == stops);
    CHECK(rig.driver->get_slewing());  // the jog still runs
    rig.driver->move_axis(jog_axis, 0.0);
}

// A stop that fails may leave the GOTO running on either axis: the MoveAxis reports the failure, and Slewing raises it
// (as after a failed AbortSlew) rather than reading false.
TEST_CASE("Celestron MoveAxis - a failed one-axis stop during an async slew never reads Slewing false (#2458)",
          "[celestron][telescope][motion-owner]") {
    SupersedeRig rig(500);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    rig.st->reject_stop.store(true);
    require_alpaca_error([&] { rig.driver->move_axis(0, 0.0); }, alpacacore::AlpacaError::DriverException);
    CHECK(rig.st->cancel_goto_count.load() == 0);
    try {
        const bool slewing = rig.driver->get_slewing();
        CHECK(slewing);  // a completion value is acceptable only while it still says moving
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).rfind("MoveAxis stop failed: ", 0) == 0);
    }
    rig.st->reject_stop.store(false);
    rig.driver->abort_slew();
}

// A refused command changes nothing (decision 0006 rule 2): a Park or async slew that the slew-safety gate refuses
// (unaligned, no Sync this session) leaves the running pulse alone. IsPulseGuiding and the chain agree: the window
// stays open and the chain sends its next chunk.
TEST_CASE("Celestron motion owner - a refused Park or async slew leaves a running pulse and its window alone (#2263)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    const int operation = GENERATE(0, 1);
    CAPTURE(operation);
    SupersedeRig rig;
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    const std::size_t idle = rig.clock.waiter_count();
    rig.driver->pulse_guide(2, 25500);  // RA: 255 cs now, the chain body waits 2.55 s for its next chunk
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, std::chrono::seconds(5)));
    REQUIRE(rig.st->guide_command_count(0) == 1);
    rig.st->aligned.store(false);
    if (operation == 0) {
        require_alpaca_error([&] { rig.driver->park(); }, alpacacore::AlpacaError::InvalidOperation);
    } else {
        require_alpaca_error([&] { rig.driver->slew_to_coordinates_async(5.5, 20.0); },
                             alpacacore::AlpacaError::InvalidOperation);
    }
    CHECK(rig.driver->get_is_pulse_guiding());
    CHECK_FALSE(rig.driver->get_slewing());
    step_virtual(rig.clock, std::chrono::milliseconds(3000));  // past the chain's next chunk
    CHECK(wait_until([&] { return rig.st->guide_command_count(0) == 2; }, 2000));
    CHECK(rig.driver->get_is_pulse_guiding());  // the window still covers the chunk that went out
    rig.driver->abort_slew();
}

// A blocking SlewToCoordinates owns the mount until it returns, its settle and tracking restore included: Slewing reads
// false once it lands, but a PulseGuide in the settle is refused and does not supersede the slew (#2263), so the call
// returns success and restores tracking, as it did before the motion owner. A PulseGuide after the call returns is
// accepted.
TEST_CASE(
    "Celestron motion owner - a PulseGuide during a blocking slew's settle is refused and the slew completes (#2263)",
    "[celestron][telescope][pulseguide][motion-owner]") {
    const int leg = GENERATE(0, 1);  // 0: PulseGuide during the settle; 1: right after the call returns
    CAPTURE(leg);
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->tracking_on.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_slew_settle_time(2);
    REQUIRE(driver->get_can_pulse_guide());
    const int t_before = st->t_count.load();
    std::atomic<bool> done{false};
    std::atomic<bool> threw{false};
    std::thread slewer([&] {
        try {
            driver->slew_to_coordinates(5.0, 20.0);
        } catch (const std::exception&) {
            threw.store(true);
        }
        done.store(true);
    });
    const auto finish = [&] {
        const bool finished = advance_until(clock, [&] { return done.load(); }, 2);
        if (!finished &&
            !clock.drain_until([&] { return done.load(); }, std::chrono::seconds(1), std::chrono::seconds(10))) {
            std::terminate();  // the slewer is stuck off the clock: joining it would hang
        }
        slewer.join();
        return finished;
    };
    if (leg == 0) {
        // The GOTO is on the wire and reported moving; then it lands: Slewing reads false while the call is still in
        // its landing read or its settle.
        REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
        REQUIRE(advance_until(clock, [&] { return done.load() || driver->get_slewing(); }, 2));
        REQUIRE(advance_until(clock, [&] { return done.load() || !driver->get_slewing(); }, 2));
        REQUIRE_FALSE(done.load());
        bool refused = false;
        try {
            driver->pulse_guide(0, 500);
        } catch (const alpacacore::AlpacaException& ex) {
            refused = ex.error_code() == alpacacore::AlpacaError::InvalidOperation;
        }
        CHECK(refused);
        REQUIRE(finish());
        CHECK_FALSE(threw.load());
        CHECK(st->t_count.load() > t_before);  // the completion tail restored tracking
        CHECK(st->guide_command_count(1) == 0);
        CHECK_FALSE(driver->get_is_pulse_guiding());
    } else {
        REQUIRE(finish());
        CHECK_FALSE(threw.load());
        CHECK(st->t_count.load() > t_before);
        CHECK_NOTHROW(driver->pulse_guide(0, 500));
        CHECK(driver->get_is_pulse_guiding());
    }
    driver->set_connected(false);
}

#endif  // !_WIN32

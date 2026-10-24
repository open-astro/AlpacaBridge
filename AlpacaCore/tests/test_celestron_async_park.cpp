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
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeCelestronState {
    // The GOTO timer reads the driver's task clock (a FakeTaskClock in these
    // cases), so a park runs in virtual time like the driver's own waits.
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    alpacacore::util::TaskClock::clock::time_point now() const { return clock->now(); }
    std::atomic<bool> goto_seen{false};
    std::atomic<int> goto_count{0};
    std::atomic<int> level_start_count{0};
    std::atomic<Clock::rep> goto_started{0};
    // #742: `silent` makes the handset answer nothing (every command times
    // out, the stops included); `silent_from_goto` flips it on at the next
    // GOTO, which then goes unanswered too, so a park dispatch fails.
    std::atomic<bool> silent{false};
    std::atomic<bool> silent_from_goto{false};
    // #832: `tracking_on` makes the 't' query answer EQ-North so the completion
    // tail has a tracking restore to perform; `t_count` counts every 'T' write.
    std::atomic<bool> tracking_on{false};
    std::atomic<bool> reject_level_start{false};
    std::atomic<bool> invalid_next_home_status{false};
    std::atomic<bool> invalid_home_status{false};
    std::atomic<int> home_status_polls{0};
    std::atomic<int> cancel_goto_count{0};
    std::atomic<int> t_count{0};
    std::atomic<int> transactions{0};  // every request the handset received
    std::atomic<int> position_reads{0};  // every position query (e/E/z/Z)
    // Initiator supersede cases: >0 holds the next GOTO exchange this long (real time) before it is answered.
    std::atomic<int> gate_ms{0};
    std::atomic<Clock::rep> gate_entered{0};
    std::atomic<Clock::rep> gate_left{0};
    // Failure-stop case: >0 holds the next cancel-GOTO exchange (a stop) this long; reject_goto swallows the GOTO, so
    // the wrapper times out and the slew body fails.
    std::atomic<int> stop_gate_ms{0};
    std::atomic<Clock::rep> stop_gate_entered{0};
    std::atomic<Clock::rep> stop_gate_left{0};
    std::atomic<bool> reject_goto{false};
    void hold_stop() {
        const int hold = stop_gate_ms.exchange(0);
        if (hold <= 0) return;
        stop_gate_entered.store(Clock::now().time_since_epoch().count());
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        stop_gate_left.store(Clock::now().time_since_epoch().count());
    }
    void hold_goto() {
        const int hold = gate_ms.exchange(0);
        if (hold <= 0) return;
        gate_entered.store(Clock::now().time_since_epoch().count());
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        gate_left.store(Clock::now().time_since_epoch().count());
    }
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (!goto_seen.load()) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder celestron_responder(std::shared_ptr<FakeCelestronState> st) {
    return [st](const std::string& chunk) -> std::string {
        st->transactions.fetch_add(1);
        if (st->silent.load()) return "";
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                st->position_reads.fetch_add(1);
                return "12AB0500,20000500#";  // parseable 16/24-bit position pair
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                if (st->silent_from_goto.load()) {
                    st->silent.store(true);
                    return "";
                }
                if (st->reject_goto.load()) return "";
                st->hold_goto();
                st->goto_started.store(st->now().time_since_epoch().count());
                st->goto_seen.store(true);
                st->goto_count.fetch_add(1);
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                st->hold_stop();
                st->cancel_goto_count.fetch_add(1);
                st->goto_seen.store(false);
                return "#";
            case 'J':  // alignment complete (the slew-safety gate requires it)
                return "1#";
            case 'w':  // site read: eight binary bytes (a short answer would wait out the response timeout)
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            case 'T':  // tracking mode write
                st->t_count.fetch_add(1);
                return "#";
            case 't':  // tracking mode query
                return st->tracking_on.load() ? std::string("\x02#") : std::string("\x00#", 2);
            case 'P': {  // AUX passthrough: P len dev op ...
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                if (op == 0x0B) {
                    st->level_start_count.fetch_add(1);
                    if (st->reject_level_start.load()) return "";
                    return "#";
                }
                if (op == 0xFE) {  // complete the binary firmware reply for each AUX bus probe
                    return std::string("\x00\x00#", 3);
                }
                if (op == 0x12) {
                    st->home_status_polls.fetch_add(1);
                    if (st->invalid_next_home_status.exchange(false)) return std::string("\x01#", 2);
                    if (st->invalid_home_status.load()) return std::string("\x01#", 2);
                    return std::string("\xFF#", 2);
                }
                if (op == 0x02 || op == 0x17) {  // MC_GOTO_FAST / MC_GOTO_SLOW
                    if (st->silent_from_goto.load()) {
                        st->silent.store(true);
                        return "";
                    }
                    if (st->reject_goto.load()) return "";
                    st->hold_goto();
                    st->goto_started.store(st->now().time_since_epoch().count());
                    st->goto_seen.store(true);
                    st->goto_count.fetch_add(1);
                    return "#";
                }
                if (op == 0x13) {  // MC_SLEW_DONE: 0x00 = still slewing, 0xFF = done
                    return st->goto_in_progress() ? std::string("\x00#", 2) : std::string("\xFF#");
                }
                return std::string("\xFF#");
            }
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::celestron::ConnectionInfo endpoint(int port, int response_timeout_ms = 200) {
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = response_timeout_ms;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
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
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred,
                   std::chrono::milliseconds step = std::chrono::milliseconds(250), int max_steps = 200) {
    for (int i = 0; i < max_steps; ++i) {
        if (pred()) return true;
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(step);
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return pred();
}

}  // namespace

TEST_CASE("Celestron async - Park returns immediately, AtPark flips when the slew ends",
          "[celestron][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    // The task is parked on its first 250 ms poll: a second GOTO would be on the wire by now.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    CHECK(st->goto_count.load() == 1);

    REQUIRE(advance_until(clock, [&] { return driver->get_at_park(); }));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
    // The cancelled park task must never flip AtPark afterwards.
    // Virtual time well past the GOTO and the start grace.
    CHECK_FALSE(advance_until(clock, [&] { return driver->get_at_park(); }, std::chrono::milliseconds(250), 12));
    REQUIRE_FALSE(driver->get_at_park());

    // A park in flight gates motion members like a completed park does, so a
    // slew or axis jog cannot silently clobber it (ParkedException, 0x408).
    driver->park();
    REQUIRE(driver->get_slewing());
    CHECK_THROWS_AS(driver->move_axis(0, 0.5), alpacacore::AlpacaException);
    // Tracking is off on this fake: the parked refusal comes first (open-astro#1274).
    try {
        driver->slew_to_coordinates_async(5.0, 20.0);
        FAIL("a slew during a park in flight must be refused");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    CHECK_THROWS_AS(driver->sync_to_coordinates(5.0, 20.0), alpacacore::AlpacaException);
    try {
        driver->move_axis(0, 0.5);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    REQUIRE(driver->get_slewing());  // the park is still in flight
    driver->abort_slew();            // ...but AbortSlew may cancel it
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));

    // Disconnect with a park in flight joins the task cleanly.
    driver->park();
    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
}

TEST_CASE("Celestron FindHome - refuses during Park without replacing the park task",
          "[celestron][telescope][async][home]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    CHECK(advance_until(clock, [&] { return driver->get_at_park(); }));
    CHECK(st->goto_count.load() == 1);
    driver->unpark();
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - dispatch failure persists through Slewing and AtHome",
          "[celestron][telescope][async][homefailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    st->reject_level_start.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->silent.store(true);
    std::string message;
    REQUIRE(advance_until(
        clock, [&] { return read_slewing_error(*driver, &message); }, std::chrono::milliseconds(250), 48));
    CHECK(message.find("Park slew status polling failed") != std::string::npos);
    CHECK(read_slewing_error(*driver));
    CHECK_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Celestron FindHome - invalid home-status replies persist through Slewing and AtHome",
          "[celestron][telescope][async][home]") {
    auto st = std::make_shared<FakeCelestronState>();
    st->invalid_home_status.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->invalid_next_home_status.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->invalid_next_home_status.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        auto st = std::make_shared<FakeCelestronState>();
        st->clock = &clock;
        st->tracking_on.store(true);
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        REQUIRE(advance_until(clock, [&] { return done.load(); }));
        slewer.join();
        CHECK(code.load() == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        CHECK(t_at_exit.load() == t_after_supersede);  // no tracking restore after the supersede
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron sync slew - uncontended slew returns and runs the tracking restore",
          "[celestron][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    st->tracking_on.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    const bool finished = advance_until(clock, [&] { return done.load(); });
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
    std::shared_ptr<FakeCelestronState> st = std::make_shared<FakeCelestronState>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    std::atomic<int> bodies_done{0};  // slew-slot bodies that have returned (counted by the spawn seam)
    explicit SupersedeRig(int response_timeout_ms = 6000) {
        st->clock = &clock;
        st->tracking_on.store(true);  // a slew needs tracking on
        server = std::make_unique<alpacacore::test::FakeMountServer>(celestron_responder(st));
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

void hold_slew_body(SupersedeRig& rig, const std::shared_ptr<HeldBody>& held) {
    cel::set_slew_body_hook_for_testing(*rig.driver, [held](const cel::SlewBodyStopReason& stop_reason) {
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

// ---- The Park body's dispatch writes state around two reads (decision 0006, rule 6) ----
//
// do_slew_to_coordinates() reads J and the pier side with mutex_ free, then publishes Slewing, clears the stored
// errors and sends the GOTO. The dispatch hook sits between the reads and the publish. An initiator that takes the
// Park over while the body is there owns the state: after the release the body sends no GOTO and leaves Slewing,
// AtPark and the stored error as the claimer left them.
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
    const auto code_of = [](const std::function<void()>& call) -> int {
        try {
            call();
        } catch (const alpacacore::AlpacaException& ex) {
            return ex.error_code();
        } catch (...) {
            return -1;
        }
        return 0;
    };
    REQUIRE(code_of([&] { rig.driver->abort_slew(); }) == alpacacore::AlpacaError::DriverException);
    // A park that is still recorded as in flight refuses a slew with InvalidWhileParked; on the dropped link it fails
    // on the wire instead.
    CHECK(code_of([&] { rig.driver->slew_to_coordinates_async(5.5, 20.0); }) !=
          alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(code_of([&] { rig.driver->move_axis(0, 0.5); }) != alpacacore::AlpacaError::InvalidWhileParked);
}

#endif  // !_WIN32

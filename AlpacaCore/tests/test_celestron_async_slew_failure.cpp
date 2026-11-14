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

// open-astro#575, Celestron: the async slew task dispatches the GOTO (HC "r"
// or the AUX MC_GOTO_FAST passthrough) on its own thread; a hand controller
// that never acknowledges it made the task log "Async slew dispatch failed"
// and drop Slewing to false. The next Slewing read must return an Alpaca
// error instead (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_celestron_handset.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;
using alpacacore::test::require_alpaca_error;

// The fake mount reports pier W at longitude 0, and the driver takes a forced
// meridian flip (two passthrough GOTOs, never the plain one) whenever the target's
// hour angle is negative. A fixed RA made that depend on the wall clock, so the
// cases failed for about half of every UTC day. One hour west of the local
// sidereal time (HA = +1 h, the driver's own GMST formula) never flips.
double no_flip_ra_hours() {
    const double days =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() / 86400.0;
    const double d = 2440587.5 + days - 2451545.0;
    const double lst_hours = std::fmod(std::fmod(280.46061837 + 360.98564736629 * d, 360.0) + 360.0, 360.0) / 15.0;
    return std::fmod(lst_hours - 1.0 + 24.0, 24.0);
}

using alpacacore::test::CelestronTracking;
using alpacacore::test::FakeCelestronHandset;
using alpacacore::test::wait_until;

// This file's handset: a CGX-L-class GEM (model 24, HC 5.35, pier W) tracking EQ North, at 12AB0500,20000500.
std::shared_ptr<FakeCelestronHandset> make_handset(
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    return std::make_shared<FakeCelestronHandset>(clock, CelestronTracking::EqNorth, "12AB0500,20000500#");
}

alpacacore::vendor::celestron::ConnectionInfo endpoint(int port, int timeout_ms = 1000) {
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = timeout_ms;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int port, alpacacore::util::TaskClock& clock) {
    return alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(port), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
}

// Steps the fake clock one `step` at a time, once a driver task is parked on
// it, until `pred` holds or `real_limit` of real time passes. The bound is real
// time, not a step count: another thread's waiter satisfies the rendezvous
// while the task under test is still on the socket, so a step budget can run
// out in milliseconds. Each step waits for the woken task to settle, so no
// step lands between two of its waits.
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred,
                   std::chrono::milliseconds step = std::chrono::milliseconds(250),
                   std::chrono::seconds real_limit = std::chrono::seconds(60)) {
    const auto deadline = Clock::now() + real_limit;
    while (Clock::now() < deadline) {
        if (pred()) return true;
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(step);
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return pred();
}

// Runs a blocking driver call on its own thread, stepping the clock until it returns, then rethrows what it threw.
// `started` must hold, on real time, before the first step: a task parked on the clock from before the call (a pulse
// timer the call reaps) would otherwise satisfy the rendezvous while the call is still on the socket, and virtual
// time would run ahead of it.
template <typename Fn>
void run_virtual(alpacacore::test::FakeTaskClock& clock, Fn&& fn, const std::function<bool()>& started) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        done.store(true);
    });
    const bool began = wait_until([&] { return started() || done.load(); }, 5000);
    const bool finished = began && advance_until(clock, [&] { return done.load(); });
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));  // release a hung caller (it may park again) so the thread can join
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    caller.join();
    REQUIRE(began);
    REQUIRE(finished);
    if (error) std::rethrow_exception(error);
}

// Threw only for the driver-specific DriverException (0x500) the #575 contract
// requires; any other code is ThrewOther, so a changed code fails the tests.
enum class SlewingRead : std::uint8_t { True, False, Threw, ThrewOther };

SlewingRead read_slewing(alpacacore::TelescopeDriver& driver, std::string* message = nullptr) {
    try {
        return driver.get_slewing() ? SlewingRead::True : SlewingRead::False;
    } catch (const alpacacore::AlpacaException& ex) {
        if (message) *message = ex.what();
        return ex.error_code() == alpacacore::AlpacaError::DriverException ? SlewingRead::Threw
                                                                           : SlewingRead::ThrewOther;
    }
}

}  // namespace

TEST_CASE("Celestron Telescope Driver - all four equatorial slews are refused while Tracking is false",
          "[celestron][telescope][async][tracking-guard]") {
    auto st = make_handset();
    st->tracking_on.store(false);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_tracking());
    const double ra = no_flip_ra_hours();

    using alpacacore::test::require_alpaca_error;
    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_alpaca_error([&] { driver->slew_to_coordinates(ra, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(ra, 95.0); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&] { driver->slew_to_coordinates(ra, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(ra, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_right_ascension(ra);
    driver->set_target_declination(20.0);
    require_alpaca_error([&] { driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

    // Nothing reached the mount, and the driver is not slewing.
    CHECK_FALSE(wait_until([&] { return st->goto_count.load() != 0; }, 300));
    CHECK(st->cancel_goto_count.load() == 0);
    CHECK_FALSE(driver->get_slewing());

    // With Tracking on (re-read on the next connect) the same call goes through.
    driver->set_connected(false);
    st->tracking_on.store(true);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(ra, 20.0));
    CHECK(wait_until([&] { return st->goto_count.load() != 0; }, 3000));
    driver->set_connected(false);
}

TEST_CASE("Celestron async - a GOTO the hand controller never acknowledges surfaces through Slewing (#575)",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    // True: published before the task ran. Threw: the task won mutex_ first
    // and its dispatch already failed -- never a silent False.
    const SlewingRead first = read_slewing(*driver);
    REQUIRE((first == SlewingRead::True || first == SlewingRead::Threw));

    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot
    CHECK_FALSE(driver->get_at_home());                  // a GOTO failure does not compromise the home flag

    // A retried initiator against a responsive controller clears it.
    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 15000));
    CHECK(st->goto_count.load() == 1);  // the retried slew sends exactly one GOTO
    driver->set_connected(false);
}

TEST_CASE("Celestron MoveAxis - failed dispatch preserves a stored slew error",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    st->reject_goto.store(false);
    REQUIRE(server.drop_connections());
    try {
        driver->move_axis(0, 0.5);
        FAIL("MoveAxis returned although the controller did not answer");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron MoveAxis - successful dispatch clears a stored slew error",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->move_axis(0, 0.5));
    CHECK(driver->get_slewing());
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("Celestron Unpark - a no-op Unpark clears a stored slew error",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    // An Unpark with no park in flight claims nothing and joins nothing (decision 0006): the failing body finishes its
    // own failure, so Slewing reads false once its stop is answered, not necessarily when Unpark returns.
    REQUIRE_NOTHROW(driver->unpark());
    CHECK(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 5000));
    driver->set_connected(false);
}

TEST_CASE("Celestron async - HC completion-poll failure persists through Slewing",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->silent.store(true);  // The hand controller stops answering its "L" status query.

    std::string message;
    // The poll's link fault may be latched before the slew body stores its own error: wait for the stored one.
    REQUIRE(wait_until(
        [&] {
            return read_slewing(*driver, &message) == SlewingRead::Threw &&
                   message.find("Slew completion polling failed") != std::string::npos;
        },
        10000));
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron Slewing - a direct hardware status-poll failure is propagated",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The status poll asks the mount for Slewing (the getter serves its frame): pause its own ticks and drive it.
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    const int polls_before = st->slew_status_polls.load();
    st->malformed_slew_status.store(1);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_slewing());  // the last good frame is served while the fault is young
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_slewing());
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Slewing returned the cached state after three status polls failed");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("invalid response") != std::string::npos);
    }
    CHECK(st->slew_status_polls.load() - polls_before == 3);
    CHECK(st->cancel_goto_count.load() == 0);  // an idle status GET does not stop a mount
    driver->set_connected(false);
}

TEST_CASE("Celestron async - one bad HC or MC status poll recovers before the failure threshold",
          "[celestron][telescope][async][slewfailure]") {
    for (const bool motor_controller : {false, true}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        st->goto_never_ends.store(true);
        if (motor_controller) {
            st->firmware_major.store(0);
            st->firmware_minor.store(0);
            st->invalid_next_mc_slew_status.store(true);
        } else {
            st->malformed_next_slew_status.store(true);
        }
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        // The status poll's own L reads would add to the count this case pins: pause its ticks.
        alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
        REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
        const auto poll_count = [&] {
            return motor_controller ? st->slew_done_count.load() : st->slew_status_polls.load();
        };
        REQUIRE(advance_until(clock, [&] { return poll_count() >= 2; }, std::chrono::milliseconds(250)));
        CHECK(st->cancel_goto_count.load() == 0);
        CHECK(driver->get_slewing());

        const int after_recovery = poll_count();
        if (motor_controller) {
            st->invalid_mc_slew_status.store(true);
        } else {
            st->malformed_slew_status.store(1);
        }
        REQUIRE(advance_until(clock, [&] { return st->cancel_goto_count.load() > 0; }, std::chrono::milliseconds(250)));
        CHECK(poll_count() - after_recovery == 3);
        std::string message;
        CHECK(read_slewing(*driver, &message) == SlewingRead::Threw);
        CHECK(message.find("3 consecutive failures") != std::string::npos);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron sync slew - three status-poll failures abort the wait",
          "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->goto_never_ends.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    // The status poll's own L reads would add to the count this case pins: pause its ticks. It still parks on the
    // clock, so the slew's completion loop is the second waiter.
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    std::exception_ptr slew_error;
    std::atomic<bool> finished{false};
    std::thread slew([&] {
        try {
            driver->slew_to_coordinates(no_flip_ra_hours(), 20.0);
        } catch (...) {
            slew_error = std::current_exception();
        }
        finished.store(true);
    });
    const bool goto_dispatched = wait_until([&] { return st->goto_count.load() > 0; }, 5000);
    const bool waiting_for_poll = clock.wait_for_waiters(2, std::chrono::seconds(2));
    const int polls_before = st->slew_status_polls.load();
    st->malformed_slew_status.store(1);
    clock.advance(std::chrono::seconds(9));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    const bool finished_after_bad_poll =
        advance_until(clock, [&] { return finished.load(); }, std::chrono::milliseconds(250));
    slew.join();
    REQUIRE(goto_dispatched);
    REQUIRE(waiting_for_poll);
    REQUIRE(finished_after_bad_poll);
    REQUIRE(slew_error);
    try {
        std::rethrow_exception(slew_error);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("invalid response") != std::string::npos);
    }
    CHECK(st->slew_status_polls.load() - polls_before == 3);
    driver->set_connected(false);
}

TEST_CASE("Celestron sync slew - tracking restoration failure is propagated",
          "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));

    std::exception_ptr slew_error;
    std::atomic<bool> finished{false};
    std::thread slew([&] {
        try {
            driver->slew_to_coordinates(no_flip_ra_hours(), 20.0);
        } catch (...) {
            slew_error = std::current_exception();
        }
        finished.store(true);
    });
    const bool goto_dispatched = wait_until([&] { return st->goto_count.load() > 0; }, 5000);
    const bool waiting_for_completion = clock.wait_for_waiters(2, std::chrono::seconds(2));  // + the status poll
    st->reject_next_tracking_write.store(true);
    clock.advance(std::chrono::seconds(9));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    const bool finished_after_restore = wait_until([&] { return finished.load(); }, 5000);
    slew.join();
    REQUIRE(goto_dispatched);
    REQUIRE(waiting_for_completion);
    REQUIRE(finished_after_restore);
    REQUIRE(slew_error);
    CHECK_FALSE(st->reject_next_tracking_write.load());
    try {
        std::rethrow_exception(slew_error);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Celestron") != std::string::npos);
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron async - motor-controller completion-poll failure persists through Slewing",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    st->firmware_major.store(0);
    st->firmware_minor.store(0);  // no usable HC: use MC_GOTO_FAST and MC_SLEW_DONE
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    // One GOTO per axis: muting after the first would fail the dispatch, not the completion poll.
    REQUIRE(wait_until([&] { return st->goto_count.load() >= 2; }, 5000));
    st->silent.store(true);  // The motor controllers stop answering MC_SLEW_DONE.

    std::string message;
    // The poll's link fault may be latched before the slew body stores its own error: wait for the stored one.
    REQUIRE(wait_until(
        [&] {
            return read_slewing(*driver, &message) == SlewingRead::Threw &&
                   message.find("Slew completion polling failed") != std::string::npos;
        },
        10000));
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - invalid motor-controller completion status is rejected",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    st->firmware_major.store(0);
    st->firmware_minor.store(0);  // no usable HC: use MC_GOTO_FAST and MC_SLEW_DONE
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->invalid_mc_slew_status.store(true);
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK(message.find("Motor-controller slew completion query returned an invalid response") != std::string::npos);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - complete HC status replies are validated", "[celestron][telescope][async][slewfailure]") {
    for (const int malformed : {1, 2}) {
        auto st = make_handset();
        st->goto_never_ends.store(true);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
        REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
        st->malformed_slew_status.store(malformed);
        std::string message;
        REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
        CHECK(st->slew_status_polls.load() > 0);
        CHECK(message.find("invalid response") != std::string::npos);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron async - slew timeout is a persistent failure", "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->goto_never_ends.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    std::string message;
    const auto started = clock.now();
    REQUIRE(advance_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, std::chrono::seconds(1)));
    CHECK(clock.now() - started <= std::chrono::seconds(125));  // the 120 s timeout itself, not only its message
    CHECK(message.find("timed out after 120s") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - tracking restoration failure is retained", "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->reject_next_tracking_write.store(true);
    std::string message;
    REQUIRE(advance_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, std::chrono::milliseconds(250)));
    CHECK(message.find("Slew completion handling failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - post-slew position-read failure is retained",
          "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->reject_position_after_slew.store(true);
    std::string message;
    REQUIRE(advance_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, std::chrono::milliseconds(250)));
    CHECK(message.find("Slew completion handling failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - AbortSlew clears a stored slew failure (#575)",
          "[celestron][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - AbortSlew fences a pending GOTO", "[celestron][telescope][async][abort]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    for (int i = 0; i < 50; ++i) {
        const int before = st->goto_count.load();
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
        REQUIRE_NOTHROW(driver->abort_slew());
        const int at_abort_return = st->goto_count.load();
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        INFO("iteration " << i << ", GOTO count before=" << before << " at abort return=" << at_abort_return
                          << " after=" << st->goto_count.load());
        CHECK(st->goto_count.load() == at_abort_return);
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron AbortSlew - a stop failure still reaps the cancelled slew task",
          "[celestron][telescope][async][abort]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(server.drop_connections());
    require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::DriverException);

    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE_NOTHROW(driver->set_connected(true));
    const int before = st->goto_count.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    CHECK(wait_until([&] { return st->goto_count.load() > before; }, 5000));
    driver->set_connected(false);
}

namespace {

void refuse_slot_threads(alpacacore::TelescopeDriver& driver) {
    alpacacore::vendor::celestron::set_slew_spawn_for_testing(driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
}

// Starts a goto and lets it dispatch, then issues `second` while the slot's
// thread factory refuses (EAGAIN). The initiator has by then cancelled the
// goto body in flight, so nobody else will stop the mount: the refusal path
// has to cancel the GOTO itself and publish the AbortSlew state.
void refused_slot_start_stops_mount(const std::function<void(alpacacore::TelescopeDriver&)>& second) {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0);
    REQUIRE(wait_until([&] { return st->goto_count.load() == 1; }, 5000));
    REQUIRE(driver->get_slewing());
    const int cancels_before = st->command_count('M');

    refuse_slot_threads(*driver);
    require_alpaca_error([&] { second(*driver); }, alpacacore::AlpacaError::DriverException);

    CHECK(st->command_count('M') == cancels_before + 1);  // the GOTO was cancelled
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK_FALSE(driver->get_at_park());
    CHECK_FALSE(driver->get_at_home());
    driver->set_connected(false);
}

// A refused start that replaced nothing in the slot must put back every field
// the initiator's locked block wrote, not only Slewing: a MoveAxis in motion
// stays reported as Slewing (no stop is sent, so the axis is still driven).
void refused_start_keeps_move_axis(const std::function<void(alpacacore::TelescopeDriver&)>& second) {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->move_axis(0, 0.5);
    REQUIRE(read_slewing(*driver) == SlewingRead::True);
    const int cancels_before = st->command_count('M');
    const int passthrough_before = st->command_count('P');

    refuse_slot_threads(*driver);
    require_alpaca_error([&] { second(*driver); }, alpacacore::AlpacaError::DriverException);

    CHECK(st->command_count('M') == cancels_before);  // nothing in the slot: nothing to cancel
    CHECK(st->command_count('P') == passthrough_before);
    CHECK(read_slewing(*driver) == SlewingRead::True);  // axis 0 is still driven
    driver->set_connected(false);
}

}  // namespace

TEST_CASE("Celestron slot - a refused goto start keeps a MoveAxis in motion reported",
          "[celestron][telescope][async][slot]") {
    refused_start_keeps_move_axis([](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(6.5, 25.0); });
}

TEST_CASE("Celestron slot - a refused park start keeps a MoveAxis in motion reported",
          "[celestron][telescope][async][slot]") {
    refused_start_keeps_move_axis([](alpacacore::TelescopeDriver& d) { d.park(); });
}

TEST_CASE("Celestron slot - a refused park start keeps AtHome after FindHome", "[celestron][telescope][async][slot]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);  // no tick of its own overlaps
    driver->find_home();
    REQUIRE(
        alpacacore::vendor::celestron::poll_status_now_for_testing(*driver));  // the status poll completes the homing
    REQUIRE(read_slewing(*driver) == SlewingRead::False);
    REQUIRE(driver->get_at_home());

    refuse_slot_threads(*driver);
    require_alpaca_error([&] { driver->park(); }, alpacacore::AlpacaError::DriverException);

    CHECK(driver->get_at_home());
    CHECK_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Celestron slot - a refused goto start stops the mount when it replaced a goto",
          "[celestron][telescope][async][slot]") {
    refused_slot_start_stops_mount([](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(6.5, 25.0); });
}

TEST_CASE("Celestron slot - a refused park start stops the mount when it replaced a goto",
          "[celestron][telescope][async][slot]") {
    refused_slot_start_stops_mount([](alpacacore::TelescopeDriver& d) { d.park(); });
}

TEST_CASE("Celestron slot - a refused goto start on an idle mount cancels nothing",
          "[celestron][telescope][async][slot]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int cancels_before = st->command_count('M');

    refuse_slot_threads(*driver);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->park(); }, alpacacore::AlpacaError::DriverException);

    CHECK(st->command_count('M') == cancels_before);  // nothing was in flight: nothing to stop
    CHECK(st->goto_count.load() == 0);
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Celestron async - MoveAxis owns motion after superseding an async slew",
          "[celestron][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);

    const int before = st->goto_count.load();
    driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0);
    REQUIRE(wait_until([&] { return st->goto_count.load() > before; }, 3000));
    driver->move_axis(0, 0.5);
    const int tracking_writes_after_move = st->command_count('T');
    // Virtual time past the superseded slew task's longest wait; a stray tracking write would show.
    clock.advance(std::chrono::milliseconds(1700));
    clock.wait_for_woken_settled(std::chrono::seconds(2));

    CHECK(st->command_count('T') == tracking_writes_after_move);
    driver->set_connected(false);
}

TEST_CASE("Celestron sync - a blocking slew reaps a prior async slew task",
          "[celestron][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    const int tracking_writes_before = st->command_count('T');

    const int before = st->goto_count.load();
    driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0);
    REQUIRE(wait_until([&] { return st->goto_count.load() > before; }, 3000));
    std::atomic<bool> sync_done{false};
    std::thread syncer([&] {
        driver->slew_to_coordinates(6.0, 22.0);
        sync_done.store(true);
    });
    const bool finished = advance_until(clock, [&] { return sync_done.load(); });
    syncer.join();
    REQUIRE(finished);
    const int tracking_writes_after_sync = st->command_count('T');
    // Virtual time past any stray tail of the reaped task.
    clock.advance(std::chrono::milliseconds(200));
    clock.wait_for_woken_settled(std::chrono::seconds(2));

    CHECK(tracking_writes_after_sync == tracking_writes_before + 1);
    CHECK(st->command_count('T') == tracking_writes_after_sync);
    driver->set_connected(false);
}

TEST_CASE("Celestron PulseGuide - overlapping axes keep independent pulse chains",
          "[celestron][telescope][pulseguiding][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());

    const int ra_before = st->guide_command_count(0);
    const int dec_before = st->guide_command_count(1);
    const double ra_start = driver->get_right_ascension();
    const double ra_delta = driver->get_guide_rate().ra * 8.0 / 15.0;
    const auto parked_before = clock.waiter_count();
    driver->pulse_guide(2, 8000);  // East: RA chain continues beyond the 2.55 s hardware limit.
    driver->pulse_guide(0, 3500);  // North: must not cancel RA's remaining chunks.
    // Both chains must be parked on the clock before it moves: the pulses end on virtual time since their start, so
    // a clock that runs ahead of a chain still on the socket ends them before the checks below.
    REQUIRE(wait_until([&] { return clock.waiter_count() >= parked_before + 2; }, 5000));

    REQUIRE(advance_until(
        clock, [&] { return st->guide_command_count(0) >= ra_before + 2; }, std::chrono::milliseconds(250)));
    REQUIRE(st->guide_command_count(1) >= dec_before + 1);
    clock.advance(std::chrono::milliseconds(2500));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    CHECK(driver->get_is_pulse_guiding());  // DEC expired; the longer RA pulse is still active.
    CHECK(std::abs(driver->get_right_ascension() - (ra_start + ra_delta)) < 1e-5);
    driver->set_connected(false);
}

TEST_CASE("Celestron MoveAxis - a jog on one axis preserves the other axis pulse chain",
          "[celestron][telescope][pulseguiding][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());

    const int ra_before = st->guide_command_count(0);
    const auto parked_before = clock.waiter_count();
    driver->pulse_guide(2, 6000);  // RA pulse is still chaining after its first hardware chunk.
    driver->move_axis(1, 0.0);     // A Dec-axis stop must not cancel the RA pulse.
    REQUIRE(wait_until([&] { return clock.waiter_count() >= parked_before + 1; }, 5000));  // as above

    CHECK(advance_until(
        clock, [&] { return st->guide_command_count(0) >= ra_before + 2; }, std::chrono::milliseconds(250)));
    driver->set_connected(false);
}

TEST_CASE("Celestron AbortSlew - stops each active pulse chain before returning",
          "[celestron][telescope][pulseguiding][abort]") {
    for (const int direction : {0, 2}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset();
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
            0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_can_pulse_guide());

        const int axis = direction == 0 ? 1 : 0;
        const int before = st->guide_command_count(axis);
        driver->pulse_guide(direction, 6000);
        REQUIRE(st->guide_command_count(axis) > before);
        REQUIRE(driver->get_is_pulse_guiding());
        REQUIRE_NOTHROW(driver->abort_slew());
        const int stopped_count = st->guide_command_count(axis);
        CHECK_FALSE(driver->get_is_pulse_guiding());
        clock.advance(std::chrono::milliseconds(2700));
        CHECK(clock.wait_for_woken_settled(std::chrono::seconds(2)));
        CHECK(st->guide_command_count(axis) == stopped_count);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron PulseGuide - an expired unpolled opposite-axis pulse does not disable the hold",
          "[celestron][telescope][pulseguiding][ownership]") {
    for (const auto& [first_direction, second_direction, is_ra] : {std::tuple{2, 0, true}, std::tuple{0, 2, false}}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset();
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
            0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_can_pulse_guide());

        driver->pulse_guide(first_direction, 500);
        // Past the 500 ms pulse plus the driver's 1 s completion delay, so the
        // first pulse has expired. Do not poll IsPulseGuiding.
        clock.advance(std::chrono::milliseconds(1700));
        CHECK(clock.wait_for_woken_settled(std::chrono::seconds(2)));
        // The first read after a pulse returns its one-shot readback correction;
        // take it now so `held` below compares hold against hold.
        (void)(is_ra ? driver->get_right_ascension() : driver->get_declination());
        driver->pulse_guide(second_direction, 1500);
        const double held = is_ra ? driver->get_right_ascension() : driver->get_declination();
        st->set_position("13AB0500,21000500#");
        clock.advance(std::chrono::milliseconds(2100));  // Poll ticks may now publish the shifted position.
        CHECK(clock.wait_for_woken_settled(std::chrono::seconds(2)));

        const double during = is_ra ? driver->get_right_ascension() : driver->get_declination();
        CHECK(std::abs(during - held) < 1e-5);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron pulse state refuses a latched link fault and recovers",
          "[celestron][telescope][link][pulseguiding]") {
    const bool guiding = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));
    if (guiding) REQUIRE_NOTHROW(driver->pulse_guide(0, 25000));
    REQUIRE(driver->get_is_pulse_guiding() == guiding);
    const double altitude = driver->get_altitude();
    // The status poll reads the mount (the getters serve its frame): pause its own ticks and drive it.
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    st->silent.store(true);
    for (int i = 0; i < 3; ++i) {
        alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    }
    REQUIRE(driver->get_connected());
    const auto fault = driver->get_link_fault();
    REQUIRE_FALSE(fault.empty());
    const int requests = st->requests.load();
    try {
        (void)driver->get_is_pulse_guiding();
        FAIL("A faulted link must not answer from pulse timers");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()) == "Celestron mount communications compromised: " + fault);
    }
    // Active guides have an independent hardware-chunk worker; only idle has no background wire traffic.
    if (!guiding) CHECK(st->requests.load() == requests);
    st->silent.store(false);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(driver->get_altitude() == altitude);
    CHECK(driver->get_link_fault().empty());
    driver->abort_slew();
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("Celestron commanded state refuses a latched fault while safety stops still reach the handset",
          "[celestron][telescope][link]") {
    const bool moving = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    driver->set_utc_date(std::chrono::system_clock::now());  // UTCDate is not served before the clock is set
    const std::vector<std::pair<const char*, std::function<void()>>> reads{
        {"AtHome", [&] { (void)driver->get_at_home(); }},
        {"AtPark", [&] { (void)driver->get_at_park(); }},
        {"Slewing", [&] { (void)driver->get_slewing(); }},
        {"Tracking", [&] { (void)driver->get_tracking(); }},
        {"DeclinationRate", [&] { (void)driver->get_declination_rate(); }},
        {"RightAscensionRate", [&] { (void)driver->get_right_ascension_rate(); }},
        {"TrackingRate", [&] { (void)driver->get_tracking_rate(); }},
        {"GuideRate", [&] { (void)driver->get_guide_rate(); }},
        {"SideOfPier", [&] { (void)driver->get_side_of_pier(); }},
        {"DestinationSideOfPier", [&] { (void)driver->get_destination_side_of_pier(5.0, 20.0); }},
        {"SiteLatitude", [&] { (void)driver->get_site_latitude(); }},
        {"SiteLongitude", [&] { (void)driver->get_site_longitude(); }},
        {"SiderealTime", [&] { (void)driver->get_sidereal_time(); }},
        {"UTCDate", [&] { (void)driver->get_utc_date(); }},
    };
    for (const auto& [name, read] : reads) {
        INFO(name);
        REQUIRE_NOTHROW(read());
    }
    if (moving) driver->move_axis(1, 0.01);
    REQUIRE(driver->get_slewing() == moving);
    (void)driver->get_altitude();
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    st->silent.store(true);
    for (int i = 0; i < 3; ++i) {
        alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    }
    const auto fault = driver->get_link_fault();
    REQUIRE_FALSE(fault.empty());
    const int requests = st->requests.load();
    for (const auto& [name, read] : reads) {
        INFO(name);
        try {
            read();
            CHECK(false);
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(e.what()) == "Celestron mount communications compromised: " + fault);
        }
    }
    CHECK(st->requests.load() == requests);
    CHECK(driver->get_connected());
    CHECK(driver->get_can_pulse_guide());
    CHECK_FALSE(driver->get_name().empty());
    for (const auto& value : driver->get_device_state()) {
        CHECK(value.name == "TimeStamp");
    }
    st->silent.store(false);  // replies work again, but no status poll has cleared the latch
    const int before_stop = st->command_count('P');
    REQUIRE_NOTHROW(driver->move_axis(1, 0.0));
    CHECK(st->command_count('P') > before_stop);
    const int before_abort = st->command_count('P');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('P') > before_abort);
    CHECK(driver->get_link_fault() == fault);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(driver->get_link_fault().empty());
    for (const auto& [name, read] : reads) {
        INFO(name);
        CHECK_NOTHROW(read());
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron position - stale cache faults after repeated failed polls and recovers",
          "[celestron][telescope][link]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    // The status poll reads the mount (the getters serve its frame): pause its own ticks and drive it.
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    const double ra = driver->get_right_ascension();
    st->silent.store(true);

    // Two failed polls: the last good frame is still served and no fault is latched.
    for (int i = 0; i < 2; ++i) {
        alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
        CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
        CHECK(driver->get_link_fault().empty());
    }
    // The third latches the fault: the frame is no longer served.
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    std::string last_error;
    try {
        (void)driver->get_right_ascension();
        FAIL("stale position must not be served after a failed mount read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        last_error = e.what();
    }
    CHECK(driver->get_connected());
    CHECK(last_error.find("communications compromised") != std::string::npos);
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing

    try {
        (void)driver->get_altitude();
        FAIL("a faulted link must refuse the alt/az read too");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
    st->silent.store(false);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());

    // One failed poll after the recovery starts the count again: no fault, the frame is served.
    st->silent.store(true);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    st->silent.store(false);
    driver->set_connected(false);
}

TEST_CASE("Celestron tracking - mode follows mount geometry and site hemisphere", "[celestron][telescope][tracking]") {
    struct Expected {
        unsigned char model_id;
        unsigned char firmware_major;
        unsigned char firmware_minor;
        double latitude;
        alpacacore::AlignmentMode alignment;
        unsigned char tracking_mode;
    };
    for (const auto& expected : {Expected{24, 5, 35, 41.3, alpacacore::AlignmentMode::GermanPolar, 2},
                                 Expected{24, 5, 35, -41.3, alpacacore::AlignmentMode::GermanPolar, 3},
                                 Expected{9, 5, 35, -41.3, alpacacore::AlignmentMode::AltAz, 1},
                                 Expected{6, 3, 3, 41.3, alpacacore::AlignmentMode::GermanPolar, 1},
                                 Expected{6, 3, 3, -41.3, alpacacore::AlignmentMode::GermanPolar, 2}}) {
        auto st = make_handset();
        st->model_id.store(expected.model_id);
        st->firmware_major.store(expected.firmware_major);
        st->firmware_minor.store(expected.firmware_minor);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_site_latitude(expected.latitude);
        driver->set_site_longitude(174.8);
        CHECK(driver->get_alignment_mode() == expected.alignment);
        driver->set_tracking(true);

        const auto commands = [&] {
            std::lock_guard<std::mutex> lock(st->command_mutex);
            return st->commands;
        }();
        const auto tracking = std::find_if(commands.rbegin(), commands.rend(), [](const std::string& command) {
            return command.size() >= 2 && command[0] == 'T';
        });
        REQUIRE(tracking != commands.rend());
        CHECK(static_cast<unsigned char>((*tracking)[1]) == expected.tracking_mode);
        driver->set_connected(false);
    }

    auto st = make_handset();
    st->model_id.store(0xff);  // unknown model has no trustworthy alignment-mode mapping
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_name() == "Celestron NexStar Telescope");  // decision 0024: never "Celestron Mount"
    require_alpaca_error([&] { (void)driver->get_alignment_mode(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Celestron tracking - a configured alignment mode overrides the nominal fork geometry (#860)",
          "[celestron][telescope][tracking]") {
    using alpacacore::vendor::celestron::CelestronAlignmentSetting;
    struct Expected {
        unsigned char model_id;
        CelestronAlignmentSetting setting;
        double latitude;
        alpacacore::AlignmentMode alignment;
        unsigned char tracking_mode;
    };
    // 9 = CPC and 22 = NexStar Evolution (forks); 24 = CGX-L (German equatorial).
    for (const auto& expected :
         {Expected{9, CelestronAlignmentSetting::Equatorial, 41.3, alpacacore::AlignmentMode::Polar, 2},
          Expected{22, CelestronAlignmentSetting::Equatorial, -41.3, alpacacore::AlignmentMode::Polar, 3},
          Expected{9, CelestronAlignmentSetting::AltAz, 41.3, alpacacore::AlignmentMode::AltAz, 1},
          Expected{9, CelestronAlignmentSetting::Auto, 41.3, alpacacore::AlignmentMode::AltAz, 1},
          Expected{24, CelestronAlignmentSetting::AltAz, 41.3, alpacacore::AlignmentMode::GermanPolar, 2}}) {
        auto st = make_handset();
        st->model_id.store(expected.model_id);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
            0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt, expected.setting);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_site_latitude(expected.latitude);
        driver->set_site_longitude(174.8);
        CHECK(driver->get_alignment_mode() == expected.alignment);
        driver->set_tracking(true);

        const auto commands = [&] {
            std::lock_guard<std::mutex> lock(st->command_mutex);
            return st->commands;
        }();
        const auto tracking = std::find_if(commands.rbegin(), commands.rend(), [](const std::string& command) {
            return command.size() >= 2 && command[0] == 'T';
        });
        REQUIRE(tracking != commands.rend());
        CHECK(static_cast<unsigned char>((*tracking)[1]) == expected.tracking_mode);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron PulseGuide - an operation that reaps the pulse clears IsPulseGuiding (#831)",
          "[celestron][telescope][pulseguiding][reap]") {
    enum class Op { SlewToCoordinates, SlewToCoordinatesAsync, FindHome, Park };
    for (const Op op : {Op::SlewToCoordinates, Op::SlewToCoordinatesAsync, Op::FindHome, Op::Park}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_can_pulse_guide());

        // The longest pulse the driver takes; it outlasts the sync slew, so only the reap can clear it.
        driver->pulse_guide(0, 25500);
        REQUIRE(driver->get_is_pulse_guiding());
        switch (op) {
            case Op::SlewToCoordinates:
                run_virtual(
                    clock, [&] { driver->slew_to_coordinates(no_flip_ra_hours(), 20.0); },
                    [&] { return st->goto_count.load() >= 1; });  // the pulse is reaped, the GOTO is out
                break;
            case Op::SlewToCoordinatesAsync:
                driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0);
                break;
            case Op::FindHome:
                driver->find_home();
                break;
            case Op::Park:
                driver->park();
                break;
        }
        INFO("op " << static_cast<int>(op));
        CHECK_FALSE(driver->get_is_pulse_guiding());
        // The async slew, FindHome and park run on the task clock: step them out before the disconnect joins them.
        REQUIRE(advance_until(clock, [&] { return !driver->get_slewing(); }));
        driver->set_connected(false);
    }
}

// A reaper cancels the parked task and wakes it. A task that has checked its
// stop reason but not yet blocked must not miss that, or it sleeps until the
// next advance() and holds up the reaper's join. The pulse body waits through
// the AsyncOperation slot, which checks and blocks under the slot mutex that
// cancel() takes. FakeTaskClock's before_block hook holds the first parked
// task in exactly that window while the reaper runs (same shape as the
// Sky-Watcher case, #743).
TEST_CASE("Celestron async - a reaper's cancel is not lost between a parked task's check and its block",
          "[celestron][telescope][async][pulseguiding]") {
    std::atomic<bool> at_window{false};
    std::atomic<int> fired{0};
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // Nothing is parked on the clock yet, so the pulse task below is the first thread at the hook.
    REQUIRE(clock.waiter_count() == 0);
    clock.set_before_block([&](const std::function<bool()>& pred) {
        if (fired.fetch_add(1) == 0) {
            at_window.store(true);
            // The slot runs the predicate and the block under its one mutex,
            // so the reaper's cancel cannot reach the body until it blocks:
            // hold the task in the window for a moment of real time.
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (!pred() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::yield();
            }
        }
    });
    driver->pulse_guide(0, 4000);  // North: longer than one 2.55 s chunk, so a chained task parks on the clock
    REQUIRE(wait_until([&] { return at_window.load(); }, 3000));

    // The superseding pulse reaps the parked one: cancel, notify, join.
    std::atomic<bool> reaped{false};
    std::thread reaper([&] {
        driver->pulse_guide(0, 300);
        reaped.store(true);
    });
    CHECK(wait_until([&] { return reaped.load(); }, 2000));

    // Whatever happened, reaching the timer's deadline wakes it, so the case ends instead of hanging in a join.
    clock.advance(std::chrono::seconds(5));
    reaper.join();
    driver->set_connected(false);
    driver.reset();
    clock.set_before_block(nullptr);
}

// Decision 0022: the 8 s Slewing hold is a util::HoldWindow, armed when a GOTO starts. The async slew body polls the
// mount for its landing, not the window (rule 3), and clears the window when it lands (rule 2), so a GOTO the mount
// reports done after 1.5 s reads Slewing false well before the hold would have ended.
TEST_CASE("Celestron hold window - an async GOTO reports landed before the 8 s hold ends",
          "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto start = clock.now();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    CHECK(driver->get_slewing());
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(clock.wait_for_waiters(2, std::chrono::seconds(2)));  // the paused poll and the slew body
    REQUIRE(advance_until(clock, [&] { return clock.now() - start >= std::chrono::seconds(4); }));
    CHECK_FALSE(st->goto_in_progress());
    // The body lands (its tracking restore and landing read run on real time); virtual time stays at 4 s.
    CHECK(wait_until([&] { return !driver->get_slewing(); }, 3000));
    CHECK(clock.now() - start < std::chrono::seconds(8));
    driver->set_connected(false);
}

// Rule 2: AbortSlew clears the hold. Once the superseded slew body has ended, nothing else holds Slewing true.
TEST_CASE("Celestron hold window - AbortSlew inside the hold clears Slewing at once",
          "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(clock.wait_for_waiters(2, std::chrono::seconds(2)));  // the paused poll and the slew body
    clock.advance(std::chrono::seconds(1));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    REQUIRE(driver->get_slewing());

    REQUIRE_NOTHROW(driver->abort_slew());
    // The superseded body wakes from its 250 ms poll sleep, sees the claim and ends; only the paused poll waits then.
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    REQUIRE(wait_until([&] { return clock.waiter_count() == 1; }, 2000));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// The cross-axis position hold (pg_hold_) follows the other axis's pulse window (pulse_hold_): a Dec pulse holds RA
// at its pre-pulse value once an earlier RA pulse's window has closed, and serves RA live while that window is still
// open, because the running RA pulse moves it. A polled tick publishes the shifted mount position in between.
TEST_CASE("Celestron hold window - a Dec pulse holds RA only once the RA pulse window has closed",
          "[celestron][telescope][hold-window][pulseguiding]") {
    for (const bool ra_pulse_running : {false, true}) {
        CAPTURE(ra_pulse_running);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset();
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_can_pulse_guide());
        alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

        driver->pulse_guide(2, ra_pulse_running ? 5000 : 500);  // East, RA; the window is the pulse plus 1 s
        clock.advance(std::chrono::milliseconds(1600));
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
        REQUIRE(driver->get_is_pulse_guiding() == ra_pulse_running);
        (void)driver->get_right_ascension();  // takes the RA pulse's one-shot correction
        const double before = driver->get_right_ascension();
        driver->pulse_guide(0, 1500);  // North, Dec
        st->set_position("13AB0500,21000500#");
        REQUIRE(alpacacore::vendor::celestron::poll_status_now_for_testing(*driver));
        const double during = driver->get_right_ascension();
        if (ra_pulse_running) {
            CHECK(std::abs(during - before) > 0.05);  // served live: the mount's RA moved by about 0.094 h
        } else {
            CHECK(std::abs(during - before) < 1e-9);  // held at the pre-pulse value
        }
        driver->set_connected(false);
    }
}

#endif  // _WIN32

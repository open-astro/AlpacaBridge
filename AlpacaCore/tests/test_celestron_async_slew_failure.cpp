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

// Decision 0022 rule 2 (#1833): the closing of the three rule 2 gaps the Adoption row named.
namespace {

// An MC_AUX_GUIDE pass-through to the RA (axis 0) or Dec (axis 1) motor, as FakeCelestronHandset counts them.
bool is_guide_command(const std::string& chunk, int axis) {
    const auto device = static_cast<unsigned char>(axis == 0 ? 0x10 : 0x11);
    return chunk.size() >= 8 && chunk[0] == 'P' && static_cast<unsigned char>(chunk[2]) == device &&
           static_cast<unsigned char>(chunk[3]) == 0x26;
}

}  // namespace

// The per-axis pulse window is armed before the MC_AUX_GUIDE write: IsPulseGuiding read while the mount receives the
// command, with mutex_ free for the exchange, is already true.
TEST_CASE("Celestron hold window - the pulse window is armed before the guide command is written",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<alpacacore::TelescopeDriver*> probe{nullptr};
    std::atomic<int> guiding_at_write{-1};  // -1 not read, 0 false, 1 true, 2 threw
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (auto* driver = probe.load(); driver != nullptr && is_guide_command(chunk, 0)) {
            try {
                guiding_at_write.store(driver->get_is_pulse_guiding() ? 1 : 0);
            } catch (...) {
                guiding_at_write.store(2);
            }
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    probe.store(driver.get());
    REQUIRE_NOTHROW(driver->pulse_guide(2, 500));  // East, RA: one chunk
    probe.store(nullptr);
    CHECK(guiding_at_write.load() == 1);
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// A guide write the mount never answers throws, and the window armed before it closes again: IsPulseGuiding reads
// false at once, not for the pulse plus the 1 s completion delay.
TEST_CASE("Celestron hold window - a failed guide write leaves IsPulseGuiding false",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> swallow_guide{false};
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (swallow_guide.load() && is_guide_command(chunk, 1)) {
            return std::string();  // unanswered: the wrapper times out
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    swallow_guide.store(true);
    const auto start = clock.now();
    CHECK_THROWS_AS(driver->pulse_guide(0, 2000), alpacacore::AlpacaException);  // North, Dec
    swallow_guide.store(false);
    REQUIRE(clock.now() - start < std::chrono::seconds(3));  // virtual time has not reached the window's end
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// A blocking SlewToCoordinates whose GOTO reaches the hand controller but whose reply is lost throws. The mount may
// be slewing, so the driver stops it as the async body's fail_slew() does, after the GOTO, before the error reaches
// the client. An answered stop ends the 8 s hold armed before the send: Slewing reads false at once. A stop that
// fails too keeps the failure in Slewing, because the mount may still be moving.
TEST_CASE("Celestron hold window - a blocking SlewToCoordinates whose GOTO send fails leaves Slewing false",
          "[celestron][telescope][hold-window]") {
    for (const bool stop_fails : {false, true}) {
        CAPTURE(stop_fails);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        std::atomic<bool> lose_goto_reply{false};
        const auto inner = st->responder();
        alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
            std::string reply = inner(chunk);
            if (lose_goto_reply.load() && !chunk.empty() && (chunk[0] == 'R' || chunk[0] == 'r')) {
                return std::string();  // the mount took the GOTO; its reply is lost and the wrapper times out
            }
            return reply;
        });
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
        REQUIRE(read_slewing(*driver) == SlewingRead::False);

        lose_goto_reply.store(true);
        st->reject_stop.store(stop_fails);
        const auto start = clock.now();
        CHECK_THROWS_AS(driver->slew_to_coordinates(no_flip_ra_hours(), 20.0), alpacacore::AlpacaException);
        lose_goto_reply.store(false);
        st->reject_stop.store(false);
        REQUIRE(st->goto_count.load() == 1);                     // the mount accepted the GOTO
        REQUIRE(clock.now() - start < std::chrono::seconds(8));  // virtual time has not reached the hold's end
        if (stop_fails) {
            CHECK(read_slewing(*driver) == SlewingRead::Threw);
        } else {
            CHECK(st->cancel_goto_count.load() == 1);
            CHECK(st->cancel_first_seq.load() > st->goto_last_seq.load());  // the stop went out after the GOTO
            CHECK_FALSE(st->goto_in_progress());
            CHECK(read_slewing(*driver) == SlewingRead::False);
        }
        driver->set_connected(false);
    }
}

// The same for the flip decision's pier read, which runs after the publish and before any GOTO: the hand controller
// never answers it, the call throws, and Slewing reads false at once.
TEST_CASE("Celestron hold window - a blocking SlewToCoordinates whose pier read fails leaves Slewing false",
          "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> swallow_pier{false};
    std::atomic<int> pier_reads{0};
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (swallow_pier.load() && chunk == "p") {
            ++pier_reads;
            return std::string();  // unanswered: the wrapper times out
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE(read_slewing(*driver) == SlewingRead::False);

    swallow_pier.store(true);
    const auto start = clock.now();
    CHECK_THROWS_AS(driver->slew_to_coordinates(no_flip_ra_hours(), 20.0), alpacacore::AlpacaException);
    swallow_pier.store(false);
    REQUIRE(pier_reads.load() == 1);
    REQUIRE(st->goto_requests.load() == 0);
    REQUIRE(clock.now() - start < std::chrono::seconds(8));  // virtual time has not reached the hold's end
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

// The rollback after a failed pier read puts back the Slewing value the client was served, which the status poll
// took from the mount, not the driver's own last write: a GOTO started from the hand pad reads true before the call
// and still reads true after it throws.
TEST_CASE("Celestron hold window - a failed pier read keeps the polled Slewing of a hand-pad GOTO",
          "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> swallow_pier{false};
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (swallow_pier.load() && chunk == "p") {
            return std::string();  // unanswered: the wrapper times out
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE(read_slewing(*driver) == SlewingRead::False);

    st->goto_never_ends.store(true);
    st->goto_seen.store(true);  // a GOTO from the hand pad: the driver sent nothing
    REQUIRE(alpacacore::vendor::celestron::poll_status_now_for_testing(*driver));
    REQUIRE(read_slewing(*driver) == SlewingRead::True);

    swallow_pier.store(true);
    CHECK_THROWS_AS(driver->slew_to_coordinates(no_flip_ra_hours(), 20.0), alpacacore::AlpacaException);
    swallow_pier.store(false);
    REQUIRE(st->goto_requests.load() == 0);
    CHECK(read_slewing(*driver) == SlewingRead::True);
    driver->set_connected(false);
}

// A blocking SlewToCoordinates that takes over a MoveAxis or a FindHome keeps its flag until its GOTO is out. When
// the GOTO's reply is lost and the stops after it are answered, the mount is stopped: the MoveAxis or FindHome is
// over with it, so Slewing reads false at once, not until a later MoveAxis(axis, 0) or AbortSlew.
TEST_CASE("Celestron hold window - a failed blocking GOTO send ends a superseded MoveAxis or FindHome",
          "[celestron][telescope][hold-window]") {
    for (const bool homing : {false, true}) {
        CAPTURE(homing);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        std::atomic<bool> lose_goto_reply{false};
        const auto inner = st->responder();
        alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
            std::string reply = inner(chunk);
            if (lose_goto_reply.load() && !chunk.empty() && (chunk[0] == 'R' || chunk[0] == 'r')) {
                return std::string();  // the mount took the GOTO; its reply is lost and the wrapper times out
            }
            return reply;
        });
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

        if (homing) {
            REQUIRE_NOTHROW(driver->find_home());
        } else {
            REQUIRE_NOTHROW(driver->move_axis(0, 2.0));
        }
        REQUIRE(read_slewing(*driver) == SlewingRead::True);

        lose_goto_reply.store(true);
        CHECK_THROWS_AS(driver->slew_to_coordinates(no_flip_ra_hours(), 20.0), alpacacore::AlpacaException);
        lose_goto_reply.store(false);
        REQUIRE(st->goto_requests.load() == 1);      // the GOTO went out
        REQUIRE(st->cancel_goto_count.load() == 1);  // and the stop after it was answered
        CHECK(read_slewing(*driver) == SlewingRead::False);
        driver->set_connected(false);
    }
}

// A failed AbortSlew stop is stored and Slewing raises it, because the mount may still be moving. A blocking
// SlewToCoordinates whose flip pier read is never answered fails before any GOTO byte goes out, so nothing it did
// reached the mount: the stored stop failure is still what Slewing raises, not the false the rollback serves.
TEST_CASE("Celestron hold window - a failed pier read keeps a stored AbortSlew stop failure",
          "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> swallow_pier{false};
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (swallow_pier.load() && chunk == "p") {
            return std::string();  // unanswered: the wrapper times out
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    st->reject_stop.store(true);
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);
    st->reject_stop.store(false);
    std::string message;
    REQUIRE(read_slewing(*driver, &message) == SlewingRead::Threw);
    REQUIRE(message.find("AbortSlew stop failed") != std::string::npos);

    swallow_pier.store(true);
    CHECK_THROWS_AS(driver->slew_to_coordinates(no_flip_ra_hours(), 20.0), alpacacore::AlpacaException);
    swallow_pier.store(false);
    REQUIRE(st->goto_requests.load() == 0);
    message.clear();
    CHECK(read_slewing(*driver, &message) == SlewingRead::Threw);
    CHECK(message.find("AbortSlew stop failed") != std::string::npos);
    driver->set_connected(false);
}

// FindHome supersedes a GOTO inside its 8 s hold and ends the hold: once the status poll sees the home done, Slewing
// reads false at once, not when the GOTO's hold would have closed.
TEST_CASE("Celestron hold window - FindHome ends a superseded GOTO's hold", "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto start = clock.now();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(clock.wait_for_waiters(2, std::chrono::seconds(2)));  // the paused poll and the slew body
    clock.advance(std::chrono::seconds(1));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    REQUIRE(driver->get_slewing());

    REQUIRE_NOTHROW(driver->find_home());
    // The superseded body wakes from the claim's cancel and ends; only the paused poll waits then.
    REQUIRE(wait_until([&] { return clock.waiter_count() == 1; }, 2000));
    REQUIRE(driver->get_slewing());  // homing
    // The fake's GOTO does not end at MC_LEVEL_START; the mount that took the home reports no GOTO on L, so the
    // poll's L read below cannot hold Slewing true for the device.
    st->no_motion.store(true);
    REQUIRE(alpacacore::vendor::celestron::poll_status_now_for_testing(*driver));  // MC_LEVEL_DONE answers done
    REQUIRE(driver->get_at_home());
    REQUIRE(clock.now() - start < std::chrono::seconds(8));  // inside the GOTO's hold
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// FindHome takes over a GOTO inside its 8 s hold. While its MC_LEVEL_START is on the wire (mutex_ free, homing_ not yet
// set) and the superseded body has ended, the GOTO's hold still owns Slewing: a status poll tick in that gap leaves L
// unread, as it does through the rest of the hold, so it cannot publish an idle frame for a GOTO L does not show yet.
// The gap is held open by the fake's MC_LEVEL_START handler, which waits on two signals: the superseded body has
// returned (the slew slot's thread factory), and the gap tick has planned its sample (the after-plan seam).
TEST_CASE("Celestron hold window - FindHome keeps a superseded GOTO's hold while its home command is in flight",
          "[celestron][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<alpacacore::TelescopeDriver*> probe{nullptr};
    std::atomic<bool> polling{false};
    std::atomic<int> l_reads_in_gap_tick{0};
    std::atomic<int> position_reads_in_gap_tick{0};
    std::atomic<bool> body_returned_before_tick{false};
    std::atomic<bool> tick_planned_in_gap{false};
    auto bodies_returned = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::Latch planned;
    std::thread poller;
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (polling.load() && !chunk.empty()) {
            if (chunk[0] == 'L') {
                ++l_reads_in_gap_tick;
            }
            const bool mc_position =
                chunk.size() >= 8 && chunk[0] == 'P' && static_cast<unsigned char>(chunk[3]) == 0x01;
            if (chunk == "e" || chunk == "E" || mc_position) {
                ++position_reads_in_gap_tick;
            }
        }
        const bool level_start = chunk.size() >= 8 && chunk[0] == 'P' && static_cast<unsigned char>(chunk[2]) == 0x10 &&
                                 static_cast<unsigned char>(chunk[3]) == 0x0B;
        if (auto* driver = probe.exchange(nullptr); driver != nullptr && level_start) {
            // The superseded body wakes from the claim's cancel and returns, so the slot no longer reports it running.
            body_returned_before_tick.store(wait_until([&] { return bodies_returned->load() >= 1; }, 5000));
            polling.store(true);
            poller = std::thread([&, driver] {
                (void)alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
                polling.store(false);
            });
            // The tick has read its plan with mutex_ free; its exchanges wait for this one to be answered.
            tick_planned_in_gap.store(planned.wait_entered());
            planned.release();
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    alpacacore::vendor::celestron::set_slew_spawn_for_testing(*driver, [bodies_returned](std::function<void()> body) {
        return std::thread([bodies_returned, body = std::move(body)] {
            body();
            bodies_returned->fetch_add(1);
        });
    });
    alpacacore::vendor::celestron::set_status_poll_after_plan_hook_for_testing(*driver, [&] {
        if (polling.load()) {
            planned.block();
        }
    });

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(clock.wait_for_waiters(2, std::chrono::seconds(2)));  // the paused poll and the slew body
    REQUIRE(driver->get_slewing());

    probe.store(driver.get());
    REQUIRE_NOTHROW(driver->find_home());
    REQUIRE(poller.joinable());
    poller.join();
    alpacacore::vendor::celestron::set_status_poll_after_plan_hook_for_testing(*driver, nullptr);
    REQUIRE(body_returned_before_tick.load());
    REQUIRE(tick_planned_in_gap.load());
    REQUIRE(position_reads_in_gap_tick.load() > 0);  // the gap tick sampled the mount
    CHECK(l_reads_in_gap_tick.load() == 0);
    CHECK(driver->get_slewing());  // homing
    driver->set_connected(false);
}

// The pulse position holds (pg_hold_) and one-shot corrections (pg_correction_) close with the pulse that armed them
// (decision 0022 rule 2). The fake's MC_AUX_GUIDE moves nothing, so a correction reads about 4" from the pre-pulse
// value and a hold reads it exactly, while the mount these cases move reads about 0.094 h and 1.4 deg away.
namespace {

struct PrePulsePosition {
    double ra_hours;
    double dec_degrees;
};

PrePulsePosition read_position(alpacacore::TelescopeDriver& driver) {
    return {driver.get_right_ascension(), driver.get_declination()};
}

// Moves the fake's mount and publishes the move with one status tick.
bool move_mount(FakeCelestronHandset& st, alpacacore::TelescopeDriver& driver) {
    st.set_position("13AB0500,21000500#");
    return alpacacore::vendor::celestron::poll_status_now_for_testing(driver);
}

// Both axes serve the moved mount: no correction and no hold left from the pulse.
void check_served_live(alpacacore::TelescopeDriver& driver, const PrePulsePosition& before) {
    CHECK(std::abs(driver.get_declination() - before.dec_degrees) > 1.0);
    CHECK(std::abs(driver.get_right_ascension() - before.ra_hours) > 0.05);
}

}  // namespace

// The reviewer's case: a North pulse whose guide write the mount never answers armed the Dec correction and the RA
// hold before the write. Neither may outlive the failure.
TEST_CASE("Celestron hold window - a failed guide write leaves no position hold or correction",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> swallow_guide{false};
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (swallow_guide.load() && is_guide_command(chunk, 1)) {
            return std::string();  // unanswered: the wrapper times out
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto before = read_position(*driver);
    swallow_guide.store(true);
    CHECK_THROWS_AS(driver->pulse_guide(0, 2000), alpacacore::AlpacaException);  // North, Dec
    swallow_guide.store(false);
    REQUIRE(move_mount(*st, *driver));
    check_served_live(*driver, before);
    driver->set_connected(false);
}

// A failed Dec guide write while an RA pulse runs gives the Dec hold back to the RA pulse that armed it: Dec stays at
// its pre-pulse value for the rest of that pulse, as it would have without the failed call.
TEST_CASE("Celestron hold window - a failed guide write keeps the hold a running pulse armed",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> swallow_guide{false};
    const auto inner = st->responder();
    alpacacore::test::FakeMountServer server([&, inner](const std::string& chunk) {
        if (swallow_guide.load() && is_guide_command(chunk, 1)) {
            return std::string();
        }
        return inner(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto before = read_position(*driver);
    REQUIRE_NOTHROW(driver->pulse_guide(2, 5000));  // East, RA: holds Dec for 6.2 s of virtual time
    swallow_guide.store(true);
    CHECK_THROWS_AS(driver->pulse_guide(0, 2000), alpacacore::AlpacaException);  // North, Dec
    swallow_guide.store(false);
    REQUIRE(driver->get_is_pulse_guiding());  // the RA pulse
    REQUIRE(move_mount(*st, *driver));
    CHECK(std::abs(driver->get_declination() - before.dec_degrees) < 1e-9);
    driver->set_connected(false);
}

// AbortSlew ends a pulse, and with it the pulse's correction and its hold on the other axis.
TEST_CASE("Celestron hold window - AbortSlew ends a pulse's position hold and correction",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto before = read_position(*driver);
    REQUIRE_NOTHROW(driver->pulse_guide(0, 2000));  // North, Dec
    REQUIRE_NOTHROW(driver->abort_slew());
    REQUIRE(move_mount(*st, *driver));
    check_served_live(*driver, before);
    driver->set_connected(false);
}

// A reconnect inside a pulse's windows starts with none of them: the new session serves the mount.
TEST_CASE("Celestron hold window - a reconnect ends a pulse's position hold and correction",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto before = read_position(*driver);
    REQUIRE_NOTHROW(driver->pulse_guide(0, 2000));  // North, Dec
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE(move_mount(*st, *driver));
    check_served_live(*driver, before);
    driver->set_connected(false);
}

// MoveAxis on the pulsed axis ends the pulse: its correction and its hold on the other axis close with its window.
TEST_CASE("Celestron hold window - MoveAxis ends the pulse's position hold and correction",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto before = read_position(*driver);
    REQUIRE_NOTHROW(driver->pulse_guide(0, 2000));  // North, Dec (axis 1)
    REQUIRE_NOTHROW(driver->move_axis(1, 0.0));
    REQUIRE(move_mount(*st, *driver));
    check_served_live(*driver, before);
    driver->set_connected(false);
}

// MoveAxis moving the other axis ends the hold a pulse put on it: RA serves the moving mount while the Dec pulse keeps
// its own correction.
TEST_CASE("Celestron hold window - MoveAxis on the other axis ends the hold a pulse put on it",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

    const auto before = read_position(*driver);
    REQUIRE_NOTHROW(driver->pulse_guide(0, 2000));  // North, Dec (axis 1): holds RA
    REQUIRE_NOTHROW(driver->move_axis(0, 0.5));
    REQUIRE(move_mount(*st, *driver));
    CHECK(std::abs(driver->get_right_ascension() - before.ra_hours) > 0.05);
    CHECK(std::abs(driver->get_declination() - before.dec_degrees) < 0.01);  // the Dec pulse's correction
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

// A pulse whose chunk chain cannot start is stopped after its first chunk: the correction predicted the whole pulse
// and the hold on the other axis was for its length, so both close with the refusal.
TEST_CASE("Celestron hold window - a refused chunk chain leaves no position hold or correction",
          "[celestron][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    alpacacore::vendor::celestron::set_pulse_spawn_for_testing(*driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });

    const auto before = read_position(*driver);
    CHECK_THROWS(driver->pulse_guide(0, 5000));  // North, Dec: 5 s needs a chained chunk
    CHECK_FALSE(driver->get_is_pulse_guiding());
    REQUIRE(move_mount(*st, *driver));
    check_served_live(*driver, before);
    driver->set_connected(false);
}

#endif  // _WIN32

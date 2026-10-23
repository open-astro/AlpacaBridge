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
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

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

struct FakeCelestronState {
    // The GOTO timer reads this clock. A case that steps the driver on a
    // FakeTaskClock points it there, so the slew runs in virtual time too.
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    alpacacore::util::TaskClock::clock::time_point now() const { return clock->now(); }
    std::atomic<bool> reject_goto{false};  // true: swallow the GOTO -> the wrapper times out and throws
    std::atomic<bool> goto_cancelled{false};  // set by the cancel-goto command, cleared by the next GOTO
    std::atomic<bool> hold_slew{false};
    std::atomic<bool> shifted_position{false};
    std::atomic<bool> reject_next_position_reply{false};
    std::atomic<bool> reject_position_after_slew{false};
    std::atomic<bool> reject_next_tracking_write{false};
    std::atomic<bool> malformed_next_slew_status{false};
    std::atomic<int> malformed_slew_status{0};
    std::atomic<bool> invalid_next_mc_slew_status{false};
    std::atomic<bool> invalid_mc_slew_status{false};
    std::atomic<int> slew_status_polls{0};
    std::atomic<int> mc_slew_status_polls{0};
    std::atomic<bool> invalid_next_home_status{false};
    std::atomic<bool> mute{false};
    std::atomic<bool> tracking_on{true};      // what the 't' query answers: EQ-North when on, Off when not
    std::atomic<unsigned char> model_id{24};  // CGX-L
    std::atomic<unsigned char> firmware_major{5};
    std::atomic<unsigned char> firmware_minor{35};
    std::atomic<int> goto_count{0};
    std::atomic<int> cancel_goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::mutex command_mutex;
    std::vector<std::string> commands;
    void record(const std::string& command) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.push_back(command);
    }
    int command_count(char prefix) {
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [prefix](const std::string& command) {
            return !command.empty() && command[0] == prefix;
        }));
    }
    int guide_command_count(int axis) {
        const auto device = static_cast<unsigned char>(axis == 0 ? 0x10 : 0x11);
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [device](const std::string& command) {
            return command.size() >= 8 && command[0] == 'P' && static_cast<unsigned char>(command[2]) == device &&
                   static_cast<unsigned char>(command[3]) == 0x26;
        }));
    }
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (goto_count.load() == 0 || goto_cancelled.load()) return false;
        if (hold_slew.load()) return true;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return now() - started < kGotoDuration;
    }
    std::string goto_reply() {
        if (reject_goto.load()) {
            return "";  // no reply at all: the response timeout fires inside the slew task
        }
        goto_cancelled.store(false);
        goto_started.store(now().time_since_epoch().count());
        goto_count.fetch_add(1);
        return "#";
    }
};

alpacacore::test::FakeMountServer::Responder celestron_responder(const std::shared_ptr<FakeCelestronState>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (st->mute.load()) return "";
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'T' || chunk[0] == 'P' || chunk[0] == 'M') {
            st->record(chunk);
        }
        switch (chunk[0]) {
            case 'm':
                return std::string(1, static_cast<char>(st->model_id.load())) + "#";
            case 'V':
                return std::string(1, static_cast<char>(st->firmware_major.load())) +
                       static_cast<char>(st->firmware_minor.load()) + "#";
            case 'w':
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                if (st->reject_next_position_reply.exchange(false)) return "";
                return st->shifted_position.load() ? "13AB0500,21000500#" : "12AB0500,20000500#";
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                return st->goto_reply();
            case 'L':
                st->slew_status_polls.fetch_add(1);
                if (st->malformed_next_slew_status.exchange(false)) return "01#";
                if (const int malformed = st->malformed_slew_status.load(); malformed == 1) return "01#";
                if (st->malformed_slew_status.load() == 2) return "0junk#";
                if (st->goto_count.load() > 0 && !st->goto_in_progress() &&
                    st->reject_position_after_slew.exchange(false)) {
                    st->reject_next_position_reply.store(true);
                    return "0#";
                }
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                st->cancel_goto_count.fetch_add(1);
                st->goto_cancelled.store(true);
                return "#";
            case 'J':  // alignment complete (the slew-safety gate requires it)
                return "1#";
            case 'p':  // pier side used by the DEC guide command
                return "W#";
            case 't':  // tracking mode query
                return st->tracking_on.load() ? std::string("\x02#") : std::string("\x00#", 2);
            case 'T':
                if (st->reject_next_tracking_write.exchange(false)) return "";
                return "#";
            case 'P': {  // AUX passthrough: P len dev op ...
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
                if (op == 0xFE && device == 0x32) {  // DEC autoguider port firmware probe
                    return std::string("\x01\x00#", 3);
                }
                if (op == 0xFE) {  // complete the binary firmware reply for absent bus devices
                    return std::string("\x00\x00#", 3);
                }
                if (op == 0x26) {  // MC_AUX_GUIDE
                    return "#";
                }
                if (op == 0x02 || op == 0x17) {  // MC_GOTO_FAST / MC_GOTO_SLOW
                    return st->goto_reply();
                }
                if (op == 0x13) {  // MC_SLEW_DONE
                    st->mc_slew_status_polls.fetch_add(1);
                    if (st->invalid_next_mc_slew_status.exchange(false)) return std::string("\x01#", 2);
                    if (st->invalid_mc_slew_status.load()) return std::string("\x01#", 2);
                    return st->goto_in_progress() ? std::string("\x00#", 2) : std::string("\xFF#");
                }
                return std::string("\xFF#");
            }
            default:
                return "0#";
        }
    };
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

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// Steps the fake clock one `step` at a time, once a driver task is parked on
// it, until `pred` holds or `max_steps` steps pass. Each step waits for the
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
    auto st = std::make_shared<FakeCelestronState>();
    st->tracking_on.store(false);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_tracking());
    const double ra = no_flip_ra_hours();

    const auto require_code = [](const std::function<void()>& call, int code) {
        try {
            call();
            FAIL("Expected an AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == code);
        }
    };
    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_code([&] { driver->slew_to_coordinates(ra, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_code([&] { driver->slew_to_coordinates_async(ra, 95.0); }, alpacacore::AlpacaError::InvalidValue);

    require_code([&] { driver->slew_to_coordinates(ra, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_code([&] { driver->slew_to_coordinates_async(ra, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_right_ascension(ra);
    driver->set_target_declination(20.0);
    require_code([&] { driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_code([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->unpark());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - HC completion-poll failure persists through Slewing",
          "[celestron][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->mute.store(true);  // The hand controller stops answering its "L" status query.

    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK(message.find("Slew completion polling failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron Slewing - a direct hardware status-poll failure is propagated",
          "[celestron][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        auto st = std::make_shared<FakeCelestronState>();
        st->clock = &clock;
        st->hold_slew.store(true);
        if (motor_controller) {
            st->firmware_major.store(0);
            st->firmware_minor.store(0);
            st->invalid_next_mc_slew_status.store(true);
        } else {
            st->malformed_next_slew_status.store(true);
        }
        alpacacore::test::FakeMountServer server(celestron_responder(st));
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        // The status poll's own L reads would add to the count this case pins: pause its ticks.
        alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);

        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
        REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
        const auto poll_count = [&] {
            return motor_controller ? st->mc_slew_status_polls.load() : st->slew_status_polls.load();
        };
        REQUIRE(advance_until(clock, [&] { return poll_count() >= 2; }, std::chrono::milliseconds(250), 20));
        CHECK(st->cancel_goto_count.load() == 0);
        CHECK(driver->get_slewing());

        const int after_recovery = poll_count();
        if (motor_controller) {
            st->invalid_mc_slew_status.store(true);
        } else {
            st->malformed_slew_status.store(1);
        }
        REQUIRE(
            advance_until(clock, [&] { return st->cancel_goto_count.load() > 0; }, std::chrono::milliseconds(250), 20));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    st->hold_slew.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        advance_until(clock, [&] { return finished.load(); }, std::chrono::milliseconds(250), 20);
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->firmware_major.store(0);
    st->firmware_minor.store(0);  // no usable HC: use MC_GOTO_FAST and MC_SLEW_DONE
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->mute.store(true);  // The motor controllers stop answering MC_SLEW_DONE.

    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK(message.find("Slew completion polling failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - invalid motor-controller completion status is rejected",
          "[celestron][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    st->firmware_major.store(0);
    st->firmware_minor.store(0);  // no usable HC: use MC_GOTO_FAST and MC_SLEW_DONE
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        auto st = std::make_shared<FakeCelestronState>();
        st->hold_slew.store(true);
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    st->hold_slew.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    std::string message;
    REQUIRE(advance_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, std::chrono::seconds(1), 125));
    CHECK(message.find("timed out after 120s") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - tracking restoration failure is retained", "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->reject_next_tracking_write.store(true);
    std::string message;
    REQUIRE(advance_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, std::chrono::milliseconds(250),
        24));
    CHECK(message.find("Slew completion handling failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - post-slew position-read failure is retained",
          "[celestron][telescope][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    st->reject_position_after_slew.store(true);
    std::string message;
    REQUIRE(advance_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, std::chrono::milliseconds(250),
        24));
    CHECK(message.find("Slew completion handling failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - AbortSlew clears a stored slew failure (#575)",
          "[celestron][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(server.drop_connections());
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);

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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    CHECK_THROWS_AS(second(*driver), alpacacore::AlpacaException);

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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    CHECK_THROWS_AS(second(*driver), alpacacore::AlpacaException);

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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    CHECK_THROWS_AS(driver->park(), alpacacore::AlpacaException);

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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int cancels_before = st->command_count('M');

    refuse_slot_threads(*driver);
    CHECK_THROWS_AS(driver->slew_to_coordinates_async(no_flip_ra_hours(), 20.0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->park(), alpacacore::AlpacaException);

    CHECK(st->command_count('M') == cancels_before);  // nothing was in flight: nothing to stop
    CHECK(st->goto_count.load() == 0);
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Celestron async - MoveAxis owns motion after superseding an async slew",
          "[celestron][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    driver->pulse_guide(2, 8000);  // East: RA chain continues beyond the 2.55 s hardware limit.
    driver->pulse_guide(0, 3500);  // North: must not cancel RA's remaining chunks.

    REQUIRE(advance_until(
        clock, [&] { return st->guide_command_count(0) >= ra_before + 2; }, std::chrono::milliseconds(250), 18));
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
    auto st = std::make_shared<FakeCelestronState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());

    const int ra_before = st->guide_command_count(0);
    driver->pulse_guide(2, 6000);  // RA pulse is still chaining after its first hardware chunk.
    driver->move_axis(1, 0.0);     // A Dec-axis stop must not cancel the RA pulse.

    CHECK(advance_until(
        clock, [&] { return st->guide_command_count(0) >= ra_before + 2; }, std::chrono::milliseconds(250), 18));
    driver->set_connected(false);
}

TEST_CASE("Celestron AbortSlew - stops each active pulse chain before returning",
          "[celestron][telescope][pulseguiding][abort]") {
    for (const int direction : {0, 2}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = std::make_shared<FakeCelestronState>();
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        auto st = std::make_shared<FakeCelestronState>();
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        st->shifted_position.store(true);
        clock.advance(std::chrono::milliseconds(2100));  // Poll ticks may now publish the shifted position.
        CHECK(clock.wait_for_woken_settled(std::chrono::seconds(2)));

        const double during = is_ra ? driver->get_right_ascension() : driver->get_declination();
        CHECK(std::abs(during - held) < 1e-5);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron position - stale cache faults after repeated failed polls and recovers",
          "[celestron][telescope][link]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
        0, endpoint(server.port()), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        alpacacore::vendor::celestron::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    // The status poll reads the mount (the getters serve its frame): pause its own ticks and drive it.
    alpacacore::vendor::celestron::set_status_poll_paused_for_testing(*driver, true);
    const double ra = driver->get_right_ascension();
    st->mute.store(true);

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
    st->mute.store(false);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());

    // One failed poll after the recovery starts the count again: no fault, the frame is served.
    st->mute.store(true);
    alpacacore::vendor::celestron::poll_status_now_for_testing(*driver);
    CHECK(driver->get_link_fault().empty());
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    st->mute.store(false);
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
        auto st = std::make_shared<FakeCelestronState>();
        st->model_id.store(expected.model_id);
        st->firmware_major.store(expected.firmware_major);
        st->firmware_minor.store(expected.firmware_minor);
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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

    auto st = std::make_shared<FakeCelestronState>();
    st->model_id.store(0xff);  // unknown model has no trustworthy alignment-mode mapping
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK_THROWS_AS(driver->get_alignment_mode(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_tracking(true), alpacacore::AlpacaException);
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
        auto st = std::make_shared<FakeCelestronState>();
        st->model_id.store(expected.model_id);
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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
        auto st = std::make_shared<FakeCelestronState>();
        st->clock = &clock;
        alpacacore::test::FakeMountServer server(celestron_responder(st));
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
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
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

#endif  // _WIN32

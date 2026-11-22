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

// open-astro#575, SynScan: the async slew task dispatches the GOTO on its own
// thread; a handset that never acknowledges it made the task log "Async slew
// dispatch failed" and drop Slewing to false -- a silent no-op to the client.
// The next Slewing read must return an Alpaca error instead (see
// test_skywatcher_async_slew_failure.cpp for the contract reference).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

using alpacacore::test::require_alpaca_error;

struct FakeSynScanState {
    // The GOTO timer reads this clock. A case that steps the driver on a
    // FakeTaskClock points it there, so the slew runs in virtual time too.
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    alpacacore::util::TaskClock::clock::time_point now() const { return clock->now(); }
    std::atomic<bool> reject_goto{false};  // true: swallow the GOTO -> the wrapper times out and throws
    std::atomic<bool> reject_tracking{false};  // true: swallow the tracking write 'T' -> the wrapper times out
    std::atomic<bool> reject_tracking_read{false};  // true: swallow the tracking read 't' -> the wrapper times out
    std::atomic<bool> mute{false};
    std::atomic<int> requests{0};
    std::atomic<unsigned char> model_id{50};
    // The handset's tracking mode ('t' reads it, 'T' writes it). A test sets it directly to stand in for the
    // keypad changing it outside the driver.
    std::atomic<int> tracking_mode{1};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<bool> goto_stopped{false};
    // The position a precise GOTO landed on; empty until the first one (#1105). With land_on_goto false the mount
    // never moves, so the driver's landing check fails the slew after one repeat GOTO.
    std::atomic<bool> land_on_goto{true};
    // true: a GOTO reports itself in progress until a cancel ('M') ends it, so the driver's wait times out (#2412).
    std::atomic<bool> goto_hangs{false};
    std::atomic<bool> goto_cancelled{false};
    std::atomic<int> cancel_count{0};
    std::mutex landed_mutex;
    std::string landed;
    // Runs on the server thread before each reply (unset: nothing), so a case can act while a command is in flight.
    std::mutex before_reply_mutex;
    std::function<void(const std::string&)> before_reply;
    std::mutex command_mutex;
    std::vector<std::string> commands;
    void record(const std::string& command) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.push_back(command);
    }
    std::vector<std::string> command_snapshot() {
        std::lock_guard<std::mutex> lock(command_mutex);
        return commands;
    }
    int command_count(char prefix) {
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [prefix](const auto& command) {
            return !command.empty() && command[0] == prefix;
        }));
    }
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (goto_count.load() == 0) return false;
        if (goto_hangs.load()) return !goto_cancelled.load();
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return !goto_stopped.load() && now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder synscan_responder(const std::shared_ptr<FakeSynScanState>& st) {
    return [st](const std::string& chunk) -> std::string {
        st->requests.fetch_add(1);
        {
            std::function<void(const std::string&)> hook;
            {
                std::lock_guard<std::mutex> lock(st->before_reply_mutex);
                hook = st->before_reply;
            }
            if (hook) hook(chunk);
        }
        if (st->mute.load()) return "";
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'P' || chunk[0] == 'T' || chunk[0] == 'r' || chunk[0] == 'R' || chunk[0] == 'W' ||
            chunk[0] == 'H') {
            st->record(chunk);
        }
        if (chunk[0] == 'T' && st->reject_tracking.load()) return "";
        if (chunk[0] == 't' && st->reject_tracking_read.load()) return "";
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#" (the connect-time link check)
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z': {
                std::lock_guard<std::mutex> lock(st->landed_mutex);
                return st->landed.empty() ? "12AB0500,20000500#" : st->landed;
            }
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                if (st->reject_goto.load()) {
                    return "";  // no reply at all: the response timeout fires inside the slew task
                }
                if (chunk[0] == 'r' && chunk.size() >= 18 && st->land_on_goto.load()) {
                    // A precise GOTO lands on its aim, so the landing check after it (#1105) sees an arrival.
                    std::lock_guard<std::mutex> lock(st->landed_mutex);
                    st->landed = chunk.substr(1, 17) + "#";
                }
                st->goto_started.store(st->now().time_since_epoch().count());
                st->goto_stopped.store(false);
                st->goto_cancelled.store(false);
                st->goto_count.fetch_add(1);
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'T':  // tracking mode write
                if (chunk.size() >= 2) {
                    st->tracking_mode.store(static_cast<unsigned char>(chunk[1]));
                }
                return "#";
            case 'M':  // cancel goto
                st->goto_cancelled.store(true);
                st->cancel_count.fetch_add(1);
                return "#";
            case 'P':  // passthrough
                // A variable-rate move on the RA motor replaces tracking on the handset, and so does its stop.
                if (chunk.size() >= 6 && chunk[1] == 3 && static_cast<unsigned char>(chunk[2]) == 16) {
                    st->tracking_mode.store(0);
                }
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 16 && chunk[4] == 0 && chunk[5] == 0) {
                    st->goto_stopped.store(true);
                }
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(st->model_id.load())) + "#";
            case 't':  // tracking mode read
                return std::string(1, static_cast<char>(st->tracking_mode.load())) + "#";
            case 'w':
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port, int timeout_ms = 1000) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = timeout_ms;
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

// The driver on a caller-owned task clock, so a case steps its waits with advance_until() instead of sleeping.
std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int port, alpacacore::util::TaskClock& clock) {
    return alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(port), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
}

// Runs a blocking driver call on its own thread, stepping the clock until it returns, then rethrows what it threw.
// `started` (optional) must hold, on real time, before the first step: a task parked on the clock from before the
// call (a pulse timer the call reaps) would otherwise satisfy the rendezvous while the call is still on the socket,
// and virtual time would run ahead of it.
template <typename Fn>
void run_virtual(alpacacore::test::FakeTaskClock& clock, Fn&& fn, const std::function<bool()>& started = nullptr) {
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
    const bool began = !started || wait_until([&] { return started() || done.load(); }, 5000);
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

bool is_variable_rate_command(const std::string& command, int device) {
    return command.size() >= 6 && command[0] == 'P' && command[1] == 3 &&
           static_cast<unsigned char>(command[2]) == device;
}

bool is_zero_rate_command(const std::string& command) {
    return command.size() >= 6 && command[4] == 0 && command[5] == 0;
}

int last_goto_index(const std::vector<std::string>& commands) {
    int result = -1;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (!commands[i].empty() && (commands[i][0] == 'r' || commands[i][0] == 'R')) {
            result = static_cast<int>(i);
        }
    }
    return result;
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

TEST_CASE("SynScan async - a GOTO the handset never acknowledges surfaces through Slewing (#575)",
          "[synscan][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // True: published before the task ran. Threw: the task won mutex_ first
    // and its dispatch already failed -- never a silent False.
    const SlewingRead first = read_slewing(*driver);
    REQUIRE((first == SlewingRead::True || first == SlewingRead::Threw));

    // The dispatch times out on the task thread. Today Slewing then reads
    // FALSE as if the mount had arrived; it must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // A retried initiator against a responsive handset clears it.
    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(wait_until([&] { return st->goto_count.load() == 1; }, 5000));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 15000));
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - failed dispatch preserves a stored slew error",
          "[synscan][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    st->reject_goto.store(false);
    REQUIRE(server.drop_connections());
    try {
        driver->move_axis(0, 1.0);
        FAIL("MoveAxis returned although the handset did not answer");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - successful dispatch clears a stored slew error",
          "[synscan][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->move_axis(0, 1.0));
    CHECK(driver->get_slewing());
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan Unpark - a no-op Unpark clears a stored slew error", "[synscan][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->unpark());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - AbortSlew clears a stored slew failure (#575)", "[synscan][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - AbortSlew fences a pending GOTO", "[synscan][telescope][async][abort]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    for (int i = 0; i < 50; ++i) {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
        REQUIRE_NOTHROW(driver->abort_slew());
        const int at_abort_return = st->goto_count.load();
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        INFO("iteration " << i << ", GOTO count at abort return=" << at_abort_return);
        CHECK(st->goto_count.load() == at_abort_return);
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - a stop failure still reaps the cancelled slew task",
          "[synscan][telescope][async][abort]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(server.drop_connections());
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);

    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE_NOTHROW(driver->set_connected(true));
    const int before = st->goto_count.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(wait_until([&] { return st->goto_count.load() > before; }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - a failed tracking restore still clears Slewing (#830)",
          "[synscan][telescope][async][abort]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));
    REQUIRE_NOTHROW(driver->move_axis(0, 1.0));
    REQUIRE(driver->get_slewing());

    st->reject_tracking.store(true);
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);
    st->reject_tracking.store(false);
    CHECK(driver->get_slewing() == false);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - MoveAxis supersedes a pending async slew", "[synscan][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(5.5, 20.0);
    driver->move_axis(0, 0.0);
    // Virtual time for the superseded slew task to wind down.
    clock.advance(std::chrono::milliseconds(100));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    const auto settled = st->command_snapshot();
    const int goto_index = last_goto_index(settled);
    const int stop_index = [&] {
        for (std::size_t i = 0; i < settled.size(); ++i) {
            if (is_variable_rate_command(settled[i], 16) && is_zero_rate_command(settled[i])) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }();
    REQUIRE(stop_index >= 0);
    CHECK((goto_index == -1 || goto_index < stop_index));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan async - blocking slew replaces a pending async target", "[synscan][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(5.5, 20.0);
    std::atomic<bool> sync_done{false};
    std::thread syncer([&] {
        driver->slew_to_coordinates(6.0, 22.0);
        sync_done.store(true);
    });
    const bool finished = advance_until(clock, [&] { return sync_done.load(); });
    syncer.join();
    REQUIRE(finished);
    auto commands = st->command_snapshot();
    int final_goto = last_goto_index(commands);
    REQUIRE(final_goto >= 0);
    const std::string replacement = commands[static_cast<std::size_t>(final_goto)];
    CHECK(replacement.find(',') != std::string::npos);
    // Virtual time for any stray tail of the replaced task.
    clock.advance(std::chrono::milliseconds(100));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    commands = st->command_snapshot();
    final_goto = last_goto_index(commands);
    REQUIRE(final_goto >= 0);
    CHECK(commands[static_cast<std::size_t>(final_goto)] == replacement);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - cross-axis pulses keep the RA tracking restore", "[synscan][telescope][pulseguiding]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);

    const auto started = std::chrono::steady_clock::now();
    driver->pulse_guide(2, 1500);  // East, RA
    driver->pulse_guide(0, 300);   // North, Dec; must not cancel the RA timer
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 5000));
    // The RA pulse ran its whole 1.5 s: a cancelled timer would also end it with T, but early.
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1500));

    const auto commands = st->command_snapshot();
    int last_ra = -1;
    bool tracking_restored_after_ra_stop = false;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (is_variable_rate_command(commands[i], 16)) {
            last_ra = static_cast<int>(i);
            tracking_restored_after_ra_stop = false;
        } else if (last_ra >= 0 && commands[i][0] == 'T') {
            tracking_restored_after_ra_stop = true;
        }
    }
    REQUIRE(last_ra >= 0);
    // An RA move replaces tracking on the handset and ends with the tracking command, not a stop (#1030).
    CHECK_FALSE(is_zero_rate_command(commands[static_cast<std::size_t>(last_ra)]));
    CHECK(tracking_restored_after_ra_stop);
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - short Dec pulse keeps the position override of a long RA pulse (#990)",
          "[synscan][telescope][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The fake mount never moves, so any difference from this reading is the private estimate.
    const double live_ra = driver->get_right_ascension();
    driver->pulse_guide(2, 1500);  // East, RA: the estimate is held until about 5.5 s
    driver->pulse_guide(0, 100);   // North, Dec: its own hold would end at about 4.1 s
    clock.wait_for_waiters(2, std::chrono::seconds(5));  // both pulse timers are parked
    clock.advance(std::chrono::milliseconds(4500));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    // Past the Dec pulse's hold, inside the RA pulse's: the RA estimate must still be served.
    CHECK(std::abs(driver->get_right_ascension() - live_ra) > 1e-6);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - guide position does not publish as target", "[synscan][telescope][pulseguiding]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->pulse_guide(0, 150);
    require_alpaca_error([&] { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&] { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    driver->set_target_right_ascension(5.5);
    driver->set_target_declination(20.0);
    driver->pulse_guide(1, 150);
    CHECK(driver->get_target_right_ascension() == 5.5);
    CHECK(driver->get_target_declination() == 20.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan async slew - position reports mount feedback after PulseGuide", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    st->land_on_goto.store(false);
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const double position_before_pulse_ra = driver->get_right_ascension();
    const double position_before_pulse_dec = driver->get_declination();
    driver->pulse_guide(0, 1000);  // Establish a distinct private guide-position estimate.
    REQUIRE(driver->get_is_pulse_guiding());

    constexpr double target_ra = 6.0;
    constexpr double target_dec = 22.0;
    const bool position_is_distinct =
        std::abs(position_before_pulse_ra - target_ra) > 0.1 || std::abs(position_before_pulse_dec - target_dec) > 1.0;
    CHECK(position_is_distinct);
    driver->slew_to_coordinates_async(target_ra, target_dec);
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    // The mount never moves, so the slew ends with the far-miss failure after one repeat GOTO (#1105).
    REQUIRE(advance_until(clock, [&] { return read_slewing(*driver) != SlewingRead::True; }));

    // The fake mount acknowledges the GOTO but stays at its fixed position.
    // Completion must not make the requested target masquerade as readback.
    CHECK(std::abs(driver->get_right_ascension() - position_before_pulse_ra) < 1e-6);
    CHECK(std::abs(driver->get_declination() - position_before_pulse_dec) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("SynScan - get_link_fault does not wait on a connect holding the driver mutex",
          "[synscan][telescope][link]") {
    // The echo passes, then the handset goes silent: every later handshake query burns its
    // response timeout while set_connected(true) holds the coarse mutex_. The management
    // listing polls get_link_fault() and must not queue behind that.
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (!chunk.empty() && chunk[0] == 'K') {
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        }
        return "";
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    driver->connect();
    // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // inside the handshake
    REQUIRE(driver->get_connecting());
    const auto t0 = std::chrono::steady_clock::now();
    std::string fault;
    std::thread reader([&] { fault = driver->get_link_fault(); });
    reader.join();
    for (const auto& read : std::vector<std::function<void()>>{[&] { (void)driver->get_right_ascension_rate(); },
                                                               [&] { (void)driver->get_declination_rate(); },
                                                               [&] { (void)driver->get_tracking_rate(); }}) {
        try {
            read();
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        }
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // Well under the multi-second handshake timeout a lock wait would cost,
    // with headroom for the sanitizer builds on a loaded runner.
    CHECK(ms < 1000);
    CHECK(fault.empty());
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }, 30000));
}

TEST_CASE("SynScan pulse state refuses a latched link fault without I/O and recovers",
          "[synscan][telescope][link][pulseguiding]") {
    const bool guiding = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    using alpacacore::vendor::synscan::poll_status_now_for_testing;
    REQUIRE_NOTHROW(driver->set_tracking(true));
    if (guiding) REQUIRE_NOTHROW(driver->pulse_guide(0, 25000));
    REQUIRE(driver->get_is_pulse_guiding() == guiding);
    const double altitude = driver->get_altitude();
    // The connect started the status poll, whose first tick is still queued behind the test's exchanges on the
    // wrapper mutex (not FIFO): unpaused, it can read after the third failure below and count a fourth, or
    // after the unmute and clear the latch (nightly run 40958614341). Pausing joins it.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    st->mute.store(true);
    for (int i = 0; i < 3; ++i) {
        poll_status_now_for_testing(*driver);
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
        CHECK(std::string(e.what()) == "SynScan mount communications compromised: " + fault);
    }
    CHECK(st->requests.load() == requests);
    st->mute.store(false);
    poll_status_now_for_testing(*driver);
    CHECK(driver->get_altitude() == altitude);
    CHECK(driver->get_link_fault().empty());
    CHECK(driver->get_is_pulse_guiding() == guiding);
    driver->abort_slew();
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("SynScan commanded state refuses a latched fault while safety stops still reach the handset",
          "[synscan][telescope][link]") {
    const bool moving = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    using alpacacore::vendor::synscan::poll_status_now_for_testing;
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
    // The connect started the status poll, whose first tick is still queued behind the test's exchanges on the
    // wrapper mutex (not FIFO): unpaused, it can read after the third failure below and count a fourth, or
    // after the unmute and clear the latch (nightly run 40958614341). Pausing joins it.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    st->mute.store(true);
    for (int i = 0; i < 3; ++i) {
        poll_status_now_for_testing(*driver);
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
            CHECK(std::string(e.what()) == "SynScan mount communications compromised: " + fault);
        }
    }
    CHECK(st->requests.load() == requests);
    CHECK(driver->get_connected());
    CHECK(driver->get_can_pulse_guide());
    CHECK_FALSE(driver->get_name().empty());
    for (const auto& value : driver->get_device_state()) {
        CHECK(value.name == "TimeStamp");
    }
    st->mute.store(false);  // replies work again, but no poll has cleared the latch
    const int before_stop = st->command_count('P');
    REQUIRE_NOTHROW(driver->move_axis(1, 0.0));
    CHECK(st->command_count('P') > before_stop);
    const int before_abort = st->command_count('P');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('P') > before_abort);
    CHECK(driver->get_link_fault() == fault);
    poll_status_now_for_testing(*driver);
    CHECK(driver->get_link_fault().empty());
    for (const auto& [name, read] : reads) {
        INFO(name);
        CHECK_NOTHROW(read());
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan position - stale cache faults after repeated failed polls and recovers",
          "[synscan][telescope][link]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    using alpacacore::vendor::synscan::poll_status_now_for_testing;
    const double ra = driver->get_right_ascension();
    st->mute.store(true);

    // One failed tick: the frame ages but is still served; a single lost reply is not a fault.
    poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());

    // The third consecutive failed tick latches the fault: the stale frame is no longer an answer.
    poll_status_now_for_testing(*driver);
    poll_status_now_for_testing(*driver);
    std::string last_error;
    try {
        (void)driver->get_right_ascension();
        FAIL("stale position must not be served after a latched link fault");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        last_error = e.what();
    }
    CHECK(driver->get_connected());
    CHECK(last_error.find("communications compromised") != std::string::npos);
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing

    // The faulted frame is not served for Alt/Az either.
    try {
        (void)driver->get_altitude();
        FAIL("a faulted frame must not be served for Alt/Az");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }

    // Recovery is automatic: the next answered tick clears the latch.
    st->mute.store(false);
    poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());
    st->mute.store(false);
    driver->set_connected(false);
}

// SynScan "T" modes (protocol v3.3): 1 = Alt/Az, 2 = equatorial in either hemisphere, 3 = PEC. The handset applies
// its own site, so a southern site must still send 2; 3 would start PEC playback.
TEST_CASE("SynScan tracking - mode follows mount geometry, the same in either hemisphere",
          "[synscan][telescope][tracking]") {
    using alpacacore::vendor::synscan::SynScanAlignmentSetting;
    struct Expected {
        unsigned char model_id;
        SynScanAlignmentSetting setting;
        alpacacore::AlignmentMode alignment;
        bool south;
        unsigned char tracking_mode;
    };
    for (const auto& expected :
         {Expected{50, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::GermanPolar, false, 2},
          Expected{50, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::GermanPolar, true, 2},
          Expected{128, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::AltAz, false, 1},
          Expected{128, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::AltAz, true, 1},
          // #860: the configured geometry decides for an AZ-EQ mount (5 = AZ-EQ6, 6 = AZ-EQ5) ...
          Expected{5, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::GermanPolar, false, 2},
          Expected{6, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::GermanPolar, true, 2},
          Expected{5, SynScanAlignmentSetting::AltAz, alpacacore::AlignmentMode::AltAz, false, 1},
          // ... and is ignored for a mount whose geometry the model ID fixes.
          Expected{50, SynScanAlignmentSetting::AltAz, alpacacore::AlignmentMode::GermanPolar, false, 2},
          Expected{128, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::AltAz, false, 1}}) {
        auto st = std::make_shared<FakeSynScanState>();
        st->model_id.store(expected.model_id);
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, expected.setting);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_site_latitude(expected.south ? -41.3 : 41.3);
        driver->set_site_longitude(174.8);
        CHECK(driver->get_alignment_mode() == expected.alignment);
        driver->set_tracking(true);

        const auto commands = st->command_snapshot();
        const auto tracking = std::find_if(commands.rbegin(), commands.rend(), [](const std::string& command) {
            return command.size() >= 2 && command[0] == 'T';
        });
        REQUIRE(tracking != commands.rend());
        CHECK(static_cast<unsigned char>((*tracking)[1]) == expected.tracking_mode);
        driver->set_connected(false);
    }

    auto st = std::make_shared<FakeSynScanState>();
    st->model_id.store(5);  // AZ-EQ can be used in two alignment configurations; the HC does not report which.
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    // alignmentMode "auto" (or absent) keeps the refusal to guess (#857).
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK_THROWS_AS(driver->get_alignment_mode(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_tracking(true), alpacacore::AlpacaException);
    CHECK(st->command_count('T') == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - a same-axis replacement runs for its requested duration",
          "[synscan][telescope][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    st->clock = &clock;
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->pulse_guide(0, 2000);
    driver->pulse_guide(0, 2000);
    // Virtual time for the replaced pulse task to wind down; the 2 s pulse is still running.
    clock.advance(std::chrono::milliseconds(300));
    clock.wait_for_woken_settled(std::chrono::seconds(2));

    const auto commands = st->command_snapshot();
    int last_dec = -1;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (is_variable_rate_command(commands[i], 17)) {
            last_dec = static_cast<int>(i);
        }
    }
    REQUIRE(last_dec >= 0);
    CHECK_FALSE(is_zero_rate_command(commands[static_cast<std::size_t>(last_dec)]));
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - stops active pulse tasks and restores requested tracking",
          "[synscan][telescope][pulseguiding][abort]") {
    for (const int direction : {0, 2}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
        const int tracking_before = st->command_count('T');
        driver->pulse_guide(direction, 1500);
        REQUIRE(driver->get_is_pulse_guiding());
        REQUIRE_NOTHROW(driver->abort_slew());
        const auto at_abort = st->command_snapshot();
        const int at_abort_size = static_cast<int>(at_abort.size());
        CHECK_FALSE(driver->get_is_pulse_guiding());
        if (direction == 2) {
            CHECK(st->command_count('T') == tracking_before + 1);
            CHECK(driver->get_tracking());
        }
        clock.advance(std::chrono::milliseconds(1700));
        CHECK(clock.wait_for_woken_settled(std::chrono::seconds(2)));
        CHECK(static_cast<int>(st->command_snapshot().size()) == at_abort_size);
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan AbortSlew - does not restore tracking disabled during an RA pulse",
          "[synscan][telescope][pulseguiding][abort]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    driver->pulse_guide(2, 1500);
    driver->set_tracking(false);
    const int tracking_writes_before_abort = st->command_count('T');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_FALSE(driver->get_tracking());
    CHECK(st->command_count('T') == tracking_writes_before_abort);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - SlewToCoordinates reaps the pulse and clears IsPulseGuiding (#831)",
          "[synscan][telescope][pulseguiding][reap]") {
    enum class Op { SlewToCoordinates, SlewToCoordinatesAsync, Park };
    for (const Op op : {Op::SlewToCoordinates, Op::SlewToCoordinatesAsync, Op::Park}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = std::make_shared<FakeSynScanState>();
        st->clock = &clock;
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        driver->pulse_guide(0, 60000);  // outlasts the sync slew, so only the reap can clear it
        REQUIRE(driver->get_is_pulse_guiding());
        switch (op) {
            case Op::SlewToCoordinates:
                run_virtual(
                    clock, [&] { driver->slew_to_coordinates(5.5, 20.0); },
                    [&] { return st->goto_count.load() >= 1; });  // the pulse is reaped, the GOTO is out
                break;
            case Op::SlewToCoordinatesAsync:
                driver->slew_to_coordinates_async(5.5, 20.0);
                break;
            case Op::Park:
                driver->park();
                break;
        }
        INFO("op " << static_cast<int>(op));
        CHECK_FALSE(driver->get_is_pulse_guiding());
        // The async slew and the park run on the task clock: step them out before the disconnect joins them.
        REQUIRE(advance_until(clock, [&] { return !driver->get_slewing(); }));
        driver->set_connected(false);
    }
}

// A reaper stores a cancel flag and then wakes the parked task. If the notify
// does not pass through task_mutex_, a task that has read its flag as false
// but not yet blocked misses it and sleeps until the next advance(), holding
// up the reaper's join. FakeTaskClock's before_block hook holds the first
// parked task in that window until its predicate sees the reaper's store;
// with notify_task_waiters() the reaper is then blocked on task_mutex_ until
// the task blocks, without it the notify goes by while the task is outside
// its wait and the join hangs (same shape as the Sky-Watcher case, #743).
TEST_CASE("SynScan async - a reaper's cancel is not lost between a parked task's check and its block",
          "[synscan][telescope][async][pulseguiding]") {
    std::atomic<bool> at_window{false};
    std::atomic<int> fired{0};
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // Only the status poll is parked on the clock: it registered before the hook is set and its own generation
    // never moves during the case, so it stays blocked past the hook and the pulse task below is the first thread
    // at the hook.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::milliseconds(5000)));
    REQUIRE(clock.waiter_count() == 1);
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
    driver->pulse_guide(0, 2000);  // North: its timer parks on the clock
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

// The mode a stop restores is the one the handset held when the motion started, not the one cached at connect: a
// keypad change in between must not be undone by AbortSlew, MoveAxis(0) or the end of an RA pulse.
namespace {
std::unique_ptr<alpacacore::TelescopeDriver> connect_with_stale_tracking(const std::shared_ptr<FakeSynScanState>& st,
                                                                         alpacacore::test::FakeMountServer& server) {
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());  // fills the driver's cache with the handset's mode 1
    st->tracking_mode = 0;            // the keypad turns tracking off
    return driver;
}
}  // namespace

TEST_CASE("SynScan AbortSlew - does not turn tracking back on after a keypad change",
          "[synscan][telescope][abort][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);
    const int writes_before = st->command_count('T');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - stopping RA does not turn tracking back on after a keypad change",
          "[synscan][telescope][moveaxis][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);
    const int writes_before = st->command_count('T');
    driver->move_axis(0, 0.5);
    driver->move_axis(0, 0.0);
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - the end of an RA pulse does not turn tracking back on after a keypad change",
          "[synscan][telescope][pulseguiding][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);
    const int writes_before = st->command_count('T');
    driver->pulse_guide(2, 200);
    REQUIRE(wait_until([&]() { return !driver->get_is_pulse_guiding(); }, 3000));
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan SlewToCoordinates - refuses after the keypad turned tracking off",
          "[synscan][telescope][slew][tracking]") {
    for (const bool async : {false, true}) {
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = connect_with_stale_tracking(st, server);
        INFO("async " << async);
        require_alpaca_error(
            [&]() {
                if (async) {
                    driver->slew_to_coordinates_async(5.5, 20.0);
                } else {
                    driver->slew_to_coordinates(5.5, 20.0);
                }
            },
            alpacacore::AlpacaError::InvalidOperation);
        CHECK(st->goto_count.load() == 0);
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan idle stops - still sent when the tracking read times out", "[synscan][telescope][tracking]") {
    for (const bool abort : {true, false}) {
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port(), 300), alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_tracking());  // cache: mode 1
        st->reject_tracking_read = true;
        const int stops_before = st->command_count('P');
        const int writes_before = st->command_count('T');
        INFO("abort " << abort);
        if (abort) {
            REQUIRE_NOTHROW(driver->abort_slew());
        } else {
            REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
        }
        CHECK(st->command_count('P') > stops_before);
        CHECK(st->command_count('T') == writes_before + 1);  // the cached mode, since the read failed
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan stops - restore the mode the handset held when the motion started",
          "[synscan][telescope][tracking]") {
    // The keypad moved tracking from 1 to 2 before the motion; the stop restores 2, not the cached 1.
    for (const int site : {0, 1, 2}) {
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = connect_with_stale_tracking(st, server);
        st->tracking_mode = 2;
        const int writes_before = st->command_count('T');
        INFO("stop site " << site);
        if (site == 0) {
            driver->abort_slew();
        } else if (site == 1) {
            driver->move_axis(0, 0.5);
            driver->move_axis(0, 0.0);
        } else {
            driver->pulse_guide(2, 200);
            REQUIRE(wait_until([&]() { return !driver->get_is_pulse_guiding(); }, 3000));
        }
        const auto commands = st->command_snapshot();
        REQUIRE(st->command_count('T') == writes_before + 1);
        const auto last_t = std::find_if(commands.rbegin(), commands.rend(),
                                         [](const std::string& c) { return !c.empty() && c[0] == 'T'; });
        REQUIRE(last_t != commands.rend());
        CHECK(static_cast<int>(static_cast<unsigned char>((*last_t)[1])) == 2);
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan AbortSlew - a finished RA pulse no longer counts as running when the keypad changes tracking",
          "[synscan][telescope][pulseguiding][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());
    driver->pulse_guide(2, 100);
    // No getter is read while waiting: only IsPulseGuiding clears the driver's pulse flag, and the pulse's end
    // time (duration plus completion delay) has passed by now.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    st->tracking_mode = 0;
    const int writes_before = st->command_count('T');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    driver->set_connected(false);
}

// The handset turns tracking off when an RA move starts, so a stop that read `t` while the motion ran would see 0 and
// leave the mount not tracking. Each case starts at mode 2 and the stop must write it back.
namespace {
int last_tracking_write(FakeSynScanState& st) {
    const auto commands = st.command_snapshot();
    const auto last_t = std::find_if(commands.rbegin(), commands.rend(),
                                     [](const std::string& c) { return !c.empty() && c[0] == 'T'; });
    return last_t == commands.rend() ? -1 : static_cast<int>(static_cast<unsigned char>((*last_t)[1]));
}

std::unique_ptr<alpacacore::TelescopeDriver> connect_tracking_mode_2(const std::shared_ptr<FakeSynScanState>& st,
                                                                     alpacacore::test::FakeMountServer& server) {
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    st->tracking_mode = 2;
    REQUIRE(driver->get_tracking());
    return driver;
}
}  // namespace

TEST_CASE("SynScan MoveAxis - stopping an RA jog restores the mode the handset held before it",
          "[synscan][telescope][moveaxis][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_tracking_mode_2(st, server);
    driver->move_axis(0, 0.5);
    REQUIRE(st->tracking_mode.load() == 0);  // the jog replaced tracking
    driver->move_axis(0, 0.0);
    CHECK(last_tracking_write(*st) == 2);
    CHECK(st->tracking_mode.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - during an RA pulse restores the mode the handset held before it",
          "[synscan][telescope][pulseguiding][abort][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_tracking_mode_2(st, server);
    driver->pulse_guide(2, 1500);
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(last_tracking_write(*st) == 2);
    CHECK(st->tracking_mode.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - a pulse replaced mid-pulse restores the mode held before the first",
          "[synscan][telescope][pulseguiding][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_tracking_mode_2(st, server);
    driver->pulse_guide(2, 1500);
    REQUIRE(driver->get_is_pulse_guiding());
    driver->pulse_guide(2, 200);
    REQUIRE(wait_until([&]() { return !driver->get_is_pulse_guiding(); }, 4000));
    CHECK(last_tracking_write(*st) == 2);
    CHECK(st->tracking_mode.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - a Dec jog does not hide a keypad change from the next RA jog",
          "[synscan][telescope][moveaxis][tracking]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);  // cache 1, handset 0
    const int writes_before = st->command_count('T');
    driver->move_axis(1, 0.5);
    driver->move_axis(0, 0.5);
    driver->move_axis(0, 0.0);
    CHECK(st->command_count('T') == writes_before);
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

// #2412: a GOTO the handset still reports in progress after the 120 s slew deadline made the wait throw and send no
// stop, so the mount kept driving after the client was told the slew failed. The deadline now stops the mount (cancel
// GOTO, both axes to rate 0, the tracking restore) and keeps the error for Slewing until the next motion command or a
// reconnect; a stop that fails says the mount may still be moving.
namespace {
constexpr auto kSlewDeadline = std::chrono::seconds(120);  // the driver's kSlewTimeout

// Steps virtual time in 1 s steps until `pred` holds, for at most `budget` of virtual time.
bool advance_virtual_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred,
                           std::chrono::seconds budget = std::chrono::seconds(200)) {
    return advance_until(clock, pred, std::chrono::seconds(1), static_cast<int>(budget.count()));
}

std::unique_ptr<alpacacore::TelescopeDriver> connect_with_hanging_goto(const std::shared_ptr<FakeSynScanState>& st,
                                                                       alpacacore::test::FakeMountServer& server,
                                                                       alpacacore::test::FakeTaskClock& clock) {
    st->clock = &clock;
    st->goto_hangs.store(true);
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());
    return driver;
}
}  // namespace

TEST_CASE("SynScan async - a GOTO past its deadline stops the mount and keeps the timeout for Slewing (#2412)",
          "[synscan][telescope][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_hanging_goto(st, server, clock);
    const int tracking_writes_before = st->command_count('T');

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(advance_virtual_until(clock, [&] { return read_slewing(*driver) != SlewingRead::True; }));

    std::string message;
    REQUIRE(read_slewing(*driver, &message) == SlewingRead::Threw);
    // The kept text itself: the async body passes the deadline's error on, it does not re-record it as a dispatch
    // failure (which would also clear Slewing behind a stop that failed).
    CHECK(message == "Slew timed out after 120 s; the mount was stopped");
    CHECK(st->cancel_count.load() >= 1);  // the GOTO was cancelled on the wire
    CHECK_FALSE(st->goto_in_progress());
    CHECK(st->command_count('T') == tracking_writes_before + 1);  // the RA stop's tracking restore
    CHECK(driver->get_tracking());
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // kept, not one-shot

    // The next motion command clears it.
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SynScan SlewToCoordinates - a blocking GOTO past its deadline stops the mount and keeps the error (#2412)",
          "[synscan][telescope][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_hanging_goto(st, server, clock);

    std::atomic<bool> done{false};
    std::string thrown;
    int thrown_code = 0;
    std::thread caller([&] {
        try {
            driver->slew_to_coordinates(5.5, 20.0);
        } catch (const alpacacore::AlpacaException& ex) {
            thrown = ex.what();
            thrown_code = ex.error_code();
        }
        done.store(true);
    });
    const bool began = wait_until([&] { return st->goto_count.load() > 0 || done.load(); }, 5000);
    const bool finished = began && advance_virtual_until(clock, [&] { return done.load(); });
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));  // release a hung caller so the thread can join
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    caller.join();
    REQUIRE(finished);
    CHECK(thrown_code == alpacacore::AlpacaError::DriverException);
    CHECK(thrown.find("Slew timed out") != std::string::npos);
    CHECK(thrown.find("the mount was stopped") != std::string::npos);
    CHECK(st->cancel_count.load() >= 1);
    CHECK_FALSE(st->goto_in_progress());

    // Slewing raises the kept error rather than reading true (or false) behind the blocking caller's error.
    std::string message;
    CHECK(read_slewing(*driver, &message) == SlewingRead::Threw);
    CHECK(message == thrown);

    // A reconnect clears it.
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - a deadline stop that fails says the mount may still be moving (#2412)",
          "[synscan][telescope][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = connect_with_hanging_goto(st, server, clock);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    const auto started = clock.now();
    // Up to just short of the deadline, then the link goes: the status reads that follow fail and keep the last
    // answer (in progress), and the stops at the deadline meet a closed peer. The SynScan stops are blind sends, so
    // only a dead link can fail them.
    REQUIRE(
        advance_virtual_until(clock, [&] { return clock.now() - started >= kSlewDeadline - std::chrono::seconds(3); }));
    REQUIRE(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(server.drop_connections());
    // The dead link latches a fault, which Slewing raises too; the kept slew error takes precedence once it exists.
    std::string message;
    REQUIRE(advance_virtual_until(
        clock,
        [&] {
            message.clear();
            return read_slewing(*driver, &message) != SlewingRead::True &&
                   message.find("Slew timed out") != std::string::npos;
        },
        std::chrono::seconds(30)));
    REQUIRE(read_slewing(*driver, &message) == SlewingRead::Threw);
    // The kept text, not re-recorded by the async body's dispatch-failure path, which would clear Slewing on a mount
    // that may still be moving.
    CHECK(message.rfind("Slew timed out after 120 s; stopping after failure failed: ", 0) == 0);
    CHECK(message.find("the mount may still be moving") != std::string::npos);
    CHECK(message.find("the mount was stopped") == std::string::npos);
    driver->set_connected(false);
}

// #2412 (#2396 AB-9): the MoveAxis bound. A MoveAxis that no new non-zero MoveAxis renews within 60 s is stopped by
// the driver on its own slot (no client needs to poll), and Slewing raises it until the next motion command or a
// reconnect. The status poll is paused, so the bound body is the only thing on the task clock.
namespace {

namespace sc_bound = alpacacore::vendor::synscan;

struct MoveAxisBoundRig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeSynScanState> st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server{synscan_responder(st)};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    std::atomic<int> infos{0};
    std::atomic<int> warns{0};
    alpacacore::logging::LogSink previous_sink = alpacacore::logging::get_log_sink();
    std::atomic<std::thread::id> bound_thread{};

    MoveAxisBoundRig() {
        st->clock = &clock;
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (component != "SynScan" || message.find("MoveAxis on axis") == std::string_view::npos) return;
                if (level == alpacacore::logging::LogLevel::Info) ++infos;
                if (level == alpacacore::logging::LogLevel::Warn || level == alpacacore::logging::LogLevel::Error) {
                    ++warns;
                }
            });
        REQUIRE(server.ok());
        driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        sc_bound::set_status_poll_paused_for_testing(*driver, true);
        // Names the bound body's thread, so start_move() waits for that body (not any waiter) to park.
        sc_bound::set_motion_bound_spawn_for_testing(*driver, [this](std::function<void()> body) {
            std::thread thread(std::move(body));
            bound_thread.store(thread.get_id());
            return thread;
        });
    }
    ~MoveAxisBoundRig() {
        st->mute.store(false);
        driver->set_connected(false);
        alpacacore::logging::set_log_sink(previous_sink);
    }
    MoveAxisBoundRig(const MoveAxisBoundRig&) = delete;
    MoveAxisBoundRig& operator=(const MoveAxisBoundRig&) = delete;

    // Starts a MoveAxis and waits until the bound body's own thread is parked on the clock. Not wait_for_waiters(): the
    // paused status poll parks and wakes on the same clock, so a count can be met before the bound body waits, and a
    // step that lands between its read of the clock and its wait moves its deadline.
    void start_move(int axis) {
        REQUIRE_NOTHROW(driver->move_axis(axis, 1.0));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (bound_thread.load() == std::thread::id{} && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(clock.wait_for_waiter_thread(bound_thread.load(), std::chrono::seconds(5)));
    }

    // In 5 s steps, polling the handset after each one: the status poll is paused, and a frame older than the
    // snapshot bound would read as a lost link.
    void advance(std::chrono::seconds by) {
        for (auto left = by; left.count() > 0; left -= std::chrono::seconds(5)) {
            clock.advance(std::min(left, std::chrono::seconds(5)));
            REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(10)));
            sc_bound::poll_status_now_for_testing(*driver);
        }
    }
    // Zero-rate variable-rate commands; one axis stop sends kStopCommands of them.
    int stops(int device) {
        const auto commands = st->command_snapshot();
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [device](const std::string& c) {
            return is_variable_rate_command(c, device) && is_zero_rate_command(c);
        }));
    }
    int starts(int device) {
        const auto commands = st->command_snapshot();
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [device](const std::string& c) {
            return is_variable_rate_command(c, device) && !is_zero_rate_command(c);
        }));
    }
};

constexpr int kStopCommands = 2;  // the wrapper's stop: direction 6 and 7 at rate 0
constexpr int kRaMotor = 16;
constexpr int kDecMotor = 17;

}  // namespace

TEST_CASE("SynScan MoveAxis - a MoveAxis not renewed for 60 s is stopped and Slewing raises it",
          "[synscan][telescope][goto-safety]") {
    const int axis = GENERATE(0, 1);
    const int motor = axis == 0 ? kRaMotor : kDecMotor;
    MoveAxisBoundRig rig;
    rig.start_move(axis);
    rig.advance(std::chrono::seconds(59));
    CHECK(rig.stops(motor) == 0);
    CHECK(read_slewing(*rig.driver) == SlewingRead::True);

    rig.advance(std::chrono::seconds(2));
    CHECK(rig.stops(motor) == kStopCommands);
    std::string message;
    CHECK(read_slewing(*rig.driver, &message) == SlewingRead::Threw);
    CHECK(message.find("ran 60 s, the longest allowed; the driver stopped it") != std::string::npos);
    CHECK(rig.infos.load() == 1);  // a client can cause it: INFO, never WARN (decision 0007 rule 9)
    CHECK(rig.warns.load() == 0);
    rig.advance(std::chrono::seconds(60));
    CHECK(rig.stops(motor) == kStopCommands);  // acted once: nothing more is sent for this move

    // The next motion command clears it.
    REQUIRE_NOTHROW(rig.driver->move_axis(axis, 0.0));
    CHECK(read_slewing(*rig.driver) == SlewingRead::False);
}

// #2412: a rate command that threw may still have reached the handset, which then moves the axis. The driver cannot
// know, so it fails closed: the axis keeps reading as moving and the 60 s bound watches it. Here the write fails on
// a socket the peer closed, which is not a lost link yet.
TEST_CASE("SynScan MoveAxis - a start that threw is still bounded", "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    REQUIRE(rig.server.drop_connections());
    // Dec: an RA start reads the tracking mode first, and that read fails before any rate command is sent.
    CHECK_THROWS_AS(rig.driver->move_axis(1, 1.0), alpacacore::AlpacaException);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rig.bound_thread.load() == std::thread::id{} && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(rig.bound_thread.load() != std::thread::id{});  // the bound watches the axis
    REQUIRE(rig.clock.wait_for_waiter_thread(rig.bound_thread.load(), std::chrono::seconds(5)));
    rig.advance(std::chrono::seconds(61));
    CHECK(rig.warns.load() == 1);  // at 60 s it tried the axis's stop (the dropped link refused it)
}

// A stop the handset answered is known to have landed: only its tracking restore failed. The axis is not put back
// under the bound (no Slewing true, no "ran 60 s" error later for a move the client already stopped).
TEST_CASE("SynScan MoveAxis - a stop whose tracking restore failed is not bounded",
          "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    REQUIRE_NOTHROW(rig.driver->set_tracking(true));
    rig.start_move(0);
    rig.st->reject_tracking.store(true);  // the `T` restore after the stop times out on a live link
    CHECK_THROWS_AS(rig.driver->move_axis(0, 0.0), alpacacore::AlpacaException);
    rig.st->reject_tracking.store(false);
    CHECK(rig.stops(kRaMotor) == kStopCommands);  // the client's stop went out and was answered
    rig.advance(std::chrono::seconds(61));
    CHECK(rig.stops(kRaMotor) == kStopCommands);  // the bound sent no stop of its own
    CHECK(rig.infos.load() == 0);
    CHECK(rig.warns.load() == 0);
}

// The same for a stop: MoveAxis(axis, 0) that threw may not have stopped the axis, so it stays watched.
TEST_CASE("SynScan MoveAxis - a stop that threw leaves the axis bounded", "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    rig.start_move(1);
    rig.advance(std::chrono::seconds(30));
    REQUIRE(rig.server.drop_connections());
    CHECK_THROWS_AS(rig.driver->move_axis(1, 0.0), alpacacore::AlpacaException);
    rig.advance(std::chrono::seconds(61));
    CHECK(rig.warns.load() == 1);  // the bound still tried the axis's stop
}

TEST_CASE("SynScan MoveAxis - each non-zero MoveAxis restarts the 60 s bound", "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    rig.start_move(0);
    rig.advance(std::chrono::seconds(40));
    REQUIRE_NOTHROW(rig.driver->move_axis(0, 2.0));  // renewed at 40 s
    rig.advance(std::chrono::seconds(40));           // 80 s from the first, 40 s from the renewal
    CHECK(rig.stops(kRaMotor) == 0);
    CHECK(read_slewing(*rig.driver) == SlewingRead::True);
    rig.advance(std::chrono::seconds(21));
    CHECK(rig.stops(kRaMotor) == kStopCommands);
    CHECK(read_slewing(*rig.driver) == SlewingRead::Threw);
}

TEST_CASE("SynScan MoveAxis - a bound stop that fails is not retried and sends no start",
          "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    rig.start_move(0);
    const int starts = rig.starts(kRaMotor);
    REQUIRE(rig.server.drop_connections());  // the stop's write meets a dropped link
    rig.advance(std::chrono::seconds(61));
    require_alpaca_error([&] { (void)rig.driver->get_slewing(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(rig.warns.load() == 1);  // a stop that could not be sent is a device failure
    CHECK(rig.infos.load() == 0);
    const int requests = rig.st->requests.load();
    rig.advance(std::chrono::seconds(120));
    CHECK(rig.st->requests.load() == requests);  // nothing more was sent
    CHECK(rig.starts(kRaMotor) == starts);
}

TEST_CASE("SynScan MoveAxis - a bound failure does not outlive a reconnect", "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    rig.start_move(0);
    rig.advance(std::chrono::seconds(61));
    REQUIRE(read_slewing(*rig.driver) == SlewingRead::Threw);
    rig.driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*rig.driver, true, std::chrono::seconds(10)));
    CHECK(read_slewing(*rig.driver) == SlewingRead::False);
}

// #2412: site, time and Sync writes change the handset's limit and pointing maths, so they are refused while motion
// the driver started is under way, with nothing sent.
TEST_CASE("SynScan - site, time and Sync writes are refused while the mount moves",
          "[synscan][telescope][goto-safety]") {
    const bool goto_motion = GENERATE(false, true);
    MoveAxisBoundRig rig;
    if (goto_motion) {
        rig.st->goto_hangs.store(true);  // the GOTO body keeps waiting on it
        REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
        REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    } else {
        rig.start_move(0);
    }
    const int requests = rig.st->requests.load();
    require_alpaca_error([&] { rig.driver->set_site_latitude(10.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { rig.driver->set_site_longitude(10.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { rig.driver->set_utc_date(std::chrono::system_clock::now()); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { rig.driver->sync_to_coordinates(5.5, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    CHECK(rig.st->requests.load() == requests);

    REQUIRE_NOTHROW(rig.driver->abort_slew());
    CHECK_NOTHROW(rig.driver->set_site_latitude(10.0));
    CHECK_NOTHROW(rig.driver->set_utc_date(std::chrono::system_clock::now()));
    CHECK_NOTHROW(rig.driver->sync_to_coordinates(5.5, 20.0));
}

TEST_CASE("SynScan MoveAxis - a bound the slot cannot start stops the axis and fails the MoveAxis",
          "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    sc_bound::set_motion_bound_spawn_for_testing(*rig.driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    try {
        rig.driver->move_axis(1, 1.0);
        FAIL("MoveAxis returned although its time limit could not start");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("the axis was stopped") != std::string::npos);
    }
    CHECK(rig.stops(kDecMotor) == kStopCommands);  // fails closed: the move just started is stopped
    CHECK(read_slewing(*rig.driver) == SlewingRead::False);
}

// The second check of a site or UTCDate write runs at the write's own wire turn: a MoveAxis that set its flags while
// the write waited for its turn (queued behind it) is refused against, and the write sends nothing.
TEST_CASE("SynScan - a site or time write is refused at its wire turn by a MoveAxis queued meanwhile",
          "[synscan][telescope][goto-safety]") {
    const bool utc = GENERATE(false, true);
    MoveAxisBoundRig rig;
    std::promise<void> move_queued;
    std::atomic<int> hook_calls{0};
    std::atomic<bool> queued_in_time{false};  // set on the driver's thread; checked here (no REQUIRE off-thread)
    std::thread mover;
    struct ClearHook {
        alpacacore::TelescopeDriver& driver;
        std::thread& mover;
        ~ClearHook() {
            sc_bound::set_wire_turn_wait_hook_for_testing(driver, nullptr);
            if (mover.joinable()) mover.join();
        }
    } clear_hook{*rig.driver, mover};
    sc_bound::set_wire_turn_wait_hook_for_testing(*rig.driver, [&] {
        const int call = hook_calls.fetch_add(1);
        if (call == 0) {
            // The write's turn wait: start a MoveAxis, and wait until it waits for its own turn behind the write, its
            // flags already set.
            mover = std::thread([&] { (void)rig.driver->move_axis(0, 1.0); });
            queued_in_time.store(move_queued.get_future().wait_for(std::chrono::seconds(5)) ==
                                 std::future_status::ready);
        } else if (call == 1) {
            move_queued.set_value();
        }
    });
    const auto writes = [&] { return rig.st->command_count('W') + rig.st->command_count('H'); };
    const int before = writes();
    if (utc) {
        require_alpaca_error([&] { rig.driver->set_utc_date(std::chrono::system_clock::now()); },
                             alpacacore::AlpacaError::InvalidOperation);
    } else {
        require_alpaca_error([&] { rig.driver->set_site_latitude(10.0); }, alpacacore::AlpacaError::InvalidOperation);
    }
    mover.join();
    CHECK(queued_in_time.load());
    CHECK(writes() == before);
    CHECK(rig.starts(kRaMotor) == 1);  // the MoveAxis went out after the refused write
}

// Decision 0016 rule 6: the bound's stop releases mutex_ while it is on the wire. A renewing MoveAxis on the same axis
// that lands then keeps its own flags: Slewing stays true behind it and the bound stops it at its own 60 s.
TEST_CASE("SynScan MoveAxis - a renewal that lands while the bound's stop is on the wire stays bounded",
          "[synscan][telescope][goto-safety]") {
    MoveAxisBoundRig rig;
    rig.start_move(0);
    std::promise<void> renewal_queued;
    std::atomic<bool> queued_in_time{false};
    std::atomic<bool> fired{false};
    std::atomic<std::thread::id> renewer_id{};  // set by the renewal's own thread before it calls MoveAxis
    std::thread renewer;
    struct Cleanup {
        MoveAxisBoundRig& rig;
        std::thread& renewer;
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(rig.st->before_reply_mutex);
                rig.st->before_reply = nullptr;
            }
            sc_bound::set_wire_turn_wait_hook_for_testing(*rig.driver, nullptr);
            if (renewer.joinable()) renewer.join();
        }
    } cleanup{rig, renewer};
    // The renewal's turn wait signals it has set its flags and queued behind the stop.
    sc_bound::set_wire_turn_wait_hook_for_testing(*rig.driver, [&] {
        if (std::this_thread::get_id() == renewer_id.load()) renewal_queued.set_value();
    });
    {
        std::lock_guard<std::mutex> lock(rig.st->before_reply_mutex);
        rig.st->before_reply = [&](const std::string& chunk) {
            // The bound's first stop command is on the wire: land a renewal now, before the handset answers.
            if (is_variable_rate_command(chunk, kRaMotor) && is_zero_rate_command(chunk) && !fired.exchange(true)) {
                renewer = std::thread([&] {
                    renewer_id.store(std::this_thread::get_id());
                    (void)rig.driver->move_axis(0, 2.0);
                });
                queued_in_time.store(renewal_queued.get_future().wait_for(std::chrono::seconds(5)) ==
                                     std::future_status::ready);
            }
        };
    }
    const int starts = rig.starts(kRaMotor);
    rig.advance(std::chrono::seconds(61));
    renewer.join();
    CHECK(queued_in_time.load());
    CHECK(rig.starts(kRaMotor) == starts + 1);              // the renewal's rate went out after the stop
    CHECK(read_slewing(*rig.driver) == SlewingRead::True);  // the renewed move, not the stopped one
    const int stops = rig.stops(kRaMotor);
    rig.advance(std::chrono::seconds(61));  // the renewal's own 60 s
    CHECK(rig.stops(kRaMotor) == stops + kStopCommands);
    CHECK(read_slewing(*rig.driver) == SlewingRead::Threw);
}

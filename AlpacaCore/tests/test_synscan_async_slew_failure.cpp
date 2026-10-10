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
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == expected_code);
    }
}

struct FakeSynScanState {
    // The GOTO timer reads this clock. A case that steps the driver on a
    // FakeTaskClock points it there, so the slew runs in virtual time too.
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    alpacacore::util::TaskClock::clock::time_point now() const { return clock->now(); }
    std::atomic<bool> reject_goto{false};  // true: swallow the GOTO -> the wrapper times out and throws
    std::atomic<bool> reject_tracking{false};  // true: swallow the tracking write 'T' -> the wrapper times out
    std::atomic<bool> mute{false};
    std::atomic<unsigned char> model_id{50};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<bool> goto_stopped{false};
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
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return !goto_stopped.load() && now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder synscan_responder(const std::shared_ptr<FakeSynScanState>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (st->mute.load()) return "";
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'P' || chunk[0] == 'T' || chunk[0] == 'r' || chunk[0] == 'R') {
            st->record(chunk);
        }
        if (chunk[0] == 'T' && st->reject_tracking.load()) return "";
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#" (the connect-time link check)
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                if (st->reject_goto.load()) {
                    return "";  // no reply at all: the response timeout fires inside the slew task
                }
                st->goto_started.store(st->now().time_since_epoch().count());
                st->goto_stopped.store(false);
                st->goto_count.fetch_add(1);
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
            case 'T':  // tracking mode write
            case 'P':  // passthrough
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 16 && chunk[4] == 0 && chunk[5] == 0) {
                    st->goto_stopped.store(true);
                }
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(st->model_id.load())) + "#";
            case 't':  // tracking mode read: sidereal tracking on
                return std::string(1, '\x01') + "#";
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

    driver->pulse_guide(2, 1500);  // East, RA
    driver->pulse_guide(0, 300);   // North, Dec; must not cancel the RA timer
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 5000));

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
    CHECK(is_zero_rate_command(commands[static_cast<std::size_t>(last_ra)]));
    CHECK(tracking_restored_after_ra_stop);
    CHECK(driver->get_tracking());
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
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 12000));

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
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // Well under the multi-second handshake timeout a lock wait would cost,
    // with headroom for the sanitizer builds on a loaded runner.
    CHECK(ms < 1000);
    CHECK(fault.empty());
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }, 30000));
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
    const double ra = driver->get_right_ascension();
    clock.advance(std::chrono::milliseconds(2100));  // expire the 2 s position cache
    st->mute.store(true);

    std::string last_error;
    for (int i = 0; i < 3; ++i) {
        try {
            (void)driver->get_right_ascension();
            FAIL("stale position must not be served after a failed mount read");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
            last_error = e.what();
        }
    }
    CHECK(driver->get_connected());
    CHECK(last_error.find("communications compromised") != std::string::npos);
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing

    // The faulted cache is not served for Alt/Az either; a good request clears the latch.
    try {
        (void)driver->get_altitude();
        FAIL("faulted position cache must force a hardware poll");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
    st->mute.store(false);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());

    st->mute.store(true);
    clock.advance(std::chrono::milliseconds(2100));  // expire the recovered position cache
    try {
        (void)driver->get_right_ascension();
        FAIL("an unavailable mount must not return cached coordinates");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(std::string(e.what()).find("communications compromised") == std::string::npos);
    }
    st->mute.store(false);
    driver->set_connected(false);
}

TEST_CASE("SynScan tracking - mode follows mount geometry and site hemisphere", "[synscan][telescope][tracking]") {
    using alpacacore::vendor::synscan::SynScanAlignmentSetting;
    struct Expected {
        unsigned char model_id;
        SynScanAlignmentSetting setting;
        alpacacore::AlignmentMode alignment;
        unsigned char tracking_mode;
    };
    for (const auto& expected :
         {Expected{50, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::GermanPolar, 2},
          Expected{50, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::GermanPolar, 3},
          Expected{128, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::AltAz, 1},
          // #860: the configured geometry decides for an AZ-EQ mount (5 = AZ-EQ6, 6 = AZ-EQ5) ...
          Expected{5, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::GermanPolar, 2},
          Expected{6, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::GermanPolar, 3},
          Expected{5, SynScanAlignmentSetting::AltAz, alpacacore::AlignmentMode::AltAz, 1},
          // ... and is ignored for a mount whose geometry the model ID fixes.
          Expected{50, SynScanAlignmentSetting::AltAz, alpacacore::AlignmentMode::GermanPolar, 2},
          Expected{128, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::AltAz, 1}}) {
        auto st = std::make_shared<FakeSynScanState>();
        st->model_id.store(expected.model_id);
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, expected.setting);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_site_latitude(expected.tracking_mode == 2 ? 41.3 : -41.3);
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
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        driver->pulse_guide(0, 60000);  // outlasts the sync slew, so only the reap can clear it
        REQUIRE(driver->get_is_pulse_guiding());
        switch (op) {
            case Op::SlewToCoordinates:
                driver->slew_to_coordinates(5.5, 20.0);
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

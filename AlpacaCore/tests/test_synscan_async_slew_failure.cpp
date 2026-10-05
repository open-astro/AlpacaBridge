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
    std::atomic<bool> reject_goto{false};  // true: swallow the GOTO -> the wrapper times out and throws
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
        return !goto_stopped.load() && Clock::now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder synscan_responder(const std::shared_ptr<FakeSynScanState>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'P' || chunk[0] == 'T' || chunk[0] == 'r' || chunk[0] == 'R') {
            st->record(chunk);
        }
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
                st->goto_started.store(Clock::now().time_since_epoch().count());
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
                return std::string(1, static_cast<char>(50)) + "#";  // EQM-35 Pro
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 1000;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

TEST_CASE("SynScan async - MoveAxis supersedes a pending async slew", "[synscan][telescope][async][ownership]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(5.5, 20.0);
    driver->move_axis(0, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(5.5, 20.0);
    driver->slew_to_coordinates(6.0, 22.0);
    auto commands = st->command_snapshot();
    int final_goto = last_goto_index(commands);
    REQUIRE(final_goto >= 0);
    const std::string replacement = commands[static_cast<std::size_t>(final_goto)];
    CHECK(replacement.find(',') != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

TEST_CASE("SynScan async slew - completion reports the new target after PulseGuide", "[synscan][telescope][async]") {
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

    CHECK(std::abs(driver->get_right_ascension() - target_ra) < 1e-6);
    CHECK(std::abs(driver->get_declination() - target_dec) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - a same-axis replacement runs for its requested duration",
          "[synscan][telescope][pulseguiding]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->pulse_guide(0, 2000);
    driver->pulse_guide(0, 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

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
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1700));
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

#endif  // _WIN32

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

// Operation-slot start refusals, SynScan (decision 0006, "Thread refusal"): an
// initiator that cancelled a goto body in flight and then has its slot start
// refused has to stop the mount itself and publish the AbortSlew state; a
// refusal that replaced nothing restores every flag the initiator set and
// sends no stop.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeSynScanState {
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    alpacacore::util::TaskClock::clock::time_point now() const { return clock->now(); }
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<bool> goto_stopped{false};
    std::mutex command_mutex;
    std::vector<std::string> commands;
    void record(const std::string& command) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.push_back(command);
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
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'P' || chunk[0] == 'M') {
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
                st->goto_started.store(st->now().time_since_epoch().count());
                st->goto_stopped.store(false);
                st->goto_count.fetch_add(1);
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                st->goto_stopped.store(true);
                return "#";
            case 'T':  // tracking mode write
            case 'P':  // passthrough
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(50)) + "#";
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
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

void refuse_slot_threads(alpacacore::TelescopeDriver& driver) {
    alpacacore::vendor::synscan::set_slew_spawn_for_testing(driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
}

struct Rig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeSynScanState> st = std::make_shared<FakeSynScanState>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    Rig() {
        st->clock = &clock;
        server = std::make_unique<alpacacore::test::FakeMountServer>(synscan_responder(st));
        REQUIRE(server->ok());
        driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server->port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    }
    ~Rig() { driver->set_connected(false); }
};

// Starts a goto and lets it dispatch, then issues `second` while the slot's
// thread factory refuses (EAGAIN). The initiator has by then cancelled the
// goto body in flight, so nobody else will stop the mount.
void refused_slot_start_stops_mount(const std::function<void(alpacacore::TelescopeDriver&)>& second) {
    Rig rig;
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    REQUIRE(rig.driver->get_slewing());
    const int cancels_before = rig.st->command_count('M');

    refuse_slot_threads(*rig.driver);
    CHECK_THROWS_AS(second(*rig.driver), alpacacore::AlpacaException);

    CHECK(rig.st->command_count('M') == cancels_before + 1);  // the GOTO was cancelled
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
}

// A refused start that replaced nothing must put back every field the
// initiator's locked block wrote, not only Slewing: a MoveAxis in motion stays
// reported as Slewing, and no stop is sent because the axis is still driven.
void refused_start_keeps_move_axis(const std::function<void(alpacacore::TelescopeDriver&)>& second) {
    Rig rig;
    rig.driver->move_axis(0, 0.5);
    REQUIRE(rig.driver->get_slewing());
    const int cancels_before = rig.st->command_count('M');
    const int passthrough_before = rig.st->command_count('P');

    refuse_slot_threads(*rig.driver);
    CHECK_THROWS_AS(second(*rig.driver), alpacacore::AlpacaException);

    CHECK(rig.st->command_count('M') == cancels_before);  // nothing in the slot: nothing to cancel
    CHECK(rig.st->command_count('P') == passthrough_before);
    CHECK(rig.driver->get_slewing());  // axis 0 is still driven
}

}  // namespace

TEST_CASE("SynScan slot - a refused goto start stops the mount when it replaced a goto",
          "[synscan][telescope][async][slot]") {
    refused_slot_start_stops_mount([](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(6.5, 25.0); });
}

TEST_CASE("SynScan slot - a refused park start stops the mount when it replaced a goto",
          "[synscan][telescope][async][slot]") {
    refused_slot_start_stops_mount([](alpacacore::TelescopeDriver& d) { d.park(); });
}

TEST_CASE("SynScan slot - a refused goto start on an idle mount cancels nothing", "[synscan][telescope][async][slot]") {
    Rig rig;
    const int cancels_before = rig.st->command_count('M');

    refuse_slot_threads(*rig.driver);
    CHECK_THROWS_AS(rig.driver->slew_to_coordinates_async(5.5, 20.0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(rig.driver->park(), alpacacore::AlpacaException);

    CHECK(rig.st->command_count('M') == cancels_before);  // nothing was in flight: nothing to stop
    CHECK(rig.st->goto_count.load() == 0);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
}

TEST_CASE("SynScan slot - a refused goto start keeps a MoveAxis in motion reported",
          "[synscan][telescope][async][slot]") {
    refused_start_keeps_move_axis([](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(6.5, 25.0); });
}

TEST_CASE("SynScan slot - a refused park start keeps a MoveAxis in motion reported",
          "[synscan][telescope][async][slot]") {
    refused_start_keeps_move_axis([](alpacacore::TelescopeDriver& d) { d.park(); });
}

#endif  // _WIN32

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

// Motion initiators and a goto body blocked in one exchange, SynScan (decision 0006, "Claim"): a new initiator
// replaces the body in flight and returns without waiting for it, instead of cancelling and joining it.
// The fake holds the first goto command, so the body is inside one exchange when the second initiator is called.

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
    std::atomic<int> gate_ms{0};  // >0: the next goto command is held this long before it is answered
    std::atomic<Clock::rep> gate_entered{0};
    std::atomic<Clock::rep> gate_left{0};
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
            case 'B': {
                const int hold = st->gate_ms.exchange(0);
                if (hold > 0) {
                    st->gate_entered.store(Clock::now().time_since_epoch().count());
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                    st->gate_left.store(Clock::now().time_since_epoch().count());
                }
                st->goto_started.store(st->now().time_since_epoch().count());
                st->goto_stopped.store(false);
                st->goto_count.fetch_add(1);
                return "#";
            }
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
    info.response_timeout_ms = 6000;
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

long ms_since(Clock::time_point t0) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

constexpr int kHoldMs = 2500;

long time_second_initiator(const char* name, const std::function<void(alpacacore::TelescopeDriver&)>& first,
                           const std::function<void(alpacacore::TelescopeDriver&)>& second) {
    Rig rig;
    rig.st->gate_ms.store(kHoldMs);
    first(*rig.driver);
    REQUIRE(wait_until([&] { return rig.st->gate_entered.load() != 0; }, 5000));
    const auto entered = Clock::time_point(Clock::duration(rig.st->gate_entered.load()));
    const long into_hold = ms_since(entered);
    std::atomic<long> took{-1};
    std::atomic<bool> threw{false};
    std::thread t([&] {
        const auto t0 = Clock::now();
        try {
            second(*rig.driver);
        } catch (...) {
            threw = true;
        }
        took = ms_since(t0);
    });
    t.join();
    const bool hold_over = rig.st->gate_left.load() != 0;
    std::printf(
        "initiator %-16s hold=%d ms, called %ld ms into the hold, returned after %ld ms, hold over at return=%d, "
        "threw=%d\n",
        name, kHoldMs, into_hold, took.load(), hold_over ? 1 : 0, threw.load() ? 1 : 0);
    std::fflush(stdout);
    return took.load();
}

}  // namespace

TEST_CASE("SynScan initiator supersede - SlewToCoordinatesAsync during a blocked goto exchange",
          "[synscan][initiator-supersede]") {
    CHECK(time_second_initiator(
              "slew->slew", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
              [](auto& d) { d.slew_to_coordinates_async(6.5, 25.0); }) < kHoldMs / 2);
}

TEST_CASE("SynScan initiator supersede - MoveAxis during a blocked goto exchange", "[synscan][initiator-supersede]") {
    CHECK(time_second_initiator(
              "slew->MoveAxis", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
              [](auto& d) { d.move_axis(0, 0.5); }) < kHoldMs / 2);
}

TEST_CASE("SynScan initiator supersede - AbortSlew during a blocked goto exchange", "[synscan][initiator-supersede]") {
    CHECK(time_second_initiator(
              "slew->AbortSlew", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
              [](auto& d) { d.abort_slew(); }) < kHoldMs / 2);
}

TEST_CASE("SynScan initiator supersede - Unpark during a Park whose goto exchange is blocked",
          "[synscan][initiator-supersede]") {
    CHECK(time_second_initiator("park->Unpark", [](auto& d) { d.park(); }, [](auto& d) { d.unpark(); }) < kHoldMs / 2);
}

TEST_CASE("SynScan initiator supersede - Park during a blocked goto exchange", "[synscan][initiator-supersede]") {
    CHECK(time_second_initiator(
              "slew->Park", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); }, [](auto& d) { d.park(); }) <
          kHoldMs / 2);
}

#endif  // _WIN32

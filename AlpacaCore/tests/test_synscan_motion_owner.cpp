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
// Motion-owner follow-ups (issue #2467): tests that pin the #2263 motion-claim table for this driver.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_synscan_handset.h"
#include "fake_task_clock.h"

namespace {

using alpacacore::test::FakeSynScanHandset;
using alpacacore::test::SynScanGotoLanding;
using alpacacore::test::wait_until;

struct PulseBesideJogRig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeSynScanHandset> st =
        std::make_shared<FakeSynScanHandset>(clock, SynScanGotoLanding::Stays, "12AB0500,20000500#");
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    PulseBesideJogRig() {
        st->model_id.store(50);
        server = std::make_unique<alpacacore::test::FakeMountServer>(st->responder());
        REQUIRE(server->ok());
        alpacacore::vendor::synscan::ConnectionInfo info;
        info.type = alpacacore::vendor::synscan::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server->port();
        info.response_timeout_ms = 1000;
        driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, info, alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
    }
    ~PulseBesideJogRig() { driver->set_connected(false); }

    // Names the next pulse body the driver spawns, so a case can wait until it is parked on the clock.
    std::shared_future<std::thread::id> name_next_pulse_body() {
        auto id = std::make_shared<std::promise<std::thread::id>>();
        auto named = std::make_shared<std::atomic<bool>>(false);
        auto body = id->get_future().share();
        alpacacore::vendor::synscan::set_pulse_spawn_for_testing(*driver, [id, named](std::function<void()> fn) {
            std::thread thread(std::move(fn));
            if (!named->exchange(true)) {
                id->set_value(thread.get_id());
            }
            return thread;
        });
        return body;
    }

    // Commands logged from index `from` on that satisfy `match`.
    int commands_since(std::size_t from, const std::function<bool(const std::string&)>& match) {
        const auto log = st->command_snapshot();
        int count = 0;
        for (std::size_t i = from; i < log.size(); ++i) {
            count += match(log[i]) ? 1 : 0;
        }
        return count;
    }
};

// A `P` motor command to `device` (16 RA, 17 Dec), at any rate.
bool is_motor_command(const std::string& command, int device) {
    return command.size() >= 3 && command[0] == 'P' && static_cast<unsigned char>(command[2]) == device;
}

bool is_zero_rate_command(const std::string& command, int device) {
    return is_motor_command(command, device) && command.size() >= 6 && command[1] == 3 && command[4] == 0 &&
           command[5] == 0;
}

}  // namespace

// #2263 table, #2467 maintainer decision: a PulseGuide runs beside a MoveAxis on the OTHER axis. SynScan refused it
// because slewing_by_driver_state_locked() was true for a jog on either axis.
TEST_CASE("SynScan PulseGuide - accepted during a MoveAxis on the other axis (#2263, #2467)",
          "[synscan][telescope][pulseguiding][moveaxis][motion-owner]") {
    struct Case {
        const char* name;
        int jog_axis;
        int direction;  // 0 N, 1 S (axis 1); 2 E, 3 W (axis 0)
    };
    for (const Case c : {Case{"Dec jog, East pulse", 1, 2}, Case{"Dec jog, West pulse", 1, 3},
                         Case{"RA jog, North pulse", 0, 0}, Case{"RA jog, South pulse", 0, 1}}) {
        INFO(c.name);
        PulseBesideJogRig rig;
        rig.driver->move_axis(c.jog_axis, 0.5);
        REQUIRE(rig.driver->get_slewing());
        const std::size_t after_jog = rig.st->command_snapshot().size();
        const int rate_commands = rig.st->command_count('P');
        const auto body = rig.name_next_pulse_body();
        REQUIRE_NOTHROW(rig.driver->pulse_guide(c.direction, 200));
        CHECK(rig.st->command_count('P') > rate_commands);  // the pulse's rate command reached the handset
        CHECK(rig.driver->get_is_pulse_guiding());
        CHECK(rig.driver->get_slewing());  // the jog still runs

        // The pulse runs to its own end: a Dec pulse stops Dec, an RA pulse hands RA back to tracking (`T`). Neither
        // touches the jogging axis's motor, and an RA jog gets no tracking write, which would end it.
        REQUIRE(body.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        REQUIRE(rig.clock.wait_for_waiter_thread(body.get(), std::chrono::milliseconds(5000)));
        for (int i = 0; i < 40 && rig.driver->get_is_pulse_guiding(); ++i) {
            rig.clock.advance(std::chrono::milliseconds(100));
            rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
        }
        CHECK_FALSE(rig.driver->get_is_pulse_guiding());
        const int pulse_device = c.jog_axis == 0 ? 17 : 16;
        const int jog_device = c.jog_axis == 0 ? 16 : 17;
        CHECK(wait_until(
            [&] {
                return rig.commands_since(after_jog, [&](const std::string& command) {
                    return pulse_device == 17 ? is_zero_rate_command(command, 17) : command[0] == 'T';
                }) > 0;
            },
            5000));
        CHECK(rig.driver->get_slewing());
        CHECK(rig.commands_since(
                  after_jog, [&](const std::string& command) { return is_motor_command(command, jog_device); }) == 0);
        if (jog_device == 16) {
            CHECK(rig.commands_since(after_jog, [](const std::string& command) { return command[0] == 'T'; }) == 0);
        }
    }
}

TEST_CASE("SynScan PulseGuide - still refused during a MoveAxis on its own axis (#2263, #2467)",
          "[synscan][telescope][pulseguiding][moveaxis][motion-owner]") {
    struct Case {
        const char* name;
        int jog_axis;
        int direction;
    };
    for (const Case c : {Case{"RA jog, East pulse", 0, 2}, Case{"RA jog, West pulse", 0, 3},
                         Case{"Dec jog, North pulse", 1, 0}, Case{"Dec jog, South pulse", 1, 1}}) {
        INFO(c.name);
        PulseBesideJogRig rig;
        rig.driver->move_axis(c.jog_axis, 0.5);
        const int rate_commands = rig.st->command_count('P');
        alpacacore::test::require_alpaca_error([&] { rig.driver->pulse_guide(c.direction, 200); },
                                               alpacacore::AlpacaError::InvalidOperation);
        CHECK(rig.st->command_count('P') == rate_commands);
        CHECK_FALSE(rig.driver->get_is_pulse_guiding());
        CHECK(rig.driver->get_slewing());
    }
}

// #2467: pulse_guide() supersedes the older pulse on its axis itself (supersede_pulse_locked()), before it releases its
// wire turn, not through the slot's start(). The hook runs in the gap between the two: the older pulse's wait runs out
// there and its body gets the turn. Superseded, it sends nothing; otherwise its end stops the replacement's rate.
TEST_CASE("SynScan PulseGuide - a replacement pulse supersedes the older one before its timer starts (#2467)",
          "[synscan][telescope][pulseguiding][motion-owner]") {
    struct Case {
        const char* name;
        int direction;
        int device;
    };
    for (const Case c : {Case{"Dec", 0, 17}, Case{"RA", 3, 16}}) {
        INFO(c.name);
        PulseBesideJogRig rig;
        alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*rig.driver, true);
        auto ended = std::make_shared<std::promise<void>>();
        auto started = std::make_shared<std::promise<std::thread::id>>();
        auto first = std::make_shared<std::atomic<bool>>(false);
        alpacacore::vendor::synscan::set_pulse_spawn_for_testing(
            *rig.driver, [ended, started, first](std::function<void()> body) {
                if (first->exchange(true)) {
                    return std::thread(std::move(body));
                }
                std::thread thread([body = std::move(body), ended] {
                    body();
                    ended->set_value();
                });
                started->set_value(thread.get_id());
                return thread;
            });
        const auto old_ended = ended->get_future().share();
        rig.driver->pulse_guide(c.direction, 2000);
        auto old_body = started->get_future();
        REQUIRE(old_body.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        REQUIRE(rig.clock.wait_for_waiter_thread(old_body.get(), std::chrono::milliseconds(5000)));

        // In the gap, step past the older pulse's end and wait (real time) for its body to return.
        std::size_t after_rate = 0;
        bool old_returned = false;
        alpacacore::vendor::synscan::set_pulse_start_hook_for_testing(*rig.driver, [&](int) {
            after_rate = rig.st->command_snapshot().size();
            rig.clock.advance(std::chrono::milliseconds(2500));  // past the old pulse's 2 s, inside the new window
            old_returned = old_ended.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        });
        rig.driver->pulse_guide(c.direction, 2000);
        alpacacore::vendor::synscan::set_pulse_start_hook_for_testing(*rig.driver, nullptr);
        REQUIRE(old_returned);
        CHECK(rig.commands_since(after_rate, [&](const std::string& command) {
            return is_zero_rate_command(command, c.device) || command[0] == 'T';
        }) == 0);
        CHECK(rig.driver->get_is_pulse_guiding());
    }
}

#endif  // !_WIN32

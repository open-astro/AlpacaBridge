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

#pragma once

// A scripted OnStepX controller (the telescope, its focusers and its rotator) on a loopback FakeMountServer, the
// Network transport, for the focuser, rotator and shared-link cases (#2223, #2221): test_onstep_focuser.cpp,
// test_onstep_rotator.cpp and the [stress] registrations in test_onstep_concurrency_stress.cpp. Replies follow OnStepX
// Focuser.command.cpp and Rotator.command.cpp at af1d26bf.

#ifndef _WIN32

#include <alpacacore/focuser_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/sexagesimal.h>
#include <alpacacore/vendor/onstep/onstep_focuser_driver.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_rotator_driver.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_mount_server.h"

namespace alpacacore::test::onstep_controller {

namespace onstep = alpacacore::vendor::onstep;
using std::chrono::milliseconds;

// An OnStepX controller: a German mount at rest, focusers on :F<n>...# and a rotator on :r...#, as OnStepX answers
// them (Focuser.command.cpp, Rotator.command.cpp, af1d26bf). A focuser that is not configured gets the
// unknown-command "0", as the firmware passes an addressed command for an absent axis on and nothing claims it; a
// controller built without the rotator answers :GX98# with `rotator_kind` (N) and every :r command with "0". Every
// command is logged.
struct OnStepXController {
    std::mutex mutex;
    std::vector<std::string> log;            // under mutex
    std::string version = "10.23a";          // :GVN#; under mutex
    std::set<int> focusers{1};               // the configured focusers; under mutex
    long min_steps = 100;                    // :F<n>i#
    long max_steps = 50100;                  // :F<n>m#
    long position = 1100;                    // :F<n>g#, raw steps; under mutex
    long target = 1100;                      // under mutex
    bool moving = false;                     // under mutex
    long step_per_status = 5000;             // how far a move gets at each :F<n>T#
    std::string microns = "0.50000";         // :F<n>u#; under mutex
    std::string temperature = "21.5";        // :F<n>t#; "nan" with no probe; under mutex
    bool tcf = false;                        // :F<n>c#; under mutex
    std::string goto_reply = "1";            // :F<n>s...#; under mutex
    std::string tcf_reply = "1";             // :F<n>c0#/c1#; under mutex
    std::string ge_code = "00#";             // :GE#; under mutex
    std::string set_park_reply = "1";        // :hQ#; under mutex
    std::string unpark_reply = "1";          // :hR#; under mutex
    std::atomic<int> focuser_latency_ms{0};  // added to every focuser reply

    // The rotator (Rotator.command.cpp). Angles are the firmware's, in degrees, inside [rotator_min, rotator_max].
    std::string rotator_kind = "R";         // :GX98#: "R" rotate only, "D" with derotation, "N" none, "0" unknown
    long rotator_min = -180;                // :rI#
    long rotator_max = 180;                 // :rM#
    std::string rotator_step = "0.00500";   // :rD#; under mutex
    double rotator_angle = 30.0;            // :rG#; under mutex
    double rotator_target = 30.0;           // under mutex
    bool rotator_moving = false;            // under mutex
    bool derotating = false;                // :rT# 'D' while stopped; under mutex
    double rotator_step_per_status = 20.0;  // how far a move gets at each :rT#
    std::string rotator_goto_reply = "1";   // :rS...#; under mutex

    // Command prefix -> reply, checked before every other reply; under mutex.
    std::map<std::string, std::string> reply_overrides;

    int count(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(
            std::count_if(log.begin(), log.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }
    std::vector<std::string> commands() {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }

    std::string focuser_reply(int n, const std::string& body) {
        if (!focusers.count(n)) return "0";  // CE_CMD_UNKNOWN, a numeric reply: a bare digit
        if (body == "T") {
            if (moving) {
                const long step = std::min(step_per_status, std::labs(target - position));
                position += target > position ? step : -step;
                moving = position != target;
            }
            return std::string(moving ? "M" : "S") + "3#";
        }
        if (body == "g") return std::to_string(position) + "#";
        if (body == "i") return std::to_string(min_steps) + "#";
        if (body == "m") return std::to_string(max_steps) + "#";
        if (body == "u") return microns + "#";
        if (body == "t") return temperature + "#";
        if (body == "c") return tcf ? "1" : "0";
        if (body == "c0" || body == "c1") {
            if (tcf_reply == "1") tcf = body == "c1";
            return tcf_reply;
        }
        if (body == "Q") {
            moving = false;
            target = position;
            return "";
        }
        if (!body.empty() && body[0] == 's') {
            if (goto_reply != "1") return goto_reply;
            target = std::strtol(body.c_str() + 1, nullptr, 10);
            moving = target != position;
            return "1";
        }
        return "0";
    }

    std::string rotator_reply(const std::string& body) {
        if (rotator_kind != "R" && rotator_kind != "D") return "0";  // CE_CMD_UNKNOWN: no rotator in this build
        if (body == "T") {
            if (rotator_moving) {
                const double step = std::min(rotator_step_per_status, std::fabs(rotator_target - rotator_angle));
                rotator_angle += rotator_target > rotator_angle ? step : -step;
                rotator_moving = std::fabs(rotator_target - rotator_angle) > 1e-9;
            }
            if (rotator_moving) return "M3#";
            return std::string("S") + (derotating ? "D" : "") + "3#";
        }
        if (body == "G") {
            // doubleToDms(fullRange, signPresent, PM_LOW): "+DDD*MM", whole arcminutes.
            return alpacacore::util::format_dm(rotator_angle, 3, '*') + "#";
        }
        if (body == "I") return std::to_string(rotator_min) + "#";
        if (body == "M") return std::to_string(rotator_max) + "#";
        if (body == "D") return rotator_step + "#";
        if (body == "Q") {
            rotator_moving = false;
            rotator_target = rotator_angle;
            return "";
        }
        if (!body.empty() && body[0] == 'S') {
            if (rotator_goto_reply != "1") return rotator_goto_reply;
            const auto degrees = alpacacore::util::parse_sexagesimal(body.substr(1));
            if (!degrees || *degrees < rotator_min || *degrees > rotator_max) return "0";
            rotator_target = *degrees;
            rotator_moving = std::fabs(rotator_target - rotator_angle) > 1e-9;
            return "1";
        }
        return "0";
    }

    std::string reply(const std::string& chunk) {
        bool focuser = false;
        std::string out;
        {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(chunk);
            const auto override_it = std::find_if(reply_overrides.begin(), reply_overrides.end(),
                                                  [&](const auto& o) { return chunk.rfind(o.first, 0) == 0; });
            if (override_it != reply_overrides.end()) {
                out = override_it->second;
            } else if (chunk.size() >= 4 && chunk.rfind(":F", 0) == 0 && chunk[2] >= '1' && chunk[2] <= '9' &&
                       chunk.back() == '#') {
                focuser = true;
                out = focuser_reply(chunk[2] - '0', chunk.substr(3, chunk.size() - 4));
            } else if (chunk.size() >= 4 && chunk.rfind(":r", 0) == 0 && chunk.back() == '#') {
                out = rotator_reply(chunk.substr(2, chunk.size() - 3));
            } else if (chunk == ":GX98#") {
                out = rotator_kind == "0" ? "0" : rotator_kind + "#";
            } else if (chunk.rfind(":GVP", 0) == 0) {
                out = "On-Step#";
            } else if (chunk.rfind(":GVN", 0) == 0) {
                out = version + "#";
            } else if (chunk.rfind(":GE", 0) == 0) {
                out = ge_code;
            } else if (chunk.rfind(":hQ", 0) == 0) {
                out = set_park_reply;
            } else if (chunk.rfind(":hR", 0) == 0) {
                out = unpark_reply;
            } else {
                out = mount_reply(chunk);
            }
        }
        if (focuser && focuser_latency_ms.load() > 0 && !out.empty()) {
            std::this_thread::sleep_for(milliseconds(focuser_latency_ms.load()));
        }
        return out;
    }

    // A German mount at rest: not tracking, no GOTO, not parked.
    static std::string mount_reply(const std::string& c) {
        if (c.rfind(":GXEE", 0) == 0) return "1";
        if (c.rfind(":GU", 0) == 0) return "nNpEo190#";
        if (c.rfind(":GRa", 0) == 0) return "12:00:00.0000#";
        if (c.rfind(":GDe", 0) == 0) return "+45*00:00.000#";
        if (c.rfind(":GA", 0) == 0) return "+45*00:00#";
        if (c.rfind(":GZ", 0) == 0) return "180*00:00#";
        if (c.rfind(":GL", 0) == 0) return "10:20:30#";
        if (c.rfind(":GC", 0) == 0) return "10/28/26#";
        if (c.rfind(":GG", 0) == 0) return "+00:00#";
        if (c.rfind(":Gt", 0) == 0) return "+40*30#";
        if (c.rfind(":Gg", 0) == 0) return "+074*00#";
        if (c.rfind(":Q", 0) == 0 || c.rfind(":M", 0) == 0 || c.rfind(":R", 0) == 0 || c.rfind(":T", 0) == 0) {
            return "";
        }
        if (c.rfind(":S", 0) == 0 || c.rfind(":h", 0) == 0) return "1";
        return "0#";
    }
};

inline onstep::ConnectionInfo endpoint(int port, int response_timeout_ms = 300) {
    onstep::ConnectionInfo info;
    info.type = onstep::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = response_timeout_ms;
    return info;
}

// The controller and its server; the devices a case builds on it are declared after it, so they go first.
struct Controller {
    std::shared_ptr<OnStepXController> state = std::make_shared<OnStepXController>();
    alpacacore::test::FakeMountServer server{[s = state](const std::string& chunk) { return s->reply(chunk); }};

    Controller() { REQUIRE(server.ok()); }
    onstep::ConnectionInfo info() const { return endpoint(server.port()); }
    std::unique_ptr<alpacacore::FocuserDriver> focuser(int n = 1) {
        return onstep::create_onstep_focuser(0, info(), n);
    }
    std::unique_ptr<alpacacore::TelescopeDriver> telescope() { return onstep::create_onstep_telescope(0, info()); }
    std::unique_ptr<alpacacore::RotatorDriver> rotator() { return onstep::create_onstep_rotator(0, info()); }
};

// Runs `action` `tries` times on this thread. Before each exchange it makes, the BeforeExchange hook lets another
// thread ask the shared link for the product name (:GVP#) and waits up to 50 ms for it. A refused command and its
// :GE# then stay together only if the command holds the link's Sequence: without it, the :GVP# lands between the
// two every time. Returns the command log for the caller to check the pairs.
template <typename Action>
std::vector<std::string> run_with_a_sibling_at_every_exchange(Controller& controller, int tries, Action&& action) {
    const auto link = onstep::detail::registered_link_for_testing(controller.info());
    REQUIRE(link);
    std::mutex m;
    std::condition_variable cv;
    bool go = false;
    bool done = false;
    bool stop = false;
    std::atomic<std::thread::id> mover{std::this_thread::get_id()};
    std::thread sibling([&] {
        std::unique_lock<std::mutex> lock(m);
        for (;;) {
            cv.wait(lock, [&] { return go || stop; });
            if (stop) return;
            go = false;
            lock.unlock();
            (void)link->get_product_name();  // waits for the wire, and for a Sequence another device holds
            lock.lock();
            done = true;
            cv.notify_all();
        }
    });
    onstep::detail::set_link_loss_hook([&](onstep::detail::LinkLossHookPoint point) {
        if (point != onstep::detail::LinkLossHookPoint::BeforeExchange || std::this_thread::get_id() != mover.load()) {
            return;
        }
        std::unique_lock<std::mutex> lock(m);
        done = false;
        go = true;
        cv.notify_all();
        cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; });  // done only when nothing held it
    });
    for (int i = 0; i < tries; ++i) {
        action();
    }
    onstep::detail::set_link_loss_hook({});
    mover.store(std::thread::id());
    {
        std::lock_guard<std::mutex> lock(m);
        stop = true;
    }
    cv.notify_all();
    sibling.join();
    return controller.state->commands();
}

// Checks that every :GE# in `log` follows a command starting with `prefix`, and counts them.
inline int ge_pairs_after(const std::vector<std::string>& log, const std::string& prefix) {
    int pairs = 0;
    for (std::size_t i = 1; i < log.size(); ++i) {
        if (log[i] == ":GE#") {
            ++pairs;
            CAPTURE(i);
            CHECK(log[i - 1].rfind(prefix, 0) == 0);  // never another exchange between the command and its :GE#
        }
    }
    return pairs;
}

}  // namespace alpacacore::test::onstep_controller

#endif  // !_WIN32

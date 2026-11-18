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

// A scripted OnStepX controller (the telescope and its focusers) on a loopback FakeMountServer, the Network
// transport, for the focuser and shared-link cases (#2223): test_onstep_focuser.cpp and the [stress] registration in
// test_onstep_concurrency_stress.cpp. Replies follow OnStepX Focuser.command.cpp at af1d26bf.

#ifndef _WIN32

#include <alpacacore/focuser_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/onstep/onstep_focuser_driver.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

// An OnStepX controller: a German mount at rest and focusers on :F<n>...#, as OnStepX answers them
// (Focuser.command.cpp, af1d26bf). A focuser that is not configured gets the unknown-command "0", as the firmware
// passes an addressed command for an absent axis on and nothing claims it. Every command is logged.
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
};

}  // namespace alpacacore::test::onstep_controller

#endif  // !_WIN32

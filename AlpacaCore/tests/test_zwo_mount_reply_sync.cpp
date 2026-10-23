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

// Reply synchronization of the ZWO mount wrapper (issue #1527): a reply that
// arrives after its command timed out, and after the next command flushed its
// input, must not be read as that next command's reply. The fake mount is
// in-order like the real one: it handles one command at a time, so a slow
// command's late reply is written before the next command is looked at. Each
// case runs over a pty (serial) and over loopback TCP.
#ifndef _WIN32

#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_mount_server.h"
#include "pty_responder.h"

namespace {

using alpacacore::vendor::zwo::ConnectionInfo;
using alpacacore::vendor::zwo::ConnectionType;
using alpacacore::vendor::zwo::ZWOMountProtocolWrapper;
using Clock = std::chrono::steady_clock;
using Responder = std::function<std::string(const std::string&)>;

constexpr int kQueryTimeoutMs = 250;
constexpr auto kLateBy = std::chrono::milliseconds(700);

// A mount that answers :GD and :GR late (kLateBy after it sees the command),
// and everything else at once. mute_late=true never answers them at all.
Responder slow_position_mount(bool mute_late = false) {
    return [mute_late](const std::string& chunk) {
        std::string out;
        std::size_t pos = 0;
        while (pos < chunk.size()) {
            const std::size_t end = chunk.find('#', pos);
            if (end == std::string::npos) break;
            const std::string cmd = chunk.substr(pos, end - pos);
            pos = end + 1;
            if (cmd == ":GD" || cmd == ":GR") {
                if (mute_late) continue;
                std::this_thread::sleep_for(kLateBy);
                out += cmd == ":GD" ? "+45*30:15#" : "02:30:15#";
            } else if (cmd == ":GU") {
                out += "GN#";
            } else if (cmd == ":GAT") {
                out += "1#";
            }  // blind commands (:Mg.., :Q) get no reply
        }
        return out;
    };
}

// Runs `body` against a connected wrapper whose mount is `responder`, once over a pty and once over TCP.
void on_both_transports(const Responder& responder, const std::function<void(ZWOMountProtocolWrapper&)>& body) {
    SECTION("serial") {
        alpacacore::test::PtyResponder device(responder, "ZWO mount");
        ZWOMountProtocolWrapper wrapper;
        ConnectionInfo info;
        info.type = ConnectionType::Serial;
        info.port_path = device.slave_path();
        REQUIRE(wrapper.connect(info));
        body(wrapper);
        wrapper.disconnect();
    }
    SECTION("tcp") {
        alpacacore::test::FakeMountServer server(responder);
        REQUIRE(server.ok());
        ZWOMountProtocolWrapper wrapper;
        ConnectionInfo info;
        info.type = ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        REQUIRE(wrapper.connect(info));
        body(wrapper);
        wrapper.disconnect();
    }
}

long ms_since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

}  // namespace

TEST_CASE("ZWO mount reply sync - a late Dec reply is not read as the next status", "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(), [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        CHECK(wrapper.send_command(":GU", false) == "GN");
    });
}

TEST_CASE("ZWO mount reply sync - a late RA reply starting with 0 does not read as tracking off",
          "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(), [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GR", true, kQueryTimeoutMs));
        CHECK(wrapper.get_tracking_enabled());
    });
}

TEST_CASE("ZWO mount reply sync - a late reply after a blind guide pulse is not read by the next query",
          "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(), [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        wrapper.pulse_guide(2, 100);
        CHECK(wrapper.send_command(":GU", false) == "GN");
    });
}

TEST_CASE("ZWO mount reply sync - a reply that never comes delays the next query by the fence bound only",
          "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(true), [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        const auto start = Clock::now();
        CHECK(wrapper.send_command(":GU", false) == "GN");
        CHECK(ms_since(start) < 800);
        // The fence is spent: the query after it waits for nothing.
        const auto second = Clock::now();
        CHECK(wrapper.send_command(":GU", false) == "GN");
        CHECK(ms_since(second) < 150);
    });
}

TEST_CASE("ZWO mount reply sync - a query with nothing owed is not delayed", "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(), [](ZWOMountProtocolWrapper& wrapper) {
        const auto start = Clock::now();
        CHECK(wrapper.send_command(":GU", false) == "GN");
        CHECK(ms_since(start) < 150);
    });
}

TEST_CASE("ZWO mount reply sync - the abort is not held by the fence", "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(true), [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        const auto start = Clock::now();
        wrapper.abort_motion();
        CHECK(ms_since(start) < 350);
    });
}

#endif  // _WIN32

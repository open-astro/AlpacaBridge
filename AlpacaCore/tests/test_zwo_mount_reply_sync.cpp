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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>

#include <atomic>
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
            } else if (cmd.rfind(":Sr", 0) == 0) {
                out += "1";
            }  // blind commands (:Mg.., :Q) get no reply
        }
        return out;
    };
}

using Push = std::function<void(const std::string&)>;

// Runs `body` against a connected wrapper whose mount is `responder`, once over a pty and once over TCP.
void on_both_transports(const Responder& responder,
                        const std::function<void(ZWOMountProtocolWrapper&, const Push&)>& body) {
    SECTION("serial") {
        alpacacore::test::PtyResponder device(responder, "ZWO mount");
        ZWOMountProtocolWrapper wrapper;
        ConnectionInfo info;
        info.type = ConnectionType::Serial;
        info.port_path = device.slave_path();
        REQUIRE(wrapper.connect(info));
        body(wrapper, [&device](const std::string& bytes) { device.push(bytes); });
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
        body(wrapper, [&server](const std::string& bytes) { server.push(bytes); });
        wrapper.disconnect();
    }
}

void on_both_transports(const Responder& responder, const std::function<void(ZWOMountProtocolWrapper&)>& body) {
    on_both_transports(responder, std::function<void(ZWOMountProtocolWrapper&, const Push&)>(
                                      [&body](ZWOMountProtocolWrapper& wrapper, const Push&) { body(wrapper); }));
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

TEST_CASE("ZWO mount reply sync - an ordinary write after a timed-out query reads its own ack",
          "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(), [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        CHECK_NOTHROW(wrapper.set_target_ra(2.5));
    });
}

TEST_CASE("ZWO mount reply sync - a disconnect during the fence ends it and writes nothing",
          "[zwo][reply-sync][issue1527]") {
    std::atomic<bool> saw_slew{false};
    const Responder inner = slow_position_mount(true);
    const Responder responder = [&saw_slew, inner](const std::string& chunk) {
        if (chunk.find(":MS#") != std::string::npos) saw_slew = true;
        return inner(chunk);
    };
    on_both_transports(responder, [&saw_slew](ZWOMountProtocolWrapper& wrapper) {
        saw_slew = false;
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        std::atomic<bool> not_connected{false};
        std::atomic<long> returned_after_cancel_ms{-1};
        std::atomic<Clock::rep> cancel_at{0};
        std::thread sender([&] {
            try {
                (void)wrapper.send_command(":MS", true, 2000);
            } catch (const alpacacore::AlpacaException& e) {
                not_connected = e.error_code() == alpacacore::AlpacaError::NotConnected;
            }
            returned_after_cancel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           Clock::now().time_since_epoch() - Clock::duration(cancel_at.load()))
                                           .count();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel_at = Clock::now().time_since_epoch().count();
        wrapper.cancel_pending_io();
        sender.join();
        CHECK(not_connected);
        CHECK(returned_after_cancel_ms.load() < 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK_FALSE(saw_slew);
    });
}

TEST_CASE("ZWO mount reply sync - a silent mount does not slow queued guide pulses", "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(true), [](ZWOMountProtocolWrapper& wrapper) {
        // The pulse worker's shape: a blind pulse, then a best-effort 250 ms position read that times out.
        const auto start = Clock::now();
        for (int i = 0; i < 5; ++i) {
            wrapper.pulse_guide(2, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK_THROWS(wrapper.get_current_equatorial(kQueryTimeoutMs));
        }
        // Without the fence this loop takes 2.3 s (pty) and 3.4 s (tcp); the fence adds only a
        // margin because it spends the read's own timeout (a fence on the full bound took 4.1 s on the pty).
        CHECK(ms_since(start) < 3800);
    });
}

TEST_CASE("ZWO mount reply sync - a line that keeps sending after the reply ends the fence at its cap",
          "[zwo][reply-sync][issue1527]") {
    on_both_transports(slow_position_mount(true), [](ZWOMountProtocolWrapper& wrapper, const Push& push) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        // The reply ends with '#', then a byte every 10 ms (under the 30 ms quiet window) for 2 s.
        std::atomic<bool> stop{false};
        std::thread talker([&] {
            push("+45*30:15#");
            for (int i = 0; i < 200 && !stop; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                push("x");
            }
        });
        const auto start = Clock::now();
        // The cap is this exchange's own timeout (300 ms); without the cap after '#' the fence holds until 2 s.
        try {
            (void)wrapper.send_command(":GU", false, 300);
        } catch (const std::exception&) {
            // The noise may leave this exchange without a clean reply; only the time it took is asserted.
        }
        const long took = ms_since(start);
        stop = true;
        talker.join();
        CHECK(took < 900);
    });
}

TEST_CASE("ZWO mount reply sync - a blind pulse that eats the owed reply leaves nothing owed",
          "[zwo][reply-sync][issue1527]") {
    // :GD answers 20 ms after its 250 ms timeout; every other command answers at once.
    const Responder mount = [](const std::string& chunk) {
        std::string out;
        std::size_t pos = 0;
        while (pos < chunk.size()) {
            const std::size_t end = chunk.find('#', pos);
            if (end == std::string::npos) break;
            const std::string cmd = chunk.substr(pos, end - pos);
            pos = end + 1;
            if (cmd == ":GD") {
                std::this_thread::sleep_for(std::chrono::milliseconds(kQueryTimeoutMs + 20));
                out += "+45*30:15#";
            } else if (cmd == ":GR") {
                out += "02:30:15#";
            }
        }
        return out;
    };
    on_both_transports(mount, [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GD", true, kQueryTimeoutMs));
        // The reply is already queued when the pulse flushes, so the flush peek eats it; the next query must
        // not wait for it.
        wrapper.pulse_guide(2, 10);
        const auto start = Clock::now();
        std::string ra;
        CHECK_NOTHROW(ra = wrapper.send_command(":GR", true, kQueryTimeoutMs));
        CHECK(ra == "02:30:15");
        CHECK(ms_since(start) < 150);
    });
}

TEST_CASE("ZWO mount reply sync - a reply that arrives during the blind drain leaves nothing owed",
          "[zwo][reply-sync][issue1527]") {
    // :GU never answers on its own; the owed reply is emitted only once the pulse command (:Mg..) is seen,
    // so it arrives after the pulse's flush, inside its 40 ms drain. It is short (3 bytes) so the drain
    // reads all of it within its window.
    const Responder mount = [](const std::string& chunk) {
        std::string out;
        std::size_t pos = 0;
        while (pos < chunk.size()) {
            const std::size_t end = chunk.find('#', pos);
            if (end == std::string::npos) break;
            const std::string cmd = chunk.substr(pos, end - pos);
            pos = end + 1;
            if (cmd.rfind(":Mg", 0) == 0) {
                out += "GN#";
            } else if (cmd == ":GR") {
                out += "02:30:15#";
            }
        }
        return out;
    };
    on_both_transports(mount, [](ZWOMountProtocolWrapper& wrapper) {
        CHECK_THROWS(wrapper.send_command(":GU", true, kQueryTimeoutMs));
        wrapper.pulse_guide(2, 10);
        const auto start = Clock::now();
        std::string ra;
        CHECK_NOTHROW(ra = wrapper.send_command(":GR", true, kQueryTimeoutMs));
        CHECK(ra == "02:30:15");
        CHECK(ms_since(start) < 150);
    });
}

#endif  // _WIN32

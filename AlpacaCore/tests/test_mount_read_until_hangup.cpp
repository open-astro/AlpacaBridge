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

// The connected-path reply read of the iOptron, Celestron NexStar, SynScan,
// ZWO and OnStep mount wrappers goes through util::read_until (issue #1644 and
// the OnStep slice after it, both of #1604). Two properties, each per wrapper over a pty:
// - a line that goes away while a command waits for its reply reads as a
//   hangup at once, not as a timeout after the full reply window: the "mount"
//   takes the command and then severs the pty, so the write succeeds and the
//   read hears EIO;
// - a read that does not require the '#' still ends at the '#': the "mount"
//   answers "1#" and then keeps the line busy, so a read that ran past the '#'
//   would wait for the whole reply window.
// And per wrapper over TCP, the byte in flight when the timeout runs out
// settles the reply, as it did when the byte-at-a-time loop checked the timeout
// after each 200 ms recv() and never after a '#': a late '#' still ends its own
// reply (left in the line it would answer the next command), and a whole reply
// that starts after the timeout is still a timeout. A binary payload is the
// exception: the loop read on after each payload byte without checking the
// timeout, so a payload that starts after the timeout is still read whole.
// The iOptron TCP flush before a command keeps the old loop's window as well:
// up to 16 stale bytes, each waited for one 200 ms read slice; and so does the
// serial blind-command ack drain: once an ack byte came it reads on until
// 100 ms of silence, past its 300 ms wait.
// OnStep keeps its own hangup handling: a hangup on the tty loses the link
// (NotConnected, "read failed: <errno>", decision 0009), and over TCP, a test
// seam that is never lost, a peer that hangs up reads as a hangup at once
// instead of a timeout. Its drain after a blind command still waits at least
// one read slice for a stray ack, as the old loop's first read did.
#ifndef _WIN32

#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "catch2_compat.h"
#include "fake_mount_server.h"
#include "fake_pty_write.h"
#include "pty_responder.h"

#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#endif
#ifdef ALPACACORE_ENABLE_ZWO
#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// A pty "mount" that echoes the NexStar / SynScan "K" check until armed. Armed
// with sever_on_next_command(), it severs the pty when the next bytes arrive:
// the command has been written, the reply never comes and the line is gone.
// Armed with trickle_after_reply_on_next_command(), it answers the next
// command with "1#" and then sends a '0' every 20 ms until it stops, so the
// line never goes idle after the '#'. Armed with slow_acks_on_next_command(), it
// answers the next command with eight '1' ack bytes, 40 ms apart from 200 ms
// on, so they straddle a 300 ms drain, and the command after it with "1#".
class HangupMount {
public:
    HangupMount() : pty_("HangupMount") {
        worker_ = std::thread([this] { run(); });
    }
    ~HangupMount() {
        stop_.store(true);
        worker_.join();
    }
    HangupMount(const HangupMount&) = delete;
    HangupMount& operator=(const HangupMount&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }
    void sever_on_next_command() { armed_.store(true); }
    void trickle_after_reply_on_next_command() { trickle_.store(true); }
    void slow_acks_on_next_command() { slow_acks_.store(true); }

private:
    void run() {
        while (!stop_.load()) {
            pollfd pfd = {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 20) <= 0) continue;
            char buf[64];
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            if (armed_.load()) {
                pty_.sever();
                return;
            }
            if (trickle_.load()) {
                alpacacore::test::pty_write_bounded(pty_.master_fd(), std::string("1#"), stop_);
                while (!stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    alpacacore::test::pty_write_bounded(pty_.master_fd(), std::string("0"), stop_);
                }
                return;
            }
            if (slow_acks_.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                for (int i = 0; i < 8 && !stop_.load(); ++i) {
                    if (i > 0) std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    alpacacore::test::pty_write_bounded(pty_.master_fd(), std::string("1"), stop_);
                }
                answer_next_.store(true);
                continue;
            }
            if (answer_next_.exchange(false)) {
                alpacacore::test::pty_write_bounded(pty_.master_fd(), std::string("1#"), stop_);
                continue;
            }
            const std::string reply =
                alpacacore::test::answer_handset_echo(std::string(buf, static_cast<std::size_t>(n)));
            if (!reply.empty()) alpacacore::test::pty_write_bounded(pty_.master_fd(), reply, stop_);
        }
    }

    alpacacore::test::PtyPair pty_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> armed_{false};
    std::atomic<bool> trickle_{false};
    std::atomic<bool> slow_acks_{false};
    std::atomic<bool> answer_next_{false};
    std::thread worker_;
};

// The message a command threw, and how long it took to throw it.
template <typename Command>
std::string hangup_message(Command&& command, Clock::duration& took) {
    const auto start = Clock::now();
    std::string message;
    try {
        command();
    } catch (const alpacacore::AlpacaException& e) {
        message = e.what();
    }
    took = Clock::now() - start;
    return message;
}

}  // namespace

#ifdef ALPACACORE_ENABLE_IOPTRON
// Falsified by: ioptron_protocol_wrapper.cpp read_response() treating a tty_read() error as "no byte yet" (the old loop
// timed out after 3 s with "Timeout waiting for mount response")
TEST_CASE("iOptron mount - a hangup while a command waits for its reply reads as a hangup",
          "[ioptron][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.sever_on_next_command();
    Clock::duration took{};
    const auto message = hangup_message([&] { (void)wrapper.send_command(":GLS", true, 3000); }, took);
    wrapper.disconnect();
    INFO("message: " << message);
    CHECK(message.find("hangup") != std::string::npos);
    CHECK(took < std::chrono::seconds(2));
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
// Falsified by: celestron_protocol_wrapper.cpp read_response() treating a tty_read() error as "no byte yet" (the old
// loop timed out after 3 s with "Timeout waiting for Celestron response")
TEST_CASE("Celestron mount - a hangup while a command waits for its reply reads as a hangup",
          "[celestron][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::celestron::CelestronProtocolWrapper::instance();
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.sever_on_next_command();
    Clock::duration took{};
    const auto message = hangup_message([&] { (void)wrapper.send_command("V", true, 3000); }, took);
    wrapper.disconnect();
    INFO("message: " << message);
    CHECK(message.find("hangup") != std::string::npos);
    CHECK(took < std::chrono::seconds(2));
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
// Falsified by: synscan_protocol_wrapper.cpp read_response() treating a tty_read() error as "no byte yet" (the old loop
// timed out after 3 s with "Timeout waiting for SynScan response")
TEST_CASE("SynScan mount - a hangup while a command waits for its reply reads as a hangup",
          "[synscan][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::synscan::SynScanProtocolWrapper::instance();
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.sever_on_next_command();
    Clock::duration took{};
    const auto message = hangup_message([&] { (void)wrapper.send_command("V", true, 3000); }, took);
    wrapper.disconnect();
    INFO("message: " << message);
    CHECK(message.find("hangup") != std::string::npos);
    CHECK(took < std::chrono::seconds(2));
}
#endif

#ifdef ALPACACORE_ENABLE_ZWO
// Falsified by: zwo_mount_protocol_wrapper.cpp read_response() dropping the LinkDead -> note_transport_error() feed
// (link_lost() stays false)
TEST_CASE("ZWO mount - a hangup while a command waits for its reply latches the link lost",
          "[zwo][serial][serial_read][issue1644]") {
    HangupMount mount;
    alpacacore::vendor::zwo::ZWOMountProtocolWrapper wrapper;
    alpacacore::vendor::zwo::ConnectionInfo info;
    info.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.sever_on_next_command();
    Clock::duration took{};
    (void)hangup_message([&] { (void)wrapper.send_command(":GVP", true, 3000); }, took);
    CHECK(wrapper.link_lost());
    CHECK(took < std::chrono::seconds(2));
    wrapper.disconnect();
}
#endif

// The reply of a read that does not require the '#', and how long it took.
template <typename Command>
std::string timed_reply(Command&& command, Clock::duration& took) {
    const auto start = Clock::now();
    std::string reply = command();
    took = Clock::now() - start;
    return reply;
}

#ifdef ALPACACORE_ENABLE_IOPTRON
// Falsified by: ioptron_protocol_wrapper.cpp read_response() using ReadSpec::Stop::IdleGap for the no-'#' read (the
// busy line keeps it reading past the '#' until the 3 s deadline)
TEST_CASE("iOptron mount - a read that does not require the '#' ends at the '#'",
          "[ioptron][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.trickle_after_reply_on_next_command();
    Clock::duration took{};
    const auto reply = timed_reply([&] { return wrapper.send_command(":GUT", false, 3000); }, took);
    wrapper.disconnect();
    CHECK(reply == "1");
    CHECK(took < std::chrono::seconds(1));
}

// Falsified by: ioptron_protocol_wrapper.cpp drain_serial_stale() ending at its 300 ms deadline while ack bytes still
// come (the acks after 300 ms answer "Next")
TEST_CASE("iOptron mount - a blind-command ack drain reads on while acks keep coming",
          "[ioptron][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.slow_acks_on_next_command();
    wrapper.send_command_blind(":Q#");
    std::string next;
    try {
        next = wrapper.send_command("Next", true, 1000);
    } catch (const alpacacore::AlpacaException& e) {
        next = std::string("threw: ") + e.what();
    }
    wrapper.disconnect();
    CHECK(next == "1");
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
// Falsified by: celestron_protocol_wrapper.cpp read_response() using ReadSpec::Stop::IdleGap for the no-'#' read (the
// busy line keeps it reading past the '#' until the 3 s deadline)
TEST_CASE("Celestron mount - a read that does not require the '#' ends at the '#'",
          "[celestron][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::celestron::CelestronProtocolWrapper::instance();
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.trickle_after_reply_on_next_command();
    Clock::duration took{};
    const auto reply = timed_reply([&] { return wrapper.send_command("e", false, 3000); }, took);
    wrapper.disconnect();
    CHECK(reply == "1");
    CHECK(took < std::chrono::seconds(1));
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
// Falsified by: synscan_protocol_wrapper.cpp read_response() using ReadSpec::Stop::IdleGap for the no-'#' read (the
// busy line keeps it reading past the '#' until the 3 s deadline)
TEST_CASE("SynScan mount - a read that does not require the '#' ends at the '#'",
          "[synscan][serial][serial_read][issue1644]") {
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::synscan::SynScanProtocolWrapper::instance();
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.trickle_after_reply_on_next_command();
    Clock::duration took{};
    const auto reply = timed_reply([&] { return wrapper.send_command("e", false, 3000); }, took);
    wrapper.disconnect();
    CHECK(reply == "1");
    CHECK(took < std::chrono::seconds(1));
}
#endif

#ifdef ALPACACORE_ENABLE_ZWO
// Falsified by: zwo_mount_protocol_wrapper.cpp read_response() using ReadSpec::Stop::IdleGap for the no-'#' read (each
// slice reads past the '#' and the read runs to the 3 s deadline)
TEST_CASE("ZWO mount - a read that does not require the '#' ends at the '#'", "[zwo][serial][serial_read][issue1644]") {
    HangupMount mount;
    alpacacore::vendor::zwo::ZWOMountProtocolWrapper wrapper;
    alpacacore::vendor::zwo::ConnectionInfo info;
    info.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    info.port_path = mount.slave_path();
    info.baud_rate = 9600;
    REQUIRE(wrapper.connect(info));
    mount.trickle_after_reply_on_next_command();
    Clock::duration took{};
    const auto reply = timed_reply([&] { return wrapper.send_command(":GVP", false, 3000); }, took);
    wrapper.disconnect();
    CHECK(reply == "1");
    CHECK(took < std::chrono::seconds(1));
}
#endif

// A TCP "mount" that answers "Late" with "#" and "Slow" with "1#", both kLateReply after the command arrives: past the
// kLateTimeoutMs the case gives the read but inside the 200 ms read slice that follows it. It answers the binary
// queries the same way, in one write: NexStar "V" with firmware 5.16 (0x05 0x10 '#'), SynScan "h" with 12:34:56 on
// 10/25/26 (eight bytes, one of them a 0x0A, then '#'). It answers "Next" with "1#" at once. ZWO sends ":Late#"; the
// other wrappers send the command as given.
constexpr int kLateTimeoutMs = 300;
constexpr auto kLateReply = std::chrono::milliseconds(350);

std::string late_hash_reply(const std::string& chunk) {
    const std::string cmd = chunk.rfind(':', 0) == 0 ? chunk.substr(1) : chunk;
    if (cmd.rfind("Late", 0) == 0 || cmd.rfind("Slow", 0) == 0) {
        std::this_thread::sleep_for(kLateReply);
        return cmd.rfind("Late", 0) == 0 ? "#" : "1#";
    }
    if (cmd == "V" || cmd == "h") {
        std::this_thread::sleep_for(kLateReply);
        return cmd == "V" ? std::string("\x05\x10#") : std::string("\x0c\x22\x38\x0a\x19\x1a\x00\x00#", 9);
    }
    if (cmd.rfind("Next", 0) == 0) return "1#";
    return alpacacore::test::answer_handset_echo(chunk);
}

// Sends `first` with the short timeout, then "Next": the error the first threw (empty when it returned) and the
// reply to the second.
template <typename Send>
std::pair<std::string, std::string> late_then_next(Send&& send, const std::string& first = "Late") {
    std::string error;
    try {
        (void)send(first, kLateTimeoutMs);
    } catch (const alpacacore::AlpacaException& e) {
        error = e.what();
    }
    std::string next;
    try {
        next = send("Next", 1000);
    } catch (const alpacacore::AlpacaException& e) {
        next = std::string("threw: ") + e.what();
    }
    return {error, next};
}

#ifdef ALPACACORE_ENABLE_IOPTRON
// Falsified by: ioptron_protocol_wrapper.cpp read_response() without spec.late_byte_grace (the read times out at
// 300 ms and the late '#' answers "Next")
TEST_CASE("iOptron mount - a '#' that arrives within one read slice after the timeout ends its own reply",
          "[ioptron][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto [error, next] =
        late_then_next([&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); });
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(next == "1");
}

// Falsified by: ioptron_protocol_wrapper.cpp read_response() reading to the timeout plus one read slice (the late "1#"
// is read as the reply)
TEST_CASE("iOptron mount - a reply that starts after the timeout is still a timeout",
          "[ioptron][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto result = late_then_next(
        [&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); }, "Slow");
    wrapper.disconnect();
    CHECK(result.first.find("Timeout") != std::string::npos);
}

// Falsified by: ioptron_protocol_wrapper.cpp flush_input() ending the TCP drain at the first 1 ms gap after a byte (the
// '#' of a stale "1#" that comes 20 ms after its '1' stays and answers "Next" with an empty reply)
TEST_CASE("iOptron mount - a TCP flush waits one read slice for each stale byte",
          "[ioptron][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    // A late "1#" in two segments, 20 ms apart: the '1' is waiting when the flush starts, the '#' comes while it runs.
    server.push("1");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::thread hash([&server] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        server.push("#");
    });
    wrapper.flush_input();
    hash.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // a '#' the flush left is in the socket by now
    std::string next;
    try {
        next = wrapper.send_command("Next", true, 1000);
    } catch (const alpacacore::AlpacaException& e) {
        next = std::string("threw: ") + e.what();
    }
    wrapper.disconnect();
    CHECK(next == "1");
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
// Falsified by: celestron_protocol_wrapper.cpp read_response() without spec.late_byte_grace (the read times out at
// 300 ms and the late '#' answers "Next")
TEST_CASE("Celestron mount - a '#' that arrives within one read slice after the timeout ends its own reply",
          "[celestron][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::celestron::CelestronProtocolWrapper::instance();
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto [error, next] =
        late_then_next([&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); });
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(next == "1");
}

// Falsified by: celestron_protocol_wrapper.cpp read_response() reading to the timeout plus one read slice (the late
// "1#" is read as the reply)
TEST_CASE("Celestron mount - a reply that starts after the timeout is still a timeout",
          "[celestron][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::celestron::CelestronProtocolWrapper::instance();
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto result = late_then_next(
        [&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); }, "Slow");
    wrapper.disconnect();
    CHECK(result.first.find("Timeout") != std::string::npos);
}
// Falsified by: celestron_protocol_wrapper.cpp read_response() without late_byte_grace on the binary read (the payload
// is left in the line, the read times out and "Next" reads it)
TEST_CASE("Celestron mount - a binary reply that starts after the timeout is read whole",
          "[celestron][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::celestron::CelestronProtocolWrapper::instance();
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = kLateTimeoutMs;
    REQUIRE(wrapper.connect(info));
    std::string version;
    std::string error;
    try {
        version = wrapper.get_handset_firmware_version();
    } catch (const alpacacore::AlpacaException& e) {
        error = e.what();
    }
    std::string next;
    try {
        next = wrapper.send_command("Next", true, 1000);
    } catch (const alpacacore::AlpacaException& e) {
        next = std::string("threw: ") + e.what();
    }
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(version == "5.16");
    CHECK(next == "1");
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
// Falsified by: synscan_protocol_wrapper.cpp read_response() without spec.late_byte_grace (the read times out at
// 300 ms and the late '#' answers "Next")
TEST_CASE("SynScan mount - a '#' that arrives within one read slice after the timeout ends its own reply",
          "[synscan][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::synscan::SynScanProtocolWrapper::instance();
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto [error, next] =
        late_then_next([&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); });
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(next == "1");
}

// Falsified by: synscan_protocol_wrapper.cpp read_response() reading to the timeout plus one read slice (the late "1#"
// is read as the reply)
TEST_CASE("SynScan mount - a reply that starts after the timeout is still a timeout",
          "[synscan][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::synscan::SynScanProtocolWrapper::instance();
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto result = late_then_next(
        [&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); }, "Slow");
    wrapper.disconnect();
    CHECK(result.first.find("Timeout") != std::string::npos);
}
// Falsified by: synscan_protocol_wrapper.cpp read_response() without late_byte_grace on the binary read (the payload
// is left in the line, the read times out and "Next" reads it)
TEST_CASE("SynScan mount - a binary reply that starts after the timeout is read whole",
          "[synscan][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::synscan::SynScanProtocolWrapper::instance();
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = kLateTimeoutMs;
    REQUIRE(wrapper.connect(info));
    alpacacore::vendor::synscan::TimeInfo time;
    std::string error;
    try {
        time = wrapper.get_time();
    } catch (const alpacacore::AlpacaException& e) {
        error = e.what();
    }
    std::string next;
    try {
        next = wrapper.send_command("Next", true, 1000);
    } catch (const alpacacore::AlpacaException& e) {
        next = std::string("threw: ") + e.what();
    }
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(time.hour == 12);
    CHECK(time.minute == 34);
    CHECK(time.second == 56);
    CHECK(time.month == 10);
    CHECK(time.day == 25);
    CHECK(time.year == 26);
    CHECK(next == "1");
}
#endif

#ifdef ALPACACORE_ENABLE_ZWO
// Falsified by: zwo_mount_protocol_wrapper.cpp read_response() without the late byte after the timeout (the read times
// out at 300 ms and the late '#' answers "Next")
TEST_CASE("ZWO mount - a '#' that arrives within one read slice after the timeout ends its own reply",
          "[zwo][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    alpacacore::vendor::zwo::ZWOMountProtocolWrapper wrapper;
    alpacacore::vendor::zwo::ConnectionInfo info;
    info.type = alpacacore::vendor::zwo::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto [error, next] =
        late_then_next([&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); });
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(next == "1");
}

// Falsified by: zwo_mount_protocol_wrapper.cpp read_response() reading to the timeout plus one read slice (the late
// "1#" is read as the reply)
TEST_CASE("ZWO mount - a reply that starts after the timeout is still a timeout",
          "[zwo][network][serial_read][issue1644]") {
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    alpacacore::vendor::zwo::ZWOMountProtocolWrapper wrapper;
    alpacacore::vendor::zwo::ConnectionInfo info;
    info.type = alpacacore::vendor::zwo::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    REQUIRE(wrapper.connect(info));
    const auto result = late_then_next(
        [&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); }, "Slow");
    wrapper.disconnect();
    CHECK(result.first.find("Timeout") != std::string::npos);
}
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
namespace {

// The OnStep wrapper is a process-wide singleton: each case starts and ends with it closed and its loss stamps taken.
struct OnStepReset {
    OnStepReset() { reset(); }
    ~OnStepReset() { reset(); }
    OnStepReset(const OnStepReset&) = delete;
    OnStepReset& operator=(const OnStepReset&) = delete;
    static void reset() {
        auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
        wrapper.disconnect();
        (void)wrapper.consume_link_lost_at();
        (void)wrapper.consume_last_alive_before_loss();
    }
};

alpacacore::vendor::onstep::ConnectionInfo onstep_serial_info(const std::string& path) {
    alpacacore::vendor::onstep::ConnectionInfo info;
    info.type = alpacacore::vendor::onstep::ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 9600;
    return info;
}

alpacacore::vendor::onstep::ConnectionInfo onstep_network_info(int port) {
    alpacacore::vendor::onstep::ConnectionInfo info;
    info.type = alpacacore::vendor::onstep::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    return info;
}

}  // namespace

// Falsified by: onstep_protocol_wrapper.cpp checked_read() not sending a LinkDead on the tty to lose_link_locked() (the
// read times out after 3 s with "Timeout waiting for OnStep response" and the link reads alive)
TEST_CASE("OnStep mount - a hangup while a command waits for its reply loses the link",
          "[onstep][serial][serial_read][issue1644]") {
    OnStepReset reset;
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(onstep_serial_info(mount.slave_path())));
    mount.sever_on_next_command();
    const auto start = Clock::now();
    int code = 0;
    std::string message;
    try {
        (void)wrapper.send_command(":GVP#", true, 3000);
    } catch (const alpacacore::AlpacaException& e) {
        code = e.error_code();
        message = e.what();
    }
    const auto took = Clock::now() - start;
    INFO("message: " << message);
    CHECK(code == alpacacore::AlpacaError::NotConnected);
    CHECK(message.find("link lost (read failed: ") != std::string::npos);
    CHECK(wrapper.link_lost_reason().find("read failed: ") != std::string::npos);
    CHECK_FALSE(wrapper.link_alive());
    CHECK(took < std::chrono::seconds(2));
}

// Falsified by: onstep_protocol_wrapper.cpp checked_read() returning a LinkDead on the TCP fd as a timeout (the old
// recv() loop read a closed peer as silence and timed out after 3 s with "Timeout waiting for OnStep response")
TEST_CASE("OnStep mount - a TCP hangup while a command waits for its reply reads as a hangup",
          "[onstep][network][serial_read][issue1644]") {
    OnStepReset reset;
    std::atomic<bool> asked{false};
    alpacacore::test::FakeMountServer server([&asked](const std::string& chunk) {
        if (chunk.rfind(":Hang", 0) == 0) asked.store(true);
        return std::string();
    });
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(onstep_network_info(server.port())));
    // The "mount" takes the command and then closes the connection: the write succeeded, the reply never comes.
    std::thread dropper([&] {
        const auto give_up = Clock::now() + std::chrono::seconds(2);
        while (!asked.load() && Clock::now() < give_up) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)server.drop_connections();
    });
    Clock::duration took{};
    const auto message = hangup_message([&] { (void)wrapper.send_command("Hang", true, 3000); }, took);
    dropper.join();
    INFO("message: " << message);
    CHECK(message.find("OnStep network link lost (hangup): ") != std::string::npos);
    CHECK(took < std::chrono::seconds(2));
}

// Falsified by: onstep_protocol_wrapper.cpp drain_stale() waiting only its 50 ms for a first byte (an ack 120 ms after
// a blind command, inside the 200 ms TCP read slice, stays and answers "Next" with "11")
TEST_CASE("OnStep mount - a blind-command drain waits one read slice for a stray ack",
          "[onstep][network][serial_read][issue1644]") {
    OnStepReset reset;
    alpacacore::test::FakeMountServer server([](const std::string& chunk) {
        if (chunk.rfind(":Blind", 0) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            return std::string("1");
        }
        if (chunk.rfind(":Next", 0) == 0) return std::string("1#");
        return std::string();
    });
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(onstep_network_info(server.port())));
    wrapper.send_command_blind("Blind");
    std::string next;
    try {
        next = wrapper.send_command("Next", true, 1000);
    } catch (const alpacacore::AlpacaException& e) {
        next = std::string("threw: ") + e.what();
    }
    wrapper.disconnect();
    CHECK(next == "1");
}

// Falsified by: onstep_protocol_wrapper.cpp read_response() using ReadSpec::Stop::IdleGap for the no-'#' read (the
// busy line keeps it reading past the '#' until the 3 s deadline)
TEST_CASE("OnStep mount - a read that does not require the '#' ends at the '#'",
          "[onstep][serial][serial_read][issue1644]") {
    OnStepReset reset;
    HangupMount mount;
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(onstep_serial_info(mount.slave_path())));
    mount.trickle_after_reply_on_next_command();
    Clock::duration took{};
    const auto reply = timed_reply([&] { return wrapper.send_command(":GXEE#", false, 3000); }, took);
    wrapper.disconnect();
    CHECK(reply == "1");
    CHECK(took < std::chrono::seconds(1));
}

// Falsified by: onstep_protocol_wrapper.cpp read_response() without spec.late_byte_grace (the read times out at
// 300 ms and the late '#' answers "Next")
TEST_CASE("OnStep mount - a '#' that arrives within one read slice after the timeout ends its own reply",
          "[onstep][network][serial_read][issue1644]") {
    OnStepReset reset;
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(onstep_network_info(server.port())));
    const auto [error, next] =
        late_then_next([&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); });
    wrapper.disconnect();
    INFO("error: " << error);
    CHECK(error.empty());
    CHECK(next == "1");
}

// Falsified by: onstep_protocol_wrapper.cpp read_response() reading to the timeout plus one read slice (the late "1#"
// is read as the reply)
TEST_CASE("OnStep mount - a reply that starts after the timeout is still a timeout",
          "[onstep][network][serial_read][issue1644]") {
    OnStepReset reset;
    alpacacore::test::FakeMountServer server(late_hash_reply);
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::onstep::OnStepProtocolWrapper::instance();
    REQUIRE(wrapper.connect(onstep_network_info(server.port())));
    const auto result = late_then_next(
        [&](const std::string& c, int timeout) { return wrapper.send_command(c, true, timeout); }, "Slow");
    wrapper.disconnect();
    CHECK(result.first.find("Timeout waiting for OnStep response") != std::string::npos);
}
#endif

#endif  // _WIN32

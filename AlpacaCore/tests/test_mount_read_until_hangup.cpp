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

// The connected-path reply read of the iOptron, Celestron NexStar, SynScan and
// ZWO mount wrappers goes through util::read_until (issue #1644, slice of
// #1604). Two properties, each per wrapper over a pty:
// - a line that goes away while a command waits for its reply reads as a
//   hangup at once, not as a timeout after the full reply window: the "mount"
//   takes the command and then severs the pty, so the write succeeds and the
//   read hears EIO;
// - a read that does not require the '#' still ends at the '#': the "mount"
//   answers "1#" and then keeps the line busy, so a read that ran past the '#'
//   would wait for the whole reply window.
// And per wrapper over TCP: a '#' that arrives in the read slice in which the
// timeout runs out still ends its own reply, as it did when the byte-at-a-time
// loop checked the timeout only after each 200 ms recv() and never after a '#';
// a '#' left in the line there would answer the next command.
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

namespace {

using Clock = std::chrono::steady_clock;

// A pty "mount" that echoes the NexStar / SynScan "K" check until armed. Armed
// with sever_on_next_command(), it severs the pty when the next bytes arrive:
// the command has been written, the reply never comes and the line is gone.
// Armed with trickle_after_reply_on_next_command(), it answers the next
// command with "1#" and then sends a '0' every 20 ms until it stops, so the
// line never goes idle after the '#'.
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
            const std::string reply =
                alpacacore::test::answer_handset_echo(std::string(buf, static_cast<std::size_t>(n)));
            if (!reply.empty()) alpacacore::test::pty_write_bounded(pty_.master_fd(), reply, stop_);
        }
    }

    alpacacore::test::PtyPair pty_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> armed_{false};
    std::atomic<bool> trickle_{false};
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

// A TCP "mount" that answers "Late" with "#" kLateReply after it arrives, past the kLateTimeoutMs the case gives the
// read but inside the 200 ms read slice that follows it, and answers "Next" with "1#" at once.
constexpr int kLateTimeoutMs = 300;
constexpr auto kLateReply = std::chrono::milliseconds(350);

std::string late_hash_reply(const std::string& chunk) {
    if (chunk.rfind("Late", 0) == 0) {
        std::this_thread::sleep_for(kLateReply);
        return "#";
    }
    if (chunk.rfind("Next", 0) == 0) return "1#";
    return alpacacore::test::answer_handset_echo(chunk);
}

// Sends "Late" with the short timeout, then "Next": the error the first threw (empty when its '#' ended it) and the
// reply to the second.
template <typename Send>
std::pair<std::string, std::string> late_then_next(Send&& send) {
    std::string error;
    try {
        (void)send("Late", kLateTimeoutMs);
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
// Falsified by: ioptron_protocol_wrapper.cpp reply_deadline() without the read slice (the read times out at 300 ms
// and the late '#' answers "Next")
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
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
// Falsified by: celestron_protocol_wrapper.cpp reply_deadline() without the read slice (the read times out at 300 ms
// and the late '#' answers "Next")
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
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
// Falsified by: synscan_protocol_wrapper.cpp reply_deadline() without the read slice (the read times out at 300 ms
// and the late '#' answers "Next")
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
#endif

#ifdef ALPACACORE_ENABLE_ZWO
// Falsified by: zwo_mount_protocol_wrapper.cpp reply_deadline() without the read slice (the read times out at 300 ms)
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
#endif

#endif  // _WIN32

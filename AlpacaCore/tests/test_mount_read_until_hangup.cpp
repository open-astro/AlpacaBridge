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
// #1604): a line that goes away while a command waits for its reply reads as a
// hangup at once, not as a timeout after the full reply window. Each case
// connects a wrapper to a pty, lets the "mount" take the command and then
// severs the pty, so the write succeeds and the read hears EIO.
#ifndef _WIN32

#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "catch2_compat.h"
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

// A pty "mount" that echoes the NexStar / SynScan "K" check until armed, then
// severs the pty when the next bytes arrive: the command has been written, the
// reply never comes and the line is gone.
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

private:
    void run() {
        while (!stop_.load()) {
            struct pollfd pfd{};
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
            const std::string reply =
                alpacacore::test::answer_handset_echo(std::string(buf, static_cast<std::size_t>(n)));
            if (!reply.empty()) alpacacore::test::pty_write_bounded(pty_.master_fd(), reply, stop_);
        }
    }

    alpacacore::test::PtyPair pty_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> armed_{false};
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

#endif  // _WIN32

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

// A TCP peer hangup (EOF, ECONNRESET, EPIPE) is positive evidence that the transport is gone, so the iOptron,
// Celestron NexStar and SynScan mounts lose the link at once over TCP, as over serial (decision 0009 rule 2a, #2718):
// the socket is released, the exchange answers NotConnected, Connected reads false and DeviceState is empty, with no
// clock advance (the task clock is fake and never moves, so the 30 s staleness bound cannot be what ends the link).
// A reply timeout reports reachability, not a gone connection: it stays a failed exchange for the fault latch.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"
#include "fake_task_clock.h"

#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include "fake_ioptron_mount.h"
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#endif

namespace {

namespace err = alpacacore::AlpacaError;
using namespace std::chrono_literals;

// The driver's link is gone at once: Connected false, DeviceState empty, a read answers NotConnected.
void require_link_lost(alpacacore::TelescopeDriver& driver) {
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_device_state().empty());
    try {
        (void)driver.get_right_ascension();
        FAIL("a lost link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == err::NotConnected);
    }
}

// What a wrapper exchange threw: the error code, or 0 when it returned.
template <typename Command>
int thrown_code(Command&& command) {
    try {
        command();
    } catch (const alpacacore::AlpacaException& e) {
        return e.error_code();
    }
    return 0;
}

// Disconnects a singleton wrapper when the case ends, a failed REQUIRE included, so the next case can connect.
template <typename Wrapper>
struct DisconnectOnExit {
    Wrapper& wrapper;
    ~DisconnectOnExit() { wrapper.disconnect(); }
};

// The default handset until `mute` is set, then one that takes every command and answers nothing: a reply timeout,
// the link still up.
alpacacore::test::FakeMountServer::Responder mutable_responder(std::shared_ptr<std::atomic<bool>> mute) {
    auto base = alpacacore::test::FakeMountServer::default_responder();
    return [base, mute](const std::string& chunk) { return mute->load() ? std::string() : base(chunk); };
}

}  // namespace

#ifdef ALPACACORE_ENABLE_IOPTRON
namespace {

namespace ioptron = alpacacore::vendor::ioptron;

ioptron::ConnectionInfo ioptron_endpoint(int port) {
    ioptron::ConnectionInfo info;
    info.type = ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    return info;
}

}  // namespace

// Falsified by: ioptron_protocol_wrapper.cpp throw_link_lost() and fail_link_if_gone_locked() failing the link over
// serial only (the poll meets the reset and throws a DriverException; Connected stays true until the 30 s bound)
TEST_CASE("iOptron mount - a TCP hangup loses the link at once",
          "[ioptron][telescope][network][link-lost][issue2718]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;  // never advanced
    auto driver = ioptron::create_ioptron_telescope(0, ioptron_endpoint(mount.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, 10s));
    ioptron::set_state_poll_paused_for_testing(*driver, true);

    mount.reset_link();  // the controller's RST: the next send fails, the next read hears ECONNRESET
    try {
        ioptron::poll_state_now_for_testing(*driver);
    } catch (const alpacacore::AlpacaException&) {
    }
    require_link_lost(*driver);
    CHECK_FALSE(driver->get_link_fault().empty());
}

// Falsified by: ioptron_protocol_wrapper.cpp throw_link_lost() failing the link over serial only (a hangup while a
// command waits for its reply throws a DriverException and the wrapper stays linked)
TEST_CASE("iOptron mount - a TCP hangup while a command waits for its reply is NotConnected",
          "[ioptron][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto& wrapper = ioptron::iOptronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(ioptron_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    server.hang_up_on(":Hang");  // takes the command, never answers, resets the connection
    CHECK(thrown_code([&] { (void)wrapper.send_command(":Hang#", true, 3000); }) == err::NotConnected);
    CHECK_FALSE(wrapper.link_alive());
    CHECK_FALSE(wrapper.is_connected());
}

// Falsified by: ioptron_protocol_wrapper.cpp fail_link_if_gone_locked() judging a TCP send by link_.lost() only (a
// send to a reset connection throws "Failed to send command" as a DriverException)
TEST_CASE("iOptron mount - a send on a reset TCP connection is NotConnected",
          "[ioptron][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto& wrapper = ioptron::iOptronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(ioptron_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    // One answered exchange first: the fake registers a connection when it serves it, and drops only those.
    REQUIRE_NOTHROW((void)wrapper.send_command(":GVER#", true, 1000));
    REQUIRE(server.drop_connections());
    // The first send after a peer's reset can still go out; the second meets EPIPE or ECONNRESET.
    int code = 0;
    for (int i = 0; i < 3 && code == 0; ++i) {
        code = thrown_code([&] { (void)wrapper.send_command(":GVER#", true, 1000); });
    }
    CHECK(code == err::NotConnected);
    CHECK_FALSE(wrapper.link_alive());
}

// Falsified by: ioptron_protocol_wrapper.cpp throw_link_lost() or fail_link_if_gone_locked() treating a reply timeout
// as a gone link (the wrapper would latch the loss and answer NotConnected)
TEST_CASE("iOptron mount - a reply timeout over TCP stays a failed exchange", "[ioptron][network][issue2718]") {
    auto mute = std::make_shared<std::atomic<bool>>(false);
    alpacacore::test::FakeMountServer server(mutable_responder(mute));
    REQUIRE(server.ok());
    auto& wrapper = ioptron::iOptronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(ioptron_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    mute->store(true);
    CHECK(thrown_code([&] { (void)wrapper.send_command(":GVER#", true, 150); }) == err::DriverException);
    CHECK(wrapper.link_alive());
    CHECK(wrapper.is_connected());
}
#endif  // ALPACACORE_ENABLE_IOPTRON

#ifdef ALPACACORE_ENABLE_CELESTRON
namespace {

namespace cel = alpacacore::vendor::celestron;

cel::ConnectionInfo celestron_endpoint(int port) {
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

// A NexStar handset that answers a connect and a status poll.
alpacacore::test::FakeMountServer::Responder nexstar_responder() {
    return [](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            case 'w':
                return std::string(8, '\0') + "#";
            case 't':
                return std::string("\x02#");
            case 'J':
                return "1#";
            case 'P': {
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                if (op == 0xFE) return std::string("\x00\x00#", 3);
                return std::string("\xFF#");
            }
            default:
                return "0#";
        }
    };
}

}  // namespace

// Falsified by: celestron_protocol_wrapper.cpp throw_link_lost() and fail_link_if_gone_locked() failing the link over
// serial only (the poll meets the reset and throws a DriverException; Connected stays true until the 30 s bound)
TEST_CASE("Celestron mount - a TCP hangup loses the link at once",
          "[celestron][telescope][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server(nexstar_responder());
    REQUIRE(server.ok());
    alpacacore::test::FakeTaskClock clock;  // never advanced
    auto driver =
        cel::create_celestron_telescope_with_site(0, celestron_endpoint(server.port()), -33.856784, 151.215297, 42.0,
                                                  false, cel::CelestronAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, 10s));
    cel::set_status_poll_paused_for_testing(*driver, true);

    REQUIRE(server.drop_connections());  // the controller's RST: the next send fails
    try {
        (void)cel::poll_status_now_for_testing(*driver);
    } catch (const alpacacore::AlpacaException&) {
    }
    require_link_lost(*driver);
    CHECK_FALSE(driver->get_link_fault().empty());
}

// Falsified by: celestron_protocol_wrapper.cpp throw_link_lost() failing the link over serial only (a hangup while a
// command waits for its reply throws a DriverException and the wrapper stays linked)
TEST_CASE("Celestron mount - a TCP hangup while a command waits for its reply is NotConnected",
          "[celestron][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto& wrapper = cel::CelestronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(celestron_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    server.hang_up_on("Hang");  // takes the command, never answers, resets the connection
    CHECK(thrown_code([&] { (void)wrapper.send_command("Hang", true, 3000); }) == err::NotConnected);
    CHECK_FALSE(wrapper.link_alive());
    CHECK_FALSE(wrapper.is_connected());
}

// Falsified by: celestron_protocol_wrapper.cpp fail_link_if_gone_locked() judging a TCP send by link_.lost() only (a
// send to a reset connection throws "Failed to send command" as a DriverException)
TEST_CASE("Celestron mount - a send on a reset TCP connection is NotConnected",
          "[celestron][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto& wrapper = cel::CelestronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(celestron_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    REQUIRE_NOTHROW((void)wrapper.send_command("V", true, 1000));  // the fake drops only a connection it has served
    REQUIRE(server.drop_connections());
    int code = 0;
    for (int i = 0; i < 3 && code == 0; ++i) {
        code = thrown_code([&] { (void)wrapper.send_command("V", true, 1000); });
    }
    CHECK(code == err::NotConnected);
    CHECK_FALSE(wrapper.link_alive());
}

// Falsified by: celestron_protocol_wrapper.cpp throw_link_lost() or fail_link_if_gone_locked() treating a reply
// timeout as a gone link (the wrapper would latch the loss and answer NotConnected)
TEST_CASE("Celestron mount - a reply timeout over TCP stays a failed exchange", "[celestron][network][issue2718]") {
    auto mute = std::make_shared<std::atomic<bool>>(false);
    alpacacore::test::FakeMountServer server(mutable_responder(mute));
    REQUIRE(server.ok());
    auto& wrapper = cel::CelestronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(celestron_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    mute->store(true);
    CHECK(thrown_code([&] { (void)wrapper.send_command("V", true, 150); }) == err::DriverException);
    CHECK(wrapper.link_alive());
    CHECK(wrapper.is_connected());
}
#endif  // ALPACACORE_ENABLE_CELESTRON

#ifdef ALPACACORE_ENABLE_SYNSCAN
namespace {

namespace syn = alpacacore::vendor::synscan;

syn::ConnectionInfo synscan_endpoint(int port) {
    syn::ConnectionInfo info;
    info.type = syn::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

// A handset that answers a connect and a position poll.
alpacacore::test::FakeMountServer::Responder handset_responder() {
    auto base = alpacacore::test::FakeMountServer::default_responder();
    return [base](const std::string& chunk) {
        if (!chunk.empty() && chunk[0] == 'e') return std::string("00000000,00000000#");
        if (!chunk.empty() && chunk[0] == 't') return std::string("\x01#");
        return base(chunk);
    };
}

}  // namespace

// Falsified by: synscan_protocol_wrapper.cpp throw_link_lost() and fail_link_if_gone_locked() failing the link over
// serial only (the poll meets the reset and throws a DriverException; Connected stays true until the 30 s bound)
TEST_CASE("SynScan mount - a TCP hangup loses the link at once",
          "[synscan][telescope][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server(handset_responder());
    REQUIRE(server.ok());
    alpacacore::test::FakeTaskClock clock;  // never advanced
    auto driver =
        syn::create_synscan_telescope_with_site(0, synscan_endpoint(server.port()), syn::SynScanVersion::V4, -33.856784,
                                                151.215297, 42.0, false, syn::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, 10s));
    syn::set_status_poll_paused_for_testing(*driver, true);

    REQUIRE(server.drop_connections());  // the controller's RST: the next send fails
    try {
        syn::poll_status_now_for_testing(*driver);
    } catch (const alpacacore::AlpacaException&) {
    }
    require_link_lost(*driver);
    CHECK_FALSE(driver->get_link_fault().empty());
}

// Falsified by: synscan_protocol_wrapper.cpp throw_link_lost() failing the link over serial only (a hangup while a
// command waits for its reply throws a DriverException and the wrapper stays linked)
TEST_CASE("SynScan mount - a TCP hangup while a command waits for its reply is NotConnected",
          "[synscan][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto& wrapper = syn::SynScanProtocolWrapper::instance();
    REQUIRE(wrapper.connect(synscan_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    server.hang_up_on("Hang");  // takes the command, never answers, resets the connection
    CHECK(thrown_code([&] { (void)wrapper.send_command("Hang", true, 3000); }) == err::NotConnected);
    CHECK_FALSE(wrapper.link_alive());
    CHECK_FALSE(wrapper.is_connected());
}

// Falsified by: synscan_protocol_wrapper.cpp fail_link_if_gone_locked() judging a TCP send by link_.lost() only (a
// send to a reset connection throws "Failed to send command" as a DriverException)
TEST_CASE("SynScan mount - a send on a reset TCP connection is NotConnected",
          "[synscan][network][link-lost][issue2718]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto& wrapper = syn::SynScanProtocolWrapper::instance();
    REQUIRE(wrapper.connect(synscan_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    REQUIRE_NOTHROW((void)wrapper.send_command("V", true, 1000));  // the fake drops only a connection it has served
    REQUIRE(server.drop_connections());
    int code = 0;
    for (int i = 0; i < 3 && code == 0; ++i) {
        code = thrown_code([&] { (void)wrapper.send_command("V", true, 1000); });
    }
    CHECK(code == err::NotConnected);
    CHECK_FALSE(wrapper.link_alive());
}

// Falsified by: synscan_protocol_wrapper.cpp throw_link_lost() or fail_link_if_gone_locked() treating a reply timeout
// as a gone link (the wrapper would latch the loss and answer NotConnected)
TEST_CASE("SynScan mount - a reply timeout over TCP stays a failed exchange", "[synscan][network][issue2718]") {
    auto mute = std::make_shared<std::atomic<bool>>(false);
    alpacacore::test::FakeMountServer server(mutable_responder(mute));
    REQUIRE(server.ok());
    auto& wrapper = syn::SynScanProtocolWrapper::instance();
    REQUIRE(wrapper.connect(synscan_endpoint(server.port())));
    DisconnectOnExit guard{wrapper};
    mute->store(true);
    CHECK(thrown_code([&] { (void)wrapper.send_command("V", true, 150); }) == err::DriverException);
    CHECK(wrapper.link_alive());
    CHECK(wrapper.is_connected());
}
#endif  // ALPACACORE_ENABLE_SYNSCAN

#endif  // _WIN32

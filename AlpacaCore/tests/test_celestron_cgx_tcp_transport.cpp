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

// The SkyPortal WiFi (AUX over TCP) transport of CelestronCgxProtocol against a loopback fake adapter. The retry point
// and the backlog drain are wire timing (decision 0005 rule 6, steady_clock), so these cases wait real, bounded time.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_celestron_aux_tcp_adapter.h"
#include "fake_celestron_cgx_mount.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeCelestronAuxTcpAdapter;
using alpacacore::test::require_alpaca_error;
using alpacacore::vendor::celestron::AuxFrame;
using alpacacore::vendor::celestron::CelestronCgxProtocol;
using alpacacore::vendor::celestron::CgxAxis;
using Fault = FakeCelestronAuxTcpAdapter::Fault;
using std::chrono::milliseconds;

constexpr std::uint8_t kRa = 0x10;
constexpr std::uint8_t kDec = 0x11;

AuxFrame request(std::uint8_t destination, std::uint8_t command, std::vector<std::uint8_t> payload = {}) {
    return AuxFrame{.source = 0x00, .destination = destination, .command = command, .payload = std::move(payload)};
}

void connect(CelestronCgxProtocol& protocol, const FakeCelestronAuxTcpAdapter& adapter) {
    REQUIRE(adapter.ok());
    protocol.connect_tcp("127.0.0.1", adapter.port());
}

milliseconds since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - started);
}

}  // namespace

TEST_CASE("Celestron CGX AUX TCP - exchanges from source 0x20 on the wire", "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x123456);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);

    CHECK(protocol.is_connected());
    CHECK(protocol.is_healthy());
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x123456);
    const auto ack = protocol.exchange(request(kDec, 0x24, {0x00}), milliseconds(500));
    CHECK(ack.source == kDec);
    CHECK(ack.destination == 0x20);

    const auto frames = adapter.received();
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].source == 0x20);
    CHECK(frames[1].source == 0x20);
}

TEST_CASE("Celestron CGX AUX TCP - refuses a host name or a bad port and reports a refused connect",
          "[celestron][cgx][protocol][tcp]") {
    CelestronCgxProtocol protocol;
    require_alpaca_error([&] { protocol.connect_tcp("localhost", 2000); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { protocol.connect_tcp("", 2000); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { protocol.connect_tcp("127.0.0.1", 0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { protocol.connect_tcp("127.0.0.1", 65536); }, AlpacaError::InvalidValue);

    // A loopback port with nothing listening: bind one, read its number, close it.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    ::close(fd);
    require_alpaca_error([&] { protocol.connect_tcp("127.0.0.1", ntohs(address.sin_port)); },
                         AlpacaError::DriverException);
    CHECK_FALSE(protocol.is_connected());
}

TEST_CASE("Celestron CGX AUX TCP - ignores echoes, traffic for others and collision debris",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x0a0b0c);
    adapter.set_echo(true);
    adapter.set_foreign_traffic(true);
    adapter.script(kRa, 0x01, 1, Fault::Debris);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);

    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x0a0b0c);
    CHECK(protocol.is_axis_home_done(CgxAxis::RightAscension));
    CHECK(adapter.count(kRa, 0x01) == 1);  // no re-send: the reply was there, behind the debris
    CHECK(protocol.is_healthy());
}

TEST_CASE("Celestron CGX AUX TCP - re-sends a lost read-only query once and keeps the session",
          "[celestron][cgx][protocol][tcp]") {
    struct Query {
        std::uint8_t destination;
        std::uint8_t command;
    };
    const Query queries[] = {{kRa, 0x01},  {kDec, 0x01}, {kRa, 0x05}, {kRa, 0x12},
                             {kDec, 0x13}, {kDec, 0x27}, {kRa, 0xfe}};
    for (const auto& query : queries) {
        CAPTURE(query.destination, query.command);
        FakeCelestronAuxTcpAdapter adapter;
        adapter.script(query.destination, query.command, 1, Fault::Drop);
        CelestronCgxProtocol protocol;
        connect(protocol, adapter);
        const auto session = protocol.session_generation();

        const auto started = std::chrono::steady_clock::now();
        const auto reply = protocol.exchange(request(query.destination, query.command), milliseconds(1000));
        CHECK(reply.source == query.destination);
        CHECK(since(started) >= milliseconds(300));
        CHECK(adapter.count(query.destination, query.command) == 2);
        CHECK(protocol.is_healthy());
        CHECK(protocol.session_generation() == session);
    }
}

TEST_CASE("Celestron CGX AUX TCP - a second miss is a link fault and is never sent a third time",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x000777);
    adapter.script(kRa, 0x01, 1, Fault::Drop);
    adapter.script(kRa, 0x01, 2, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    const auto session = protocol.session_generation();

    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(1000)); },
                         AlpacaError::DriverException);
    CHECK(adapter.count(kRa, 0x01) == 2);
    CHECK_FALSE(protocol.is_healthy());
    CHECK(protocol.session_generation() != session);
    CHECK(protocol.link_alive());  // a timeout is not evidence the transport is gone
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension); },
                         AlpacaError::DriverException);
    CHECK(adapter.count(kRa, 0x01) == 2);  // refused while desynchronized

    protocol.resync();  // drains the socket; never a tcflush on it
    CHECK(protocol.is_connected());
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x000777);
    CHECK(protocol.is_healthy());
}

TEST_CASE("Celestron CGX AUX TCP - never re-sends a motion or write command", "[celestron][cgx][protocol][tcp]") {
    struct Write {
        const char* name;
        std::uint8_t command;
        std::vector<std::uint8_t> payload;
    };
    const std::vector<Write> writes = {{"MC_GOTO_FAST", 0x02, {0x01, 0x02, 0x03}},
                                       {"MC_GOTO_SLOW", 0x17, {0x01, 0x02, 0x03}},
                                       {"MC_SET_POSITION", 0x04, {0x01, 0x02, 0x03}},
                                       {"MC_LEVEL_START", 0x0b, {}},
                                       {"MC_AUX_GUIDE", 0x26, {0x32, 0x0a}},
                                       {"MC_MOVE_POS (stop)", 0x24, {0x00}},
                                       {"MC_SET_POS_GUIDERATE (rate zero)", 0x06, {0x00, 0x00, 0x00}}};
    for (const auto& write : writes) {
        CAPTURE(write.name);
        FakeCelestronAuxTcpAdapter adapter;
        adapter.script(kDec, write.command, 1, Fault::Drop);
        CelestronCgxProtocol protocol;
        connect(protocol, adapter);
        require_alpaca_error(
            [&] { (void)protocol.exchange(request(kDec, write.command, write.payload), milliseconds(450)); },
            AlpacaError::DriverException);
        CHECK(adapter.count(kDec, write.command) == 1);
    }
}

TEST_CASE("Celestron CGX AUX TCP - no re-send when the deadline comes before the retry point",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.script(kRa, 0x01, 1, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(250)); },
                         AlpacaError::DriverException);
    CHECK(adapter.count(kRa, 0x01) == 1);
}

TEST_CASE("Celestron CGX AUX TCP - a bad checksum resyncs and the lost query is re-sent",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kDec, 0x765432);
    adapter.script(kDec, 0x01, 1, Fault::BadChecksum);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    const auto session = protocol.session_generation();

    CHECK(protocol.get_axis_position(CgxAxis::Declination, milliseconds(1000)) == 0x765432);
    CHECK(adapter.count(kDec, 0x01) == 2);
    CHECK(protocol.is_healthy());
    CHECK(protocol.session_generation() == session);
}

TEST_CASE("Celestron CGX AUX TCP - a late reply is taken and its duplicate never answers the next query",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x111111);
    adapter.script(kRa, 0x01, 1, Fault::HoldUntilNext);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);

    // The held reply arrives with the re-send's reply, in one segment: one is the answer, the other a duplicate.
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(1000)) == 0x111111);
    CHECK(adapter.count(kRa, 0x01) == 2);
    adapter.set_position(kRa, 0x222222);
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x222222);
    CHECK(adapter.count(kRa, 0x01) == 3);
}

TEST_CASE("Celestron CGX AUX TCP - assembles a reply split across segments", "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kDec, 0x00abcd);
    adapter.script(kDec, 0x01, 1, Fault::Split);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0x00abcd);
    CHECK(adapter.count(kDec, 0x01) == 1);
}

TEST_CASE("Celestron CGX AUX TCP - the adapter closing the connection is a lost link until the next connect",
          "[celestron][cgx][protocol][tcp]") {
    Fault fault = Fault::Close;
    SECTION("close") {}
    SECTION("reset") { fault = Fault::Reset; }
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x000042);
    adapter.script(kRa, 0x01, 1, fault);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);

    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(1000)); },
                         AlpacaError::NotConnected);
    CHECK_FALSE(protocol.link_alive());
    CHECK_FALSE(protocol.is_connected());
    CHECK(protocol.link_fault().find("TCP link lost") != std::string::npos);
    CHECK(adapter.count(kRa, 0x01) == 1);
    require_alpaca_error([&] { protocol.resync(); }, AlpacaError::NotConnected);

    connect(protocol, adapter);
    CHECK(protocol.link_alive());
    CHECK(protocol.link_fault().empty());
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x000042);
    CHECK(adapter.connections() == 2);
}

TEST_CASE("Celestron CGX AUX TCP - discards the adapter's startup backlog", "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x0c0ffe);
    // Replies of an earlier session to source 0x20, as the Gen4 adapter replayed them on a fresh connection.
    std::vector<std::uint8_t> backlog;
    for (int i = 0; i < 8; ++i) {
        const auto stale =
            FakeCelestronAuxTcpAdapter::frame(kRa, 0x20, 0x01, {0x66, 0x66, static_cast<std::uint8_t>(i)});
        backlog.insert(backlog.end(), stale.begin(), stale.end());
    }
    // 20 ms after accept, well inside the 200 ms quiet window the connect waits out.
    adapter.set_startup_backlog(backlog, milliseconds(20));
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);

    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x0c0ffe);
    CHECK(adapter.count(kRa, 0x01) == 1);
}

TEST_CASE("Celestron CGX AUX TCP - disconnect closes the socket and a reconnect opens a new one",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kDec, 0x000100);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0x000100);
    protocol.disconnect();
    CHECK_FALSE(protocol.is_connected());
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::Declination); }, AlpacaError::NotConnected);

    connect(protocol, adapter);
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0x000100);
    CHECK(adapter.connections() == 2);
}

TEST_CASE("Celestron CGX AUX link - a lost USB reply is never re-sent", "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    mount.set_muted(true);

    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(500)); },
                         AlpacaError::DriverException);
    int sent = 0;
    for (const auto& frame : mount.requests()) {
        if (frame.destination == kRa && frame.command == 0x01) {
            ++sent;
            CHECK(frame.source == 0x00);
        }
    }
    CHECK(sent == 1);
    protocol.disconnect();
}

TEST_CASE("Celestron CGX AUX link - a USB connect after a TCP session speaks USB", "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    protocol.connect_serial(mount.slave_path());
    CHECK(adapter.connections() == 1);

    const auto reply = protocol.exchange(request(kRa, 0xfe), milliseconds(500));
    CHECK(reply.destination == 0x00);
    const auto frames = mount.requests();
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.back().source == 0x00);
    protocol.disconnect();
    require_alpaca_error([&] { (void)protocol.exchange(request(kRa, 0xfe), milliseconds(100)); },
                         AlpacaError::NotConnected);
    try {
        (void)protocol.exchange(request(kRa, 0xfe), milliseconds(100));
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(std::string(error.what()) == "Celestron CGX USB is not connected");
    }
}

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
#include <thread>
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

// Rig 2026-11-21: during a home the adapter lost requests in bursts, a query and its first re-send both (slice 5a).
TEST_CASE("Celestron CGX AUX TCP - a query whose first re-send is lost too is recovered by the second",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kDec, 0x000555);
    adapter.script(kDec, 0x01, 1, Fault::Drop);
    adapter.script(kDec, 0x01, 2, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    const auto session = protocol.session_generation();

    const auto started = std::chrono::steady_clock::now();
    CHECK(protocol.get_axis_position(CgxAxis::Declination, milliseconds(1000)) == 0x000555);
    CHECK(since(started) >= milliseconds(600));
    CHECK(adapter.count(kDec, 0x01) == 3);
    CHECK(protocol.is_healthy());
    CHECK(protocol.session_generation() == session);
    CHECK(protocol.recovered_retries() == 1);  // one lost reply recovered, however many sends it took
}

TEST_CASE("Celestron CGX AUX TCP - a third miss is a link fault and is never sent a fourth time",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x000777);
    adapter.script(kRa, 0x01, 1, Fault::Drop);
    adapter.script(kRa, 0x01, 2, Fault::Drop);
    adapter.script(kRa, 0x01, 3, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    const auto session = protocol.session_generation();

    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(1000)); },
                         AlpacaError::DriverException);
    CHECK(adapter.count(kRa, 0x01) == 3);
    CHECK_FALSE(protocol.is_healthy());
    CHECK(protocol.session_generation() != session);
    CHECK(protocol.recovered_retries() == 0);
    CHECK(protocol.link_alive());  // a timeout is not evidence the transport is gone
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension); },
                         AlpacaError::DriverException);
    CHECK(adapter.count(kRa, 0x01) == 3);  // refused while desynchronized

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
    const std::vector<Write> writes = {
        {"MC_GOTO_FAST", 0x02, {0x01, 0x02, 0x03}},    {"MC_GOTO_SLOW", 0x17, {0x01, 0x02, 0x03}},
        {"MC_SET_POSITION", 0x04, {0x01, 0x02, 0x03}}, {"MC_LEVEL_START", 0x0b, {}},
        {"MC_AUX_GUIDE", 0x26, {0x32, 0x0a}},          {"MC_SET_POS_GUIDERATE (rate zero)", 0x06, {0x00, 0x00, 0x00}}};
    // The stop (0x24 {0}) is in the confirmation cases below: on this idle mount the status shows a still axis.
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

namespace {

// A motion command over TCP whose reply has not come in 300 ms is confirmed through the axis status (slice 3).
// `started`: it returned as acknowledged, the session kept and the recovery counted. Otherwise it failed as a lost
// reply does: desynchronized and a new session. Either way it went out exactly once.
void check_confirmed(const CelestronCgxProtocol& protocol, std::uint64_t session,
                     std::chrono::steady_clock::time_point started) {
    CHECK(since(started) >= milliseconds(300));
    CHECK(protocol.is_healthy());
    CHECK(protocol.session_generation() == session);
    CHECK(protocol.recovered_retries() == 1);
}

void check_not_confirmed(const CelestronCgxProtocol& protocol, std::uint64_t session) {
    CHECK_FALSE(protocol.is_healthy());
    CHECK(protocol.session_generation() != session);
    CHECK(protocol.recovered_retries() == 0);
    CHECK(protocol.link_alive());
}

}  // namespace

TEST_CASE("Celestron CGX AUX TCP - a lost LEVEL_START reply is confirmed by LEVEL_DONE searching",
          "[celestron][cgx][protocol][tcp][motion-confirm]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_home_duration(milliseconds(3000));
    CelestronCgxProtocol protocol;
    SECTION("the search started: the reply alone was lost") {
        adapter.script(kRa, 0x0b, 1, Fault::DropReply);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        const auto started = std::chrono::steady_clock::now();
        protocol.start_axis_home(CgxAxis::RightAscension);
        check_confirmed(protocol, session, started);
        CHECK(adapter.count(kRa, 0x12) == 1);
    }
    SECTION("the request was lost: LEVEL_DONE reads done, which a search never started reads too") {
        adapter.script(kRa, 0x0b, 1, Fault::Drop);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        require_alpaca_error([&] { protocol.start_axis_home(CgxAxis::RightAscension); }, AlpacaError::DriverException);
        check_not_confirmed(protocol, session);
    }
    CHECK(adapter.count(kRa, 0x0b) == 1);
}

TEST_CASE("Celestron CGX AUX TCP - a lost GOTO reply is confirmed by SLEW_DONE and progress toward the target",
          "[celestron][cgx][protocol][tcp][motion-confirm]") {
    for (const std::uint8_t command : {std::uint8_t{0x02}, std::uint8_t{0x17}}) {
        CAPTURE(command);
        const auto send = [command](CelestronCgxProtocol& protocol, std::uint32_t target) {
            if (command == 0x02)
                protocol.goto_axis_fast(CgxAxis::Declination, target);
            else
                protocol.goto_axis_slow(CgxAxis::Declination, target);
        };
        {
            INFO("the GOTO runs: the reply alone was lost");
            FakeCelestronAuxTcpAdapter adapter;
            adapter.set_position(kDec, 0x100000);
            adapter.set_goto_duration(milliseconds(3000));
            adapter.script(kDec, command, 1, Fault::DropReply);
            CelestronCgxProtocol protocol;
            connect(protocol, adapter);
            const auto session = protocol.session_generation();
            const auto started = std::chrono::steady_clock::now();
            send(protocol, 0x300000);
            check_confirmed(protocol, session, started);
            CHECK(adapter.count(kDec, command) == 1);
            CHECK(adapter.count(kDec, 0x13) == 1);
        }
        {
            INFO("the request was lost: the axis is done where it was");
            FakeCelestronAuxTcpAdapter adapter;
            adapter.set_position(kDec, 0x100000);
            adapter.set_goto_duration(milliseconds(3000));
            adapter.script(kDec, command, 1, Fault::Drop);
            CelestronCgxProtocol protocol;
            connect(protocol, adapter);
            const auto session = protocol.session_generation();
            require_alpaca_error([&] { send(protocol, 0x300000); }, AlpacaError::DriverException);
            check_not_confirmed(protocol, session);
            CHECK(adapter.count(kDec, command) == 1);
        }
        {
            INFO("the request was lost while the axis runs elsewhere: busy, but moving away from the target");
            FakeCelestronAuxTcpAdapter adapter;
            adapter.set_position(kDec, 0x400000);
            adapter.start_goto(kDec, 0x200000, milliseconds(5000));
            adapter.script(kDec, command, 1, Fault::Drop);
            CelestronCgxProtocol protocol;
            connect(protocol, adapter);
            const auto session = protocol.session_generation();
            require_alpaca_error([&] { send(protocol, 0x600000); }, AlpacaError::DriverException);
            check_not_confirmed(protocol, session);
            CHECK(adapter.count(kDec, command) == 1);
        }
        {
            INFO("a GOTO with nowhere to go: done at its target");
            FakeCelestronAuxTcpAdapter adapter;
            adapter.set_position(kDec, 0x123456);
            adapter.script(kDec, command, 1, Fault::DropReply);
            CelestronCgxProtocol protocol;
            connect(protocol, adapter);
            const auto session = protocol.session_generation();
            const auto started = std::chrono::steady_clock::now();
            send(protocol, 0x123456);
            check_confirmed(protocol, session, started);
        }
    }
}

TEST_CASE("Celestron CGX AUX TCP - a lost SET_POSITION reply is confirmed by the label readback",
          "[celestron][cgx][protocol][tcp][motion-confirm]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x000100);
    CelestronCgxProtocol protocol;
    SECTION("the label was written: the reply alone was lost") {
        adapter.script(kRa, 0x04, 1, Fault::DropReply);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        const auto started = std::chrono::steady_clock::now();
        protocol.set_axis_position(CgxAxis::RightAscension, 0x654321);
        check_confirmed(protocol, session, started);
        CHECK(adapter.position(kRa) == 0x654321);
    }
    SECTION("the request was lost: the readback is the old label") {
        adapter.script(kRa, 0x04, 1, Fault::Drop);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        require_alpaca_error([&] { protocol.set_axis_position(CgxAxis::RightAscension, 0x654321); },
                             AlpacaError::DriverException);
        check_not_confirmed(protocol, session);
    }
    CHECK(adapter.count(kRa, 0x04) == 1);
}

TEST_CASE("Celestron CGX AUX TCP - a lost AUX_GUIDE reply is confirmed by AUX_GUIDE_ACTIVE",
          "[celestron][cgx][protocol][tcp][motion-confirm]") {
    FakeCelestronAuxTcpAdapter adapter;
    CelestronCgxProtocol protocol;
    SECTION("the pulse runs: the reply alone was lost") {
        adapter.script(kDec, 0x26, 1, Fault::DropReply);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        const auto started = std::chrono::steady_clock::now();
        protocol.pulse_guide_axis(CgxAxis::Declination, 50, 150);  // 1.5 s: still active when the status is read
        check_confirmed(protocol, session, started);
        CHECK(adapter.count(kDec, 0x27) == 1);
    }
    SECTION("the request was lost: no pulse is active") {
        adapter.script(kDec, 0x26, 1, Fault::Drop);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        require_alpaca_error([&] { protocol.pulse_guide_axis(CgxAxis::Declination, 50, 150); },
                             AlpacaError::DriverException);
        check_not_confirmed(protocol, session);
    }
    CHECK(adapter.count(kDec, 0x26) == 1);
}

TEST_CASE("Celestron CGX AUX TCP - a lost stop reply is confirmed by SLEW_DONE and still encoders",
          "[celestron][cgx][protocol][tcp][motion-confirm]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kDec, 0x200000);
    CelestronCgxProtocol protocol;
    SECTION("the stop took: the reply alone was lost") {
        // A drift reads done on SLEW_DONE while the axis moves, so only the encoders tell the two cases apart.
        adapter.set_drift(kDec, 1.0e6);
        adapter.script(kDec, 0x24, 1, Fault::DropReply);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        const auto started = std::chrono::steady_clock::now();
        (void)protocol.exchange(request(kDec, 0x24, {0x00}));
        check_confirmed(protocol, session, started);
        CHECK(adapter.count(kDec, 0x01) == 2);
    }
    SECTION("the request was lost: the axis still moves") {
        adapter.set_drift(kDec, 1.0e6);
        adapter.script(kDec, 0x24, 1, Fault::Drop);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        require_alpaca_error([&] { (void)protocol.exchange(request(kDec, 0x24, {0x00})); },
                             AlpacaError::DriverException);
        check_not_confirmed(protocol, session);
    }
    SECTION("the request was lost on an idle axis: still, which is what a stop is for") {
        adapter.script(kDec, 0x24, 1, Fault::Drop);
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        const auto started = std::chrono::steady_clock::now();
        (void)protocol.exchange(request(kDec, 0x24, {0x00}));
        check_confirmed(protocol, session, started);
    }
    CHECK(adapter.count(kDec, 0x24) == 1);
}

TEST_CASE("Celestron CGX AUX link - a lost USB motion reply reads no status",
          "[celestron][cgx][protocol][integration]") {
    struct Motion {
        const char* name;
        std::uint8_t command;
        std::vector<std::uint8_t> payload;
    };
    const std::vector<Motion> motions = {{"MC_GOTO_FAST", 0x02, {0x01, 0x02, 0x03}},
                                         {"MC_SET_POSITION", 0x04, {0x01, 0x02, 0x03}},
                                         {"MC_LEVEL_START", 0x0b, {}},
                                         {"MC_AUX_GUIDE", 0x26, {0x32, 0x96}},
                                         {"MC_MOVE_POS (stop)", 0x24, {0x00}}};
    for (const auto& motion : motions) {
        CAPTURE(motion.name);
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        CelestronCgxProtocol protocol;
        protocol.connect_serial(mount.slave_path());
        // The command reaches the motor and runs; only its reply is lost, so a status read would confirm it.
        mount.drop_next_reply(kRa, motion.command, true);
        const auto before = mount.requests().size();
        require_alpaca_error([&] { (void)protocol.exchange(request(kRa, motion.command, motion.payload)); },
                             AlpacaError::DriverException);
        const auto frames = mount.requests();
        REQUIRE(frames.size() == before + 1);
        CHECK(frames.back().command == motion.command);
        CHECK_FALSE(protocol.is_healthy());
        protocol.disconnect();
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

TEST_CASE("Celestron CGX AUX TCP - a re-send goes out only while the deadline is later",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.script(kRa, 0x01, 1, Fault::Drop);
    adapter.script(kRa, 0x01, 2, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    // 300 ms is before a 500 ms deadline, 600 ms is not: one re-send, then the timeout at the deadline.
    const auto started = std::chrono::steady_clock::now();
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension, milliseconds(500)); },
                         AlpacaError::DriverException);
    CHECK(since(started) < milliseconds(590));  // never waits for a re-send point past the deadline
    CHECK(adapter.count(kRa, 0x01) == 2);
}

TEST_CASE("Celestron CGX AUX TCP - a burst of lost requests after a home start", "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kDec, 0x123456);
    adapter.set_home_duration(milliseconds(3000));
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    const auto session = protocol.session_generation();
    SECTION("the query and its first re-send lost: the second re-send is answered") {
        adapter.set_home_burst_loss(2, kDec);
        protocol.start_axis_home(CgxAxis::RightAscension);
        CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0x123456);
        CHECK(adapter.count(kDec, 0x01) == 3);
        CHECK(protocol.is_healthy());
        CHECK(protocol.session_generation() == session);
        CHECK(protocol.recovered_retries() == 1);
    }
    SECTION("all three lost: the usual timeout") {
        adapter.set_home_burst_loss(3, kDec);
        protocol.start_axis_home(CgxAxis::RightAscension);
        require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::Declination); },
                             AlpacaError::DriverException);
        CHECK(adapter.count(kDec, 0x01) == 3);
        CHECK_FALSE(protocol.is_healthy());
        CHECK(protocol.session_generation() != session);
        CHECK(protocol.link_alive());
    }
    CHECK(adapter.count(kRa, 0x0b) == 1);
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

namespace {

// The ACKs the client sent the adapter for motor reports: frames of command 0x50 or 0x51.
std::vector<AuxFrame> report_acks(const FakeCelestronAuxTcpAdapter& adapter) {
    std::vector<AuxFrame> acks;
    for (const auto& frame : adapter.received())
        if (frame.command == 0x50 || frame.command == 0x51) acks.push_back(frame);
    return acks;
}

// The ACKs once the fake has read `count` of them: the exchange returns as soon as its reply is in, before the fake
// reads the ACK sent just ahead of it (real time, bounded at 1 s).
std::vector<AuxFrame> wait_for_report_acks(const FakeCelestronAuxTcpAdapter& adapter, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + milliseconds(1000);
    while (report_acks(adapter).size() < count && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(5));
    return report_acks(adapter);
}

}  // namespace

TEST_CASE("Celestron CGX AUX TCP - an exchange without a timeout gets 1000 ms, room for the re-send",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x0abcde);
    // Lost, re-sent at 300 ms, and the re-send answered 250 ms late: past 500 ms, inside 1000 ms.
    adapter.script(kRa, 0x01, 1, Fault::Drop);
    adapter.script(kRa, 0x01, 2, Fault::Late);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x0abcde);
    CHECK(adapter.count(kRa, 0x01) >= 2);  // the 600 ms re-send may go out just before the late reply lands
    CHECK(protocol.is_healthy());
}

TEST_CASE("Celestron CGX AUX link - a USB exchange without a timeout still gets 500 ms",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    mount.set_muted(true);
    const auto started = std::chrono::steady_clock::now();
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension); },
                         AlpacaError::DriverException);
    CHECK(since(started) >= milliseconds(450));
    CHECK(since(started) < milliseconds(900));
    protocol.disconnect();
}

TEST_CASE("Celestron CGX AUX TCP - counts the lost replies a re-send recovered, per session",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.script(kDec, 0x13, 1, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    CHECK(protocol.recovered_retries() == 0);
    CHECK(protocol.is_axis_slew_done(CgxAxis::Declination));
    CHECK(protocol.recovered_retries() == 1);
    CHECK(protocol.is_axis_slew_done(CgxAxis::Declination));
    CHECK(protocol.recovered_retries() == 1);
    connect(protocol, adapter);
    CHECK(protocol.recovered_retries() == 0);
}

TEST_CASE("Celestron CGX AUX TCP - one link per adapter endpoint, released on every close",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    CelestronCgxProtocol first;
    connect(first, adapter);
    CelestronCgxProtocol second("Celestron CGX second mount");
    try {
        second.connect_tcp("127.0.0.1", adapter.port());
        FAIL("a second link to the same adapter was accepted");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == AlpacaError::InvalidOperation);
        CHECK(std::string(error.what()).find("Celestron CGX mount") != std::string::npos);
    }
    CHECK_FALSE(second.is_connected());
    CHECK(adapter.connections() == 1);

    first.disconnect();
    connect(second, adapter);
    CHECK(second.get_axis_position(CgxAxis::RightAscension) == 0);

    // A lost link releases the endpoint too.
    adapter.script(kRa, 0x01, 2, Fault::Close);
    require_alpaca_error([&] { (void)second.get_axis_position(CgxAxis::RightAscension); }, AlpacaError::NotConnected);
    connect(first, adapter);
    CHECK(first.get_axis_position(CgxAxis::RightAscension) == 0);
}

TEST_CASE("Celestron CGX AUX TCP - a low-battery report is acknowledged and the exchange goes on",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.set_position(kRa, 0x000321);
    adapter.inject_before_reply(kRa, 0x01, 1, FakeCelestronAuxTcpAdapter::frame(kRa, 0x20, 0x50, {0x00}));
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    const auto session = protocol.session_generation();
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x000321);
    CHECK(protocol.is_healthy());
    CHECK(protocol.session_generation() == session);
    const auto acks = wait_for_report_acks(adapter, 1);
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].source == 0x20);
    CHECK(acks[0].destination == kRa);
    CHECK(acks[0].command == 0x50);
    CHECK(acks[0].payload.empty());
}

TEST_CASE("Celestron CGX AUX TCP - a motor fault report is acknowledged and fails the session",
          "[celestron][cgx][protocol][tcp]") {
    struct Report {
        std::uint8_t source;
        std::uint8_t destination;
        std::uint8_t command;
        std::vector<std::uint8_t> payload;
    };
    const Report reports[] = {
        {kDec, 0x20, 0x51, {0x82}}, {kRa, 0x00, 0x51, {0x7f}}, {kRa, 0x00, 0x50, {0x01}}, {kDec, 0x20, 0x50, {}}};
    for (const auto& report : reports) {
        CAPTURE(report.source, report.destination, report.command, report.payload.size());
        FakeCelestronAuxTcpAdapter adapter;
        adapter.inject_before_reply(
            kRa, 0x01, 1,
            FakeCelestronAuxTcpAdapter::frame(report.source, report.destination, report.command, report.payload));
        CelestronCgxProtocol protocol;
        connect(protocol, adapter);
        const auto session = protocol.session_generation();
        require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension); },
                             AlpacaError::DriverException);
        CHECK_FALSE(protocol.is_healthy());
        CHECK(protocol.session_generation() != session);
        CHECK(protocol.link_alive());
        const auto acks = wait_for_report_acks(adapter, 1);
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].source == report.destination);
        CHECK(acks[0].destination == report.source);
        CHECK(acks[0].command == report.command);
        CHECK(acks[0].payload.empty());
    }
}

TEST_CASE("Celestron CGX AUX TCP - a report between exchanges is handled by the next one, never dropped",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0);

    adapter.inject(FakeCelestronAuxTcpAdapter::frame(kDec, 0x20, 0x50, {0x00}));
    std::this_thread::sleep_for(milliseconds(100));  // real time: the fake's serve slice sends it
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0);
    CHECK(wait_for_report_acks(adapter, 1).size() == 1);

    const auto session = protocol.session_generation();
    adapter.inject(FakeCelestronAuxTcpAdapter::frame(kDec, 0x20, 0x51, {0x01}));
    std::this_thread::sleep_for(milliseconds(100));
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::Declination); }, AlpacaError::DriverException);
    CHECK(wait_for_report_acks(adapter, 2).size() == 2);
    CHECK(protocol.session_generation() != session);
    protocol.resync();
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0);
}

TEST_CASE("Celestron CGX AUX TCP - reports from other devices or for other clients get no ACK",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    std::vector<std::uint8_t> chatter = FakeCelestronAuxTcpAdapter::frame(0x12, 0x20, 0x51, {0x01});
    const auto for_hand_control = FakeCelestronAuxTcpAdapter::frame(kRa, 0x0d, 0x51, {0x01});
    chatter.insert(chatter.end(), for_hand_control.begin(), for_hand_control.end());
    adapter.inject_before_reply(kRa, 0x01, 1, chatter);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0);
    CHECK(protocol.is_healthy());
    std::this_thread::sleep_for(milliseconds(100));  // real time: an ACK sent in error would have been read by now
    CHECK(report_acks(adapter).empty());
}

TEST_CASE("Celestron CGX AUX TCP - a send-only stop goes out on a desynchronized stream",
          "[celestron][cgx][protocol][tcp]") {
    FakeCelestronAuxTcpAdapter adapter;
    adapter.script(kRa, 0x01, 1, Fault::Drop);
    adapter.script(kRa, 0x01, 2, Fault::Drop);
    adapter.script(kRa, 0x01, 3, Fault::Drop);
    CelestronCgxProtocol protocol;
    connect(protocol, adapter);
    require_alpaca_error([&] { (void)protocol.get_axis_position(CgxAxis::RightAscension); },
                         AlpacaError::DriverException);
    REQUIRE_FALSE(protocol.is_healthy());

    protocol.send_stop(CgxAxis::RightAscension);
    const auto deadline = std::chrono::steady_clock::now() + milliseconds(1000);
    while (adapter.count(kRa, 0x06) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(5));  // real time: the fake reads the frames
    CHECK(adapter.count(kRa, 0x24) == 1);
    CHECK(adapter.count(kRa, 0x06) == 1);
    for (const auto& frame : adapter.received()) {
        if (frame.command == 0x24) CHECK(frame.payload == std::vector<std::uint8_t>{0x00});
        if (frame.command == 0x06) CHECK(frame.payload == std::vector<std::uint8_t>{0x00, 0x00, 0x00});
    }
    protocol.disconnect();
    require_alpaca_error([&] { protocol.send_stop(CgxAxis::Declination); }, AlpacaError::NotConnected);
}

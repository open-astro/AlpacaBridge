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

// Contract of FakeZwoMount (fake_zwo_mount.h): the stateful ZWO mount fake must behave as
// the driver's wrapper expects before any driver test relies on it. These cases talk to it
// over its socket with a raw client, so they pin the fake, not the driver.

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"

namespace {

using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZwoMount;
using namespace std::chrono_literals;

// One loopback connection; ask() sends a command and reads up to the reply's '#'.
class Client {
public:
    explicit Client(int port) {
        REQUIRE(port > 0);
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        timeval timeout{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    ~Client() { ::close(fd_); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void send(const std::string& command) { REQUIRE(::send(fd_, command.data(), command.size(), 0) > 0); }
    std::string read_reply() {
        std::string reply;
        char ch = 0;
        while (reply.empty() || reply.back() != '#') {
            if (::recv(fd_, &ch, 1, 0) != 1) break;
            reply += ch;
        }
        return reply;
    }
    std::string ask(const std::string& command) {
        send(command);
        return read_reply();
    }
    /// True once the peer has closed or reset the connection: recv sees end of file or an error, not the receive
    /// timeout.
    bool closed() {
        char ch = 0;
        const auto n = ::recv(fd_, &ch, 1, 0);
        return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
    }

private:
    int fd_ = -1;
};

struct Rig {
    FakeTaskClock clock;
    FakeZwoMount mount{clock};
    Client client{mount.port()};
};

}  // namespace

TEST_CASE("FakeZwoMount - an accepted GOTO reports slewing until the slew duration is advanced", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.set_slew_duration(10s);
    CHECK(rig.client.ask(":GU#") == "NG#");
    CHECK(rig.client.ask(":Sr06:00:00#") == "1#");
    CHECK(rig.client.ask(":Sd+30*00:00#") == "1#");
    CHECK(rig.client.ask(":MS#") == "0#");
    CHECK(rig.client.ask(":GU#") == "G#");         // no 'N': the mount is moving
    CHECK(rig.client.ask(":GR#") == "12:00:00#");  // still at the start position
    rig.clock.advance(9s);
    CHECK(rig.client.ask(":GU#") == "G#");
    rig.clock.advance(1s);
    CHECK(rig.client.ask(":GU#") == "NG#");
    CHECK(rig.client.ask(":GR#") == "06:00:00#");
    CHECK(rig.client.ask(":GD#") == "+30*00:00#");
}

TEST_CASE("FakeZwoMount - land_short_by leaves the landed Dec short of the target", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.set_land_short_by(3600.0);  // one degree
    rig.client.ask(":Sr06:00:00#");
    rig.client.ask(":Sd+30*00:00#");
    rig.client.ask(":MS#");
    rig.clock.advance(5s);
    CHECK(rig.client.ask(":GR#") == "06:00:00#");
    CHECK(rig.client.ask(":GD#") == "+29*00:00#");
}

TEST_CASE("FakeZwoMount - Q freezes the position and clears slewing", "[zwo][fake][unit]") {
    Rig rig;
    rig.client.ask(":Sr06:00:00#");
    rig.client.ask(":Sd+30*00:00#");
    rig.client.ask(":MS#");
    rig.client.send(":Me#");
    rig.client.send(":Q#");  // blind: no reply
    CHECK(rig.client.ask(":GU#") == "NG#");
    CHECK_FALSE(rig.mount.jogging());  // ":Q" also ends a jog (fake contract; no rig evidence, see the fake's TODO)
    rig.clock.advance(60s);
    CHECK(rig.client.ask(":GR#") == "12:00:00#");  // the GOTO never landed
    CHECK(rig.client.ask(":GD#") == "+45*00:00#");
}

TEST_CASE("FakeZwoMount - hold_running keeps slewing past the slew duration", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.hold_running(true);
    rig.client.ask(":MS#");
    rig.clock.advance(3600s);
    CHECK(rig.client.ask(":GU#") == "G#");
    rig.mount.hold_running(false);
    CHECK(rig.client.ask(":GU#") == "NG#");
}

TEST_CASE("FakeZwoMount - hold_reply blocks the transaction until release", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.hold_reply(":Sr");
    auto reply = std::async(std::launch::async, [&] { return rig.client.ask(":Sr06:00:00#"); });
    REQUIRE(rig.mount.wait_until_held(5s));
    CHECK(rig.mount.count(":Sr") == 1);  // counted on receipt
    CHECK(reply.wait_for(100ms) == std::future_status::timeout);
    rig.mount.release();
    REQUIRE(reply.wait_for(5s) == std::future_status::ready);
    CHECK(reply.get() == "1#");
}

TEST_CASE("FakeZwoMount - reject_goto answers the reply the given number of times, then accepts", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.reject_goto("e3", 1);
    CHECK(rig.client.ask(":MS#") == "e3#");
    CHECK(rig.client.ask(":GU#") == "NG#");  // a rejected GOTO does not move the mount
    CHECK(rig.client.ask(":MS#") == "0#");
    CHECK(rig.client.ask(":GU#") == "G#");
    rig.mount.reject_goto("2");
    CHECK(rig.client.ask(":MS#") == "2#");
    CHECK(rig.client.ask(":MS#") == "2#");
}

TEST_CASE("FakeZwoMount - Gps follows park and unpark", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.set_slew_duration(20s);
    CHECK(rig.client.ask(":Gps#") == "0#");
    rig.client.send(":hP#");
    CHECK(rig.client.ask(":Gps#") == "1#");
    rig.clock.advance(20s);
    CHECK(rig.client.ask(":Gps#") == "2#");
    CHECK(rig.client.ask(":Spu#") == "1#");
    CHECK(rig.client.ask(":Gps#") == "0#");
}

TEST_CASE("FakeZwoMount - tracking and jog flags are reported in :GU", "[zwo][fake][unit]") {
    Rig rig;
    CHECK(rig.client.ask(":Td#") == "1#");
    CHECK(rig.client.ask(":GAT#") == "0#");
    CHECK(rig.client.ask(":GU#") == "nNG#");
    CHECK(rig.client.ask(":Te#") == "1#");
    rig.client.send(":Me#");
    CHECK(rig.client.ask(":GU#") == "G#");
    CHECK(rig.mount.jogging());
    rig.client.send(":Qe#");
    CHECK(rig.client.ask(":GU#") == "NG#");
}

TEST_CASE("FakeZwoMount - count matches what was sent, and a write replaces the time reply", "[zwo][fake][unit]") {
    Rig rig;
    rig.client.ask(":GR#");
    rig.client.ask(":GR#");
    rig.client.ask(":GD#");
    rig.client.send(":Mg0100#");
    CHECK(rig.client.ask(":SMTI10/21/2026&01:02:03&+00:00#") == "1#");
    CHECK(rig.client.ask(":GMTI#") == "10/21/2026&01:02:03&+00:00#");
    CHECK(rig.mount.count(":GR") == 2);
    CHECK(rig.mount.count(":GD") == 1);
    CHECK(rig.mount.count(":Mg") == 1);
    CHECK(rig.mount.count(":SMTI") == 1);
    CHECK(rig.mount.wait_for_count(":GR", 2, 1s));
    CHECK_FALSE(rig.mount.wait_for_count(":GR", 3, 50ms));
}

TEST_CASE("FakeZwoMount - commands the wrapper sends blind get no reply", "[zwo][fake][unit]") {
    Rig rig;
    // send_command_blind() in zwo_mount_protocol_wrapper.cpp: abort, tracking rates, move and guide rates, jogs,
    // guide pulses, home and park. A stray reply would be read as the next command's answer.
    const std::vector<std::string> blind = {":Q#",  ":TQ#", ":TL#",      ":TS#", ":Rv0001.00#", ":Rg0.50#", ":Me#",
                                            ":Qe#", ":Mw#", ":Qw#",      ":Mn#", ":Qn#",        ":Ms#",     ":Qs#",
                                            ":hC#", ":Q#",  ":Mgn0100#", ":hP#", ":Q#"};
    for (const auto& command : blind) {
        rig.client.send(command);
        CHECK(rig.client.ask(":GVP#") == "AM5N#");
    }
    CHECK(rig.mount.count(":GVP") == static_cast<int>(blind.size()));
}

TEST_CASE("FakeZwoMount - an unknown command gets no reply", "[zwo][fake][unit]") {
    Rig rig;
    rig.client.send(":Xyz#");
    CHECK(rig.client.ask(":GVP#") == "AM5N#");
    CHECK(rig.mount.count(":Xyz") == 1);
}

TEST_CASE("FakeZwoMount - :GU reports home, guide pulses and the fault and mode flags", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.set_slew_duration(10s);
    CHECK(rig.client.ask(":Gh#") == "0#");
    rig.client.send(":hC#");
    CHECK(rig.client.ask(":GU#") == "G#");  // homing: moving, not yet at home
    rig.clock.advance(10s);
    CHECK(rig.client.ask(":GU#") == "NHG#");
    CHECK(rig.mount.at_home());
    CHECK(rig.client.ask(":Gh#") == "1#");
    rig.client.send(":Mge0500#");
    rig.client.send(":Mgn0200#");
    CHECK(rig.client.ask(":GU#") == "NHTtG#");
    rig.clock.advance(200ms);
    CHECK(rig.client.ask(":GU#") == "NHTG#");  // the Dec pulse ended first
    rig.clock.advance(300ms);
    CHECK(rig.client.ask(":GU#") == "NHG#");
    CHECK(rig.client.ask(":MS#") == "0#");
    CHECK(rig.client.ask(":GU#") == "G#");  // a GOTO leaves home
    CHECK_FALSE(rig.mount.at_home());
    CHECK(rig.client.ask(":Gh#") == "1#");  // a completed home search stays reported
    rig.clock.advance(10s);
    rig.mount.set_low_power(true);
    rig.mount.set_stall(true, true);
    rig.mount.set_alt_az(true);
    CHECK(rig.client.ask(":GU#") == "NLSsZ#");
}

TEST_CASE("FakeZwoMount - rate commands set the state that :GT and :Ggr report", "[zwo][fake][unit]") {
    Rig rig;
    CHECK(rig.client.ask(":GT#") == "0#");
    CHECK(rig.client.ask(":Ggr#") == "0.50#");
    rig.client.send(":TS#");
    CHECK(rig.client.ask(":GT#") == "2#");
    rig.client.send(":TL#");
    CHECK(rig.client.ask(":GT#") == "1#");
    CHECK(rig.mount.tracking_rate() == 1);
    rig.client.send(":Rg0.30#");
    CHECK(rig.client.ask(":Ggr#") == "0.30#");
    CHECK(rig.mount.guide_rate() == "0.30");
    rig.client.send(":Rv0012.50#");
    CHECK(rig.client.ask(":GVP#") == "AM5N#");
    CHECK(rig.mount.move_rate() == 12.5);
}

TEST_CASE("FakeZwoMount - override_reply answers the given reply the given number of times", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.override_reply(":GR", "e2", 2);
    CHECK(rig.client.ask(":GR#") == "e2#");
    CHECK(rig.client.ask(":GD#") == "+45*00:00#");  // only the named prefix
    CHECK(rig.client.ask(":GR#") == "e2#");
    CHECK(rig.client.ask(":GR#") == "12:00:00#");
}

TEST_CASE("FakeZwoMount - override rules for different prefixes stack and go_silent withholds the reply",
          "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.override_reply(":GR", "bad", 1);
    rig.mount.override_reply(":GU", "e1");
    rig.mount.go_silent(":GD", 1);
    CHECK(rig.client.ask(":GU#") == "e1#");
    CHECK(rig.client.ask(":GR#") == "bad#");
    rig.client.send(":GD#");  // counted, no reply: the next reply on the wire is :GR's, not a stray one
    CHECK(rig.client.ask(":GR#") == "12:00:00#");
    CHECK(rig.mount.count(":GD") == 1);
    CHECK(rig.client.ask(":GD#") == "+45*00:00#");  // one shot, spent
    rig.mount.override_reply(":GU", "", 0);         // a count of 0 removes the rule
    CHECK(rig.client.ask(":GU#") == "NG#");
}

TEST_CASE("FakeZwoMount - set_moving reports motion with no GOTO until cleared", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.set_moving(true);
    CHECK(rig.client.ask(":GU#") == "G#");
    CHECK(rig.mount.slewing());
    rig.clock.advance(3600s);
    CHECK(rig.client.ask(":GU#") == "G#");
    rig.mount.set_moving(false);
    CHECK(rig.client.ask(":GU#") == "NG#");
}

TEST_CASE("FakeZwoMount - delay_next holds one reply and wait_for_delayed_reply returns once it is on the socket",
          "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.delay_next(":GR", 400ms);
    const auto start = std::chrono::steady_clock::now();
    auto reply = std::async(std::launch::async, [&] { return rig.client.ask(":GR#"); });
    REQUIRE(rig.mount.wait_for_count(":GR", 1, 5s));
    CHECK_FALSE(rig.mount.wait_for_delayed_reply(10ms));  // still inside the delay
    REQUIRE(rig.mount.wait_for_delayed_reply(5s));
    CHECK(std::chrono::steady_clock::now() - start >= 400ms);
    CHECK(reply.get() == "12:00:00#");
    const auto second = std::chrono::steady_clock::now();
    CHECK(rig.client.ask(":GR#") == "12:00:00#");
    CHECK(std::chrono::steady_clock::now() - second < 300ms);  // one shot: the delay is spent
}

TEST_CASE("FakeZwoMount - set_reply_delay holds every reply for the real-time delay", "[zwo][fake][unit]") {
    Rig rig;
    rig.mount.set_reply_delay(100ms);
    const auto start = std::chrono::steady_clock::now();
    CHECK(rig.client.ask(":GVP#") == "AM5N#");
    CHECK(std::chrono::steady_clock::now() - start >= 100ms);
    CHECK(rig.clock.now() == FakeTaskClock::clock::time_point{});  // real time only: the task clock did not move
    rig.mount.set_reply_delay(0ms);
    CHECK(rig.client.ask(":GVP#") == "AM5N#");
}

TEST_CASE("FakeZwoMount - drop_link and reset_connections close the link and a new connection is served",
          "[zwo][fake][unit]") {
    Rig rig;
    CHECK(rig.client.ask(":GVP#") == "AM5N#");
    rig.mount.drop_link();
    CHECK(rig.client.closed());
    Client second{rig.mount.port()};
    CHECK(second.ask(":GVP#") == "AM5N#");
    rig.mount.reset_connections();
    CHECK(second.closed());
    Client third{rig.mount.port()};
    CHECK(third.ask(":GVP#") == "AM5N#");
}

#endif  // !_WIN32

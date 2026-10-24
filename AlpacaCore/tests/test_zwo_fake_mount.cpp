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

#include <chrono>
#include <cstdint>
#include <future>
#include <string>

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
    rig.client.send(":Q#");  // blind: no reply
    CHECK(rig.client.ask(":GU#") == "NG#");
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

#endif  // !_WIN32

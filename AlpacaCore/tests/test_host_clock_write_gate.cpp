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

// util::HostClockWriteGate (open-astro#2480): the connect-time host clock decision the five mounts with a clock share.
// Driven through the fake clock hooks: the ClientUtcWarning probe seam and the HostClockTrust latch.

#include <alpacacore/util/host_clock_write_gate.h>

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

using alpacacore::logging::LogLevel;
using alpacacore::util::HostClockEvent;
using alpacacore::util::HostClockTrust;
using alpacacore::util::HostClockWriteGate;
using std::chrono::milliseconds;

namespace {

constexpr auto kUntrusted =
    "Host clock is not trusted (not NTP-synchronised, not set or confirmed by a client); leaving the mount clock alone "
    "until the host clock is set or a client writes UTCDate";

// Captures the "Gate" component's lines with their level, at DEBUG and up.
class GateLog {
public:
    GateLog() {
        alpacacore::logging::set_log_level(LogLevel::Debug);
        alpacacore::logging::set_log_sink([this](LogLevel level, std::string_view component, std::string_view message) {
            if (component != "Gate") return;
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.emplace_back(level, std::string(message));
        });
    }
    ~GateLog() {
        alpacacore::logging::set_log_sink(previous_sink_);
        alpacacore::logging::set_log_level(previous_level_);
    }
    GateLog(const GateLog&) = delete;
    GateLog& operator=(const GateLog&) = delete;

    std::vector<std::pair<LogLevel, std::string>> lines() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

private:
    alpacacore::logging::LogSink previous_sink_ = alpacacore::logging::get_log_sink();
    LogLevel previous_level_ = alpacacore::logging::get_log_level();
    std::mutex mutex_;
    std::vector<std::pair<LogLevel, std::string>> lines_;
};

}  // namespace

TEST_CASE("HostClockWriteGate - an untrusted host clock may not go into the mount, warned once per connect",
          "[hostclock][gate]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    GateLog log;
    HostClockWriteGate gate("Gate");
    gate.begin_connect();
    CHECK_FALSE(gate.may_write());
    CHECK_FALSE(gate.may_write());  // a redundant Connected=true or a second pass: DEBUG
    gate.begin_connect();
    CHECK_FALSE(gate.may_write("2017-01-01T12:00:42Z"));
    const auto lines = log.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == std::make_pair(LogLevel::Warn, std::string(kUntrusted)));
    CHECK(lines[1] == std::make_pair(LogLevel::Debug, std::string(kUntrusted)));
    CHECK(lines[2].first == LogLevel::Warn);
    CHECK(lines[2].second ==
          "Host clock is not trusted (not NTP-synchronised, not set or confirmed by a client); leaving the mount clock "
          "(2017-01-01T12:00:42Z) alone until the host clock is set or a client writes UTCDate");
}

TEST_CASE("HostClockWriteGate - a trusted host clock may go into the mount, and its decision spends the warning",
          "[hostclock][gate]") {
    alpacacore::test::HostClockTrustGuard synchronized(true);
    GateLog log;
    HostClockWriteGate gate("Gate");
    gate.begin_connect();
    CHECK(gate.may_write());
    CHECK(log.lines().empty());
    // The kernel loses its sync on the same connection: the later decision logs at DEBUG only.
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return false; });
    CHECK_FALSE(gate.may_write());
    const auto lines = log.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].first == LogLevel::Debug);
}

TEST_CASE("HostClockWriteGate - the latch trusts the host clock with no kernel sync", "[hostclock][gate]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    HostClockWriteGate gate("Gate");
    CHECK_FALSE(gate.may_write());
    HostClockTrust::mark_set("a client UTCDate write");
    CHECK(gate.may_write());
}

TEST_CASE("HostClockWriteGate - an owed write is taken once, and dropped by a connect", "[hostclock][gate]") {
    HostClockWriteGate gate("Gate");
    CHECK_FALSE(gate.owed());
    CHECK_FALSE(gate.take_owed());

    gate.owe(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(24507000)});
    CHECK(gate.owed());
    CHECK(gate.owed_why() == "Host clock stepped by 24507000 ms");
    CHECK(gate.take_owed());
    CHECK_FALSE(gate.owed());
    CHECK_FALSE(gate.take_owed());

    gate.owe(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    CHECK(gate.owed_why() == "Host clock became trusted");
    gate.drop_owed();
    CHECK_FALSE(gate.owed());
    CHECK_FALSE(gate.take_owed());
}

TEST_CASE("HostClockWriteGate - describe names the change", "[hostclock][gate]") {
    CHECK(HostClockWriteGate::describe(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(5000)}) ==
          "Host clock stepped by 5000 ms");
    CHECK(HostClockWriteGate::describe(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)}) ==
          "Host clock became trusted");
}

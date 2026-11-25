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

// Host clock trust at connect and the host clock re-sync (open-astro#2419 follow-up).
// The connect writes the host clock into the mount only while util::HostClockTrust::trusted(), and
// on_host_clock_changed() re-syncs it later, off the timer thread, never under driver motion, never over a client's
// UTCDate write (.github/instructions/ascom-contract.instructions.md, the mount clock rule). The driver talks to a
// loopback FakeMountServer and runs on a FakeTaskClock with its status poll paused, so the only waits on the clock are
// the re-sync's (and a MoveAxis re-send's), stepped with advance(). Each case sets the trust latch and the kernel
// probe through HostClockTrustGuard, which restores both.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace onstep = alpacacore::vendor::onstep;

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kRendezvous = milliseconds(5000);  // real time for a driver thread to reach the clock

// The mount's own clock (RC51's iOptron read the same after a power loss): 2017-01-01 12:00:42 UTC.
std::chrono::system_clock::time_point mount_power_up() {
    using namespace std::chrono;
    return sys_days{year{2017} / January / day{1}} + hours{12} + seconds{42};
}

// An OnStepX that answers what the driver asks and logs every command. `goto_running` is a GOTO the :MS# started.
struct ClockMount {
    std::mutex mutex;
    std::vector<std::string> log;
    std::atomic<bool> goto_running{false};

    std::size_t count(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(log.begin(), log.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }

    std::string answer(const std::string& chunk) {
        if (chunk.rfind(":GVP", 0) == 0) return "OnStep#";
        if (chunk.rfind(":GVN", 0) == 0) return "10.23a#";
        if (chunk.rfind(":GXEE", 0) == 0) return "1";
        if (chunk.rfind(":GU", 0) == 0) return goto_running.load() ? "pEW180#" : "NpEW180#";  // tracking
        if (chunk.rfind(":GRa", 0) == 0) return "12:00:00.0000#";
        if (chunk.rfind(":GDe", 0) == 0) return "+45*00:00.000#";
        if (chunk.rfind(":GA", 0) == 0) return "+45*00:00#";
        if (chunk.rfind(":GZ", 0) == 0) return "180*00:00#";
        if (chunk.rfind(":GL", 0) == 0) return "12:00:42#";
        if (chunk.rfind(":GC", 0) == 0) return "01/01/17#";
        if (chunk.rfind(":GG", 0) == 0) return "+00:00#";
        if (chunk.rfind(":Gt", 0) == 0) return "+40*30#";
        if (chunk.rfind(":Gg", 0) == 0) return "+074*00#";
        if (chunk.rfind(":Gh", 0) == 0) return "-10*#";
        if (chunk.rfind(":Go", 0) == 0) return "90*#";
        if (chunk.rfind(":GE", 0) == 0) return "00#";
        if (chunk.rfind(":MS", 0) == 0) {
            goto_running = true;
            return "0";  // GOTO accepted (bare digit)
        }
        if (chunk.rfind(":Q", 0) == 0 || chunk.rfind(":M", 0) == 0 || chunk.rfind(":R", 0) == 0) return "";  // blind
        if (chunk.rfind(":S", 0) == 0 || chunk.rfind(":h", 0) == 0) return "1";  // :St, :Sg, :SL, :SC, :SG, :ST ...
        return "0#";
    }

    std::string reply(const std::string& chunk) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(chunk);
        }
        return answer(chunk);
    }
};

// The OnStep log lines at Info and above, for the case's lifetime.
class OnStepLog {
public:
    OnStepLog() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel, std::string_view component, std::string_view message) {
                if (component != "OnStep") return;
                std::lock_guard<std::mutex> lock(mutex_);
                lines_.emplace_back(message);
            });
    }
    ~OnStepLog() { alpacacore::logging::set_log_sink(previous_); }
    OnStepLog(const OnStepLog&) = delete;
    OnStepLog& operator=(const OnStepLog&) = delete;
    bool contains(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(lines_.begin(), lines_.end(),
                           [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }

private:
    alpacacore::logging::LogSink previous_ = alpacacore::logging::get_log_sink();
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

// A rendezvous with a driver thread, bounded in real time.
template <typename Pred>
bool eventually(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct Rig {
    std::shared_ptr<ClockMount> mount = std::make_shared<ClockMount>();
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeMountServer server{[m = mount](const std::string& chunk) { return m->reply(chunk); }};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    // A site in the config, so the connect writes :St#/:Sg#; the poll is paused, so the frame is the connect's.
    explicit Rig(bool sync_time_on_connect = true) {
        onstep::ConnectionInfo info;
        info.type = onstep::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = 1000;
        driver =
            onstep::create_onstep_telescope_with_site(0, info, 40.5, -74.0, std::nullopt, sync_time_on_connect, clock);
        onstep::set_status_poll_paused_for_testing(*driver, true);
    }
    ~Rig() {
        try {
            driver->set_connected(false);
        } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort teardown
        }
        driver.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void connect() {
        REQUIRE(server.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
    }
    std::size_t time_writes() { return mount->count(":SL"); }
    bool resync_done() {
        return eventually([&] { return !onstep::clock_resync_running_for_testing(*driver); });
    }
};

const alpacacore::util::HostClockEvent kStepped{alpacacore::util::HostClockEvent::Kind::Stepped,
                                                milliseconds(24507000)};
const alpacacore::util::HostClockEvent kBecameTrusted{alpacacore::util::HostClockEvent::Kind::BecameTrusted,
                                                      milliseconds(0)};

bool near_host_now(std::chrono::system_clock::time_point utc) {
    const auto diff = utc - std::chrono::system_clock::now();
    return diff < seconds(5) && diff > -seconds(5);
}

}  // namespace

TEST_CASE("OnStep host clock - an untrusted connect sends no time write and keeps the site writes", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    CHECK(rig.mount->count(":St") == 1);
    CHECK(rig.mount->count(":Sg") == 1);
    CHECK(rig.mount->count(":SL") == 0);
    CHECK(rig.mount->count(":SC") == 0);
    CHECK(rig.mount->count(":SG") == 0);
    CHECK(log.contains("leaving the mount clock alone until the host clock is set or a client writes UTCDate"));
    // UTCDate is the mount's own clock, read at connect, not the stale host's.
    CHECK(rig.driver->get_utc_date() == mount_power_up());
}

TEST_CASE("OnStep host clock - a trusted connect writes the host time into the mount", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    OnStepLog log;
    Rig rig;
    rig.connect();
    CHECK(rig.mount->count(":St") == 1);
    CHECK(rig.mount->count(":SL") == 1);
    CHECK(rig.mount->count(":SC") == 1);
    CHECK(rig.mount->count(":SG") == 1);
    CHECK(rig.mount->count(":GL") == 0);  // the written time needs no read
    CHECK_FALSE(log.contains("Host clock is not trusted"));
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("OnStep host clock - a host clock that becomes trusted while connected re-syncs the mount clock",
          "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);

    // NTP slewed the clock in, or a client confirmed it: trusted, never stepped.
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(rig.resync_done());
    CHECK(rig.mount->count(":SL") == 1);
    CHECK(rig.mount->count(":SC") == 1);
    CHECK(rig.mount->count(":SG") == 1);
    CHECK(rig.mount->count(":St") == 1);  // the site is not re-sent
    CHECK(log.contains("Host clock became trusted; mount clock re-synced to the host clock"));
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("OnStep host clock - a host clock step while connected re-syncs the mount clock", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);

    // The router saw the step and latched trust before it called the driver; the kernel still says unsynchronised.
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.resync_done());
    CHECK(rig.time_writes() == 1);
    CHECK(log.contains("Host clock stepped by 24507000 ms; mount clock re-synced to the host clock"));
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("OnStep host clock - no re-sync runs while disconnected or untrusted or with sync off", "[onstep][clock]") {
    SECTION("time sync off") {
        alpacacore::test::HostClockTrustGuard trusted(true);
        Rig rig(/*sync_time_on_connect=*/false);
        rig.connect();
        rig.driver->on_host_clock_changed(kStepped);
        rig.driver->on_host_clock_changed(kBecameTrusted);
        CHECK_FALSE(onstep::clock_resync_running_for_testing(*rig.driver));
        CHECK(rig.time_writes() == 0);
    }
    SECTION("disconnected") {
        alpacacore::test::HostClockTrustGuard trusted(true);
        Rig rig;
        rig.driver->on_host_clock_changed(kStepped);
        CHECK_FALSE(onstep::clock_resync_running_for_testing(*rig.driver));
        rig.connect();  // the connect's own write, and nothing left over from the change before it
        CHECK(rig.resync_done());
        CHECK(rig.time_writes() == 1);
    }
    SECTION("host clock still not trusted") {
        alpacacore::test::HostClockTrustGuard untrusted(false);
        Rig rig;
        rig.connect();
        rig.driver->on_host_clock_changed(kStepped);  // a step the router did not latch trust for
        REQUIRE(rig.resync_done());
        CHECK(rig.time_writes() == 0);
        CHECK(rig.driver->get_utc_date() == mount_power_up());
    }
}

// A re-sync never moves the mount clock under a GOTO (OnStepX aims it by that clock): it waits, on the task clock,
// until the GOTO the driver started has ended, then writes.
TEST_CASE("OnStep host clock - a re-sync waits out a GOTO before it writes the mount clock", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    rig.driver->slew_to_coordinates_async(12.0, 45.0);
    REQUIRE(rig.mount->goto_running.load());

    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.clock.wait_for_waiters(1, kRendezvous));  // parked in its 1 s motion wait
    CHECK(log.contains("the mount is moving, the mount clock re-sync waits for the motion to end"));
    // Past the 2 s slew grace too: the frame still shows the GOTO, so nothing goes to the mount clock.
    for (int i = 0; i < 3; ++i) {
        rig.clock.advance(seconds(1));
        REQUIRE(rig.clock.wait_for_woken_settled(kRendezvous));
        REQUIRE(rig.clock.wait_for_waiters(1, kRendezvous));
        CHECK(rig.time_writes() == 0);
    }

    // The GOTO lands: a poll tick reads it, and the next look writes the clock.
    rig.mount->goto_running = false;
    REQUIRE(onstep::poll_status_now_for_testing(*rig.driver));
    REQUIRE_FALSE(rig.driver->get_slewing());
    rig.clock.advance(seconds(1));
    REQUIRE(rig.resync_done());
    CHECK(rig.time_writes() == 1);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

// The motion check runs inside the write's own exchange, under the operation lock every motion command takes: a
// MoveAxis that starts after the host clock change, before the re-sync reaches the mount, holds the write back until
// it ends.
TEST_CASE("OnStep host clock - a MoveAxis that starts after the host clock change gets no clock write until it ends",
          "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    std::atomic<bool> moved{false};
    onstep::set_clock_resync_before_lock_hook_for_testing(*rig.driver, [&] {
        if (!moved.exchange(true)) rig.driver->move_axis(0, 0.5);
    });

    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("re-sync waits for the motion to end"); }));
    CHECK(moved.load());
    REQUIRE(rig.clock.wait_for_waiters(2, kRendezvous));  // the re-sync and the MoveAxis re-send (7 s)
    CHECK(rig.time_writes() == 0);
    rig.clock.advance(seconds(1));
    REQUIRE(rig.clock.wait_for_woken_settled(kRendezvous));
    REQUIRE(rig.clock.wait_for_waiters(2, kRendezvous));
    CHECK(rig.time_writes() == 0);

    rig.driver->move_axis(0, 0.0);
    rig.clock.advance(seconds(1));
    REQUIRE(rig.resync_done());
    CHECK(rig.time_writes() == 1);
}

// A client UTCDate write that lands after the re-sync was decided is the newer word on the mount clock: the re-sync
// sends nothing, and UTCDate reads back the client's value.
TEST_CASE("OnStep host clock - a client UTCDate write after the host clock change wins", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    const auto client_utc = std::chrono::system_clock::now() + std::chrono::hours(1);
    onstep::set_clock_resync_before_lock_hook_for_testing(*rig.driver, [&] { rig.driver->set_utc_date(client_utc); });

    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.resync_done());
    CHECK(rig.time_writes() == 1);                    // the client's alone
    CHECK(rig.driver->get_utc_date() == client_utc);  // the fake clock has not moved
    CHECK(log.contains("a client wrote UTCDate since"));
}

// A disconnect drains the re-sync (a body waiting out motion is joined, and sends nothing), and the next connect
// starts clean: it writes the clock once, itself.
TEST_CASE("OnStep host clock - a disconnect drains a re-sync that waits out motion", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    rig.driver->move_axis(0, 0.5);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("re-sync waits for the motion to end"); }));

    rig.driver->set_connected(false);
    CHECK_FALSE(onstep::clock_resync_running_for_testing(*rig.driver));
    CHECK(rig.time_writes() == 0);

    rig.connect();  // trusted now (the latch): the connect writes the clock itself
    CHECK(rig.time_writes() == 1);
    rig.clock.advance(seconds(2));
    CHECK_FALSE(onstep::clock_resync_running_for_testing(*rig.driver));
    CHECK(rig.time_writes() == 1);
}

// A host clock change that reaches the timer hook as set_connected() returns, after the rest of the call but while it
// still holds the transition lock, fails the hook's try_lock and stays pending. set_connected() picks it up after it
// lets go of the lock; checked before the unlock, the change would sit pending with nobody to start the re-sync.
TEST_CASE("OnStep host clock - a change that arrives as set_connected() returns still re-syncs", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    OnStepLog log;
    Rig rig;
    std::atomic<bool> fired{false};
    onstep::set_connected_before_transition_unlock_hook_for_testing(*rig.driver, [&] {
        if (fired.exchange(true)) return;
        // The timer thread, while this call still holds the transition lock: its try_lock fails.
        std::thread timer([&] { rig.driver->on_host_clock_changed(kStepped); });
        timer.join();
    });
    rig.connect();
    onstep::set_connected_before_transition_unlock_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(fired.load());
    REQUIRE(rig.resync_done());
    CHECK(rig.time_writes() == 2);  // the connect's own, then the re-sync's
    CHECK(log.contains("Host clock stepped by 24507000 ms; mount clock re-synced to the host clock"));
}

// A re-sync gives up waiting out motion after util::kHostClockResyncMotionBound on the task clock, sends nothing,
// and says what sets the mount clock next.
TEST_CASE("OnStep host clock - a re-sync gives up after the shared motion bound", "[onstep][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    OnStepLog log;
    Rig rig;
    rig.connect();
    rig.driver->slew_to_coordinates_async(12.0, 45.0);
    REQUIRE(rig.mount->goto_running.load());

    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.clock.wait_for_waiters(1, kRendezvous));  // parked in its motion wait
    rig.clock.advance(alpacacore::util::kHostClockResyncMotionBound - alpacacore::util::kHostClockResyncMotionPoll);
    REQUIRE(rig.clock.wait_for_woken_settled(kRendezvous));
    REQUIRE(rig.clock.wait_for_waiters(1, kRendezvous));  // still waiting, one poll short of the bound
    CHECK(onstep::clock_resync_running_for_testing(*rig.driver));
    CHECK_FALSE(log.contains("the mount kept moving"));

    rig.clock.advance(alpacacore::util::kHostClockResyncMotionPoll);
    REQUIRE(rig.resync_done());
    CHECK(rig.time_writes() == 0);
    CHECK(
        log.contains("Host clock stepped by 24507000 ms; the mount kept moving, so its clock is left alone: the next "
                     "host clock change or a client UTCDate write sets the mount clock"));
}

#endif  // _WIN32

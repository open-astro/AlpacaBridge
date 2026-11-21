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
// A ZWO AM mount keeps its own clock, and syncTimeOnConnect defaults on, so the connect, a GOTO's setup and the
// re-sync write the host clock into it only while util::HostClockTrust::trusted(); a Pi with no RTC boots on the time
// of its last shutdown. Every case pins trust through HostClockTrustGuard (the ClientUtcWarning probe seam and a
// cleared latch, both restored on exit) and drives the re-sync through the driver's hook, FakeZwoMount's command log
// and FakeTaskClock; real time only bounds a rendezvous.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"

namespace {

using namespace std::chrono_literals;
using alpacacore::TelescopeDriver;
using alpacacore::test::HostClockTrustGuard;
using alpacacore::util::ClientUtcWarning;
using alpacacore::util::HostClockEvent;

constexpr auto kReal = 5s;  // real-time bound on a rendezvous, never a driver timer

// The mount clock FakeZwoMount answers to ":GMTI#" before any ":SMTI": 2026-01-02T03:04:05Z.
constexpr const char* kMountClockReply = "01/02/26&03:04:05&+00:00";
const auto kMountClock = std::chrono::sys_days{std::chrono::year{2026} / 1 / 2} + 3h + 4min + 5s;
// A client's UTCDate, far from the host clock: 2027-03-04T05:06:07Z.
const auto kClientUtc = std::chrono::sys_days{std::chrono::year{2027} / 3 / 4} + 5h + 6min + 7s;
constexpr const char* kClientTimeWrite = ":SMTI03/04/27&05:06:07&+00:00";

const HostClockEvent kBecameTrusted{HostClockEvent::Kind::BecameTrusted, 0ms};
const HostClockEvent kStepped{HostClockEvent::Kind::Stepped, 3600000ms};

void trust_host_clock() {
    ClientUtcWarning::set_host_synchronized_probe([] { return true; });
}

// Blocks a thread until released; stays released.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, kReal, [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeZwoMount mount{clock};
    std::unique_ptr<TelescopeDriver> driver;
    // Runs on the re-sync body's thread with each stage; set before the event that starts the body.
    std::function<void(std::string_view)> on_stage;
    std::mutex stage_mutex;
    std::condition_variable stage_cv;
    std::vector<std::string> stages;

    explicit Rig(std::optional<bool> sync_time = std::nullopt, int response_timeout_ms = 1000) {
        mount.override_reply(":Gps", "e1");  // AM5N: optional query unsupported
        mount.set_slew_duration(0ns);
        mount.set_time_reply(kMountClockReply);
        alpacacore::vendor::zwo::ConnectionInfo info;
        info.type = alpacacore::vendor::zwo::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = mount.port();
        info.response_timeout_ms = response_timeout_ms;
        driver = alpacacore::vendor::zwo::create_zwo_telescope_with_site(0, info, 30.0, -100.0, {}, sync_time, clock);
        alpacacore::vendor::zwo::set_clock_resync_hook_for_testing(*driver, [this](std::string_view stage) {
            if (on_stage) on_stage(stage);
            {
                std::lock_guard lock(stage_mutex);
                stages.emplace_back(stage);
            }
            stage_cv.notify_all();
        });
    }
    ~Rig() {
        mount.release();
        driver.reset();
    }
    void connect() {
        REQUIRE(mount.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        REQUIRE(clock.wait_for_waiters(1, kReal));  // the poll loop
    }
    int seen(std::string_view stage) {
        std::lock_guard lock(stage_mutex);
        return static_cast<int>(std::count(stages.begin(), stages.end(), stage));
    }
    bool wait_for_stage(std::string_view stage, int n = 1) {
        std::unique_lock lock(stage_mutex);
        return stage_cv.wait_for(
            lock, kReal, [&] { return static_cast<int>(std::count(stages.begin(), stages.end(), stage)) >= n; });
    }
    int time_writes() const { return mount.count(":SMTI"); }
    std::string last_time_write() const {
        const auto log = mount.commands();
        const auto it = std::find_if(log.rbegin(), log.rend(),
                                     [](const std::string& command) { return command.rfind(":SMTI", 0) == 0; });
        return it == log.rend() ? std::string() : *it;
    }
    // Index in the command log of the last command starting with @p prefix; -1 when there is none.
    long last_index_of(const std::string& prefix) const {
        const auto log = mount.commands();
        for (long i = static_cast<long>(log.size()) - 1; i >= 0; --i) {
            if (log[static_cast<std::size_t>(i)].rfind(prefix, 0) == 0) return i;
        }
        return -1;
    }
    // Steps virtual time until @p done, at most @p limit, letting every woken waiter finish its step.
    bool advance_until(const std::function<bool()>& done, std::chrono::nanoseconds step,
                       std::chrono::nanoseconds limit) {
        for (std::chrono::nanoseconds spent{0}; !done() && spent < limit; spent += step) {
            clock.advance(step);
            if (!clock.wait_for_woken_settled(kReal)) return false;
        }
        return done();
    }
};

std::chrono::system_clock::time_point today_from(const std::string& time_write) {
    // ":SMTIMM/DD/YY&HH:MM:SS&+00:00"
    int mo = 0, d = 0, y = 0, h = 0, mi = 0, s = 0;
    REQUIRE(std::sscanf(time_write.c_str(), ":SMTI%d/%d/%d&%d:%d:%d", &mo, &d, &y, &h, &mi, &s) == 6);
    return std::chrono::sys_days{std::chrono::year{2000 + y} / mo / d} + std::chrono::hours(h) +
           std::chrono::minutes(mi) + std::chrono::seconds(s);
}

// A ":GMTI#" payload for @p utc, "MM/DD/YY&HH:MM:SS&+00:00".
std::string time_reply_for(std::chrono::system_clock::time_point utc) {
    const auto day = std::chrono::floor<std::chrono::days>(utc);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::seconds>(utc - day)};
    char text[40];
    std::snprintf(text, sizeof text, "%02u/%02u/%02d&%02d:%02d:%02d&+00:00", static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()), static_cast<int>(ymd.year()) % 100,
                  static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                  static_cast<int>(hms.seconds().count()));
    return text;
}

// A one-shot flag a thread waits on (real time bounds the wait).
struct Signal {
    std::mutex mutex;
    std::condition_variable cv;
    bool set = false;
    void notify() {
        std::lock_guard lock(mutex);
        set = true;
        cv.notify_all();
    }
    bool wait() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, kReal, [&] { return set; });
    }
};

// The ZWO log lines, for the case's lifetime.
class ZwoLog {
public:
    ZwoLog() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel, std::string_view component, std::string_view message) {
                if (component != "ZWO") return;
                std::lock_guard<std::mutex> lock(mutex_);
                lines_.emplace_back(message);
            });
    }
    ~ZwoLog() { alpacacore::logging::set_log_sink(previous_); }
    ZwoLog(const ZwoLog&) = delete;
    ZwoLog& operator=(const ZwoLog&) = delete;
    int count(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [&](const std::string& line) {
            return line.find(text) != std::string::npos;
        }));
    }

private:
    alpacacore::logging::LogSink previous_ = alpacacore::logging::get_log_sink();
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

}  // namespace

TEST_CASE("ZWO host clock - an untrusted connect sends no time write and keeps the site writes",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    CHECK(rig.time_writes() == 0);
    CHECK(rig.mount.count(":SMGE") == 2);  // the configured site, then the time sync's site re-send
    // UTCDate is the mount's own clock, read at connect; the fake clock has not moved since.
    CHECK(rig.driver->get_utc_date() == kMountClock);

    // A redundant Connected=true (the Platform 7 handshake) leaves the mount clock alone too.
    rig.driver->set_connected(true);
    CHECK(rig.time_writes() == 0);
    CHECK(rig.mount.count(":SMGE") == 4);
}

TEST_CASE("ZWO host clock - a trusted connect writes the host time into the mount",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    rig.connect();
    const auto after = std::chrono::system_clock::now();
    CHECK(rig.time_writes() == 1);
    CHECK(rig.mount.count(":SMGE") == 2);
    const auto written = today_from(rig.last_time_write());
    CHECK(written >= before);
    CHECK(written <= after);
    CHECK(rig.driver->get_utc_date() >= before);
    CHECK(rig.driver->get_utc_date() <= after);
}

TEST_CASE("ZWO host clock - an untrusted GOTO keeps the mount clock and still sends the site",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    rig.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, kReal));
    CHECK(rig.time_writes() == 0);
    CHECK(rig.mount.count(":SMGE") == 3);  // the GOTO setup's site write
    CHECK(rig.driver->get_utc_date() == kMountClock);

    // Trusted, the next GOTO writes the host time as before.
    trust_host_clock();
    rig.driver->slew_to_coordinates_async(7.0, 21.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 2, kReal));
    CHECK(rig.time_writes() == 1);
}

TEST_CASE("ZWO host clock - becoming trusted while connected re-syncs the mount clock",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    trust_host_clock();
    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(rig.wait_for_stage("done"));
    const auto after = std::chrono::system_clock::now();
    CHECK(rig.seen("sent") == 1);
    CHECK(rig.time_writes() == 1);
    const auto written = today_from(rig.last_time_write());
    CHECK(written >= before);
    CHECK(written <= after);
    CHECK(rig.driver->get_utc_date() >= before);  // the anchor follows the re-sync
    CHECK(rig.driver->get_utc_date() <= after);
}

TEST_CASE("ZWO host clock - a host clock step while connected re-syncs the mount clock",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    rig.mount.set_time_reply(kMountClockReply);  // the mount clock as it was before the step
    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.wait_for_stage("done"));
    const auto after = std::chrono::system_clock::now();
    CHECK(rig.time_writes() == 2);
    const auto written = today_from(rig.last_time_write());
    CHECK(written >= before);
    CHECK(written <= after);
}

TEST_CASE("ZWO host clock - no re-sync with syncTimeOnConnect off", "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig(false);
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    rig.driver->on_host_clock_changed(kBecameTrusted);
    rig.driver->on_host_clock_changed(kStepped);
    rig.driver.reset();  // joins every slot, so a body that had started has finished
    CHECK(rig.seen("started") == 0);
    CHECK(rig.time_writes() == 0);
}

TEST_CASE("ZWO host clock - no re-sync while disconnected", "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    rig.driver->set_connected(false);
    rig.driver->on_host_clock_changed(kBecameTrusted);
    rig.driver->on_host_clock_changed(kStepped);
    rig.driver.reset();
    CHECK(rig.seen("started") == 0);
    CHECK(rig.time_writes() == 1);
}

TEST_CASE("ZWO host clock - no re-sync while the host clock is still untrusted", "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    rig.driver->on_host_clock_changed(kStepped);
    rig.driver.reset();
    CHECK(rig.seen("started") == 0);
    CHECK(rig.time_writes() == 0);
}

TEST_CASE("ZWO host clock - a re-sync waits out a MoveAxis jog before it writes the mount clock",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    rig.driver->move_axis(0, 1.0);
    REQUIRE(rig.mount.jogging());
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.wait_for_stage("waiting"));
    CHECK(rig.time_writes() == 1);  // nothing while the axis moves
    rig.driver->move_axis(0, 0.0);
    REQUIRE_FALSE(rig.mount.jogging());
    const long stop_at = rig.last_index_of(":Q");
    // The jog's 2 s Slewing window, then the next ":GU" that reports it stopped.
    REQUIRE(rig.advance_until([&] { return rig.time_writes() == 2; }, 250ms, 10s));
    CHECK(rig.last_index_of(":SMTI") > stop_at);
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("sent") == 1);
}

// A re-sync gives up waiting out motion after util::kHostClockResyncMotionBound on the task clock, sends nothing,
// and says the next host clock change tries again.
TEST_CASE("ZWO host clock - a re-sync gives up after the shared motion bound", "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    ZwoLog log;
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    std::mutex body_mutex;
    std::thread::id body;
    rig.on_stage = [&](std::string_view stage) {
        if (stage != "waiting") return;
        std::lock_guard lock(body_mutex);
        body = std::this_thread::get_id();
    };
    const auto body_thread = [&] {
        std::lock_guard lock(body_mutex);
        return body;
    };
    rig.driver->move_axis(0, 1.0);
    REQUIRE(rig.mount.jogging());
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.wait_for_stage("waiting"));
    REQUIRE(rig.clock.wait_for_waiter_thread(body_thread(), kReal));  // parked in its motion wait

    // Up to one poll short of the bound, in steps the status poll keeps answering through (a single 15 min jump
    // would trip the 30 s telemetry stall watchdog and drop the link instead); the body wakes and looks again each.
    const auto short_of_bound =
        alpacacore::util::kHostClockResyncMotionBound - alpacacore::util::kHostClockResyncMotionPoll;
    int looks = 1;
    for (std::chrono::nanoseconds spent{0}; spent < short_of_bound;) {
        const std::chrono::nanoseconds step = std::min<std::chrono::nanoseconds>(5s, short_of_bound - spent);
        rig.clock.advance(step);
        spent += step;
        ++looks;
        REQUIRE(rig.wait_for_stage("waiting", looks));  // looked again, still moving
        REQUIRE(rig.clock.wait_for_waiter_thread(body_thread(), kReal));
        REQUIRE(rig.clock.wait_for_woken_settled(kReal));
    }
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.seen("done") == 0);
    CHECK(log.count("the mount kept moving") == 0);
    CHECK(rig.time_writes() == 1);

    rig.clock.advance(alpacacore::util::kHostClockResyncMotionPoll);
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(log.count("the mount kept moving, so the mount clock was not re-synced") == 1);
    CHECK(rig.seen("sent") == 0);
    CHECK(rig.time_writes() == 1);
    rig.driver->move_axis(0, 0.0);
}

TEST_CASE("ZWO host clock - a disconnect drains a re-sync that is waiting out motion",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    rig.driver->move_axis(0, 1.0);
    REQUIRE(rig.mount.jogging());
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.wait_for_stage("waiting"));
    // The body sits in its 1 s wait on the task clock, which never moves here: only the disconnect's drain ends it.
    rig.driver->set_connected(false);
    CHECK(rig.seen("done") == 1);  // finished before Connected=false returned
    CHECK(rig.seen("sent") == 0);
    CHECK(rig.time_writes() == 1);
}

TEST_CASE("ZWO host clock - a link loss ends a re-sync that is waiting out motion",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    rig.driver->move_axis(0, 1.0);
    REQUIRE(rig.mount.jogging());
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.wait_for_stage("waiting"));
    // A failed exchange marks the transport lost; the next getter drops the link and cancels the re-sync, whose wait
    // on the still task clock only that cancel can end.
    REQUIRE(rig.mount.drop_link());
    CHECK_THROWS_AS(rig.driver->command_string(":GVP#", false), alpacacore::AlpacaException);
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK(rig.wait_for_stage("done"));
    CHECK(rig.seen("sent") == 0);
    CHECK(rig.time_writes() == 1);
}

TEST_CASE("ZWO host clock - a client UTCDate write before the re-sync's write wins",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    std::atomic<bool> wrote{false};
    rig.on_stage = [&](std::string_view stage) {
        if (stage == "decided" && !wrote.exchange(true)) rig.driver->set_utc_date(kClientUtc);
    };
    trust_host_clock();
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("sent") == 0);
    CHECK(rig.time_writes() == 1);  // the client's alone
    CHECK(rig.last_time_write() == kClientTimeWrite);
    CHECK(rig.driver->get_utc_date() == kClientUtc);
}

TEST_CASE("ZWO host clock - a client UTCDate write after the re-sync's write wins",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    std::atomic<bool> wrote{false};
    rig.on_stage = [&](std::string_view stage) {
        if (stage == "sent" && !wrote.exchange(true)) rig.driver->set_utc_date(kClientUtc);
    };
    trust_host_clock();
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.time_writes() == 2);  // the re-sync's, then the client's
    CHECK(rig.last_time_write() == kClientTimeWrite);
    CHECK(rig.driver->get_utc_date() == kClientUtc);  // the re-sync did not publish over it
}

// Falsified by: run_clock_resync without its `client_utc_session_ == utc_session_` return (zwo_telescope_driver.cpp):
// it reads the generation after the client's write and sends the host time over the client's UTCDate (#2286).
TEST_CASE("ZWO host clock - a host clock step after a client UTCDate write on this connection sends no re-sync",
          "[zwo][telescope][clock][utc][issue2419][issue2286]") {
    HostClockTrustGuard trusted(true);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);  // the connect's host time
    rig.driver->set_utc_date(kClientUtc);
    REQUIRE(rig.time_writes() == 2);
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("decided") == 0);
    CHECK(rig.seen("sent") == 0);
    CHECK(rig.time_writes() == 2);  // the client's write is still the last
    CHECK(rig.last_time_write() == kClientTimeWrite);
    CHECK(rig.driver->get_utc_date() == kClientUtc);  // virtual time has not moved
}

// Falsified by: the same deleted return; the trust edge would write the host time over the client's UTCDate.
TEST_CASE("ZWO host clock - becoming trusted after a client UTCDate write on this connection sends no re-sync",
          "[zwo][telescope][clock][utc][issue2419][issue2286]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    rig.driver->set_utc_date(kClientUtc);
    REQUIRE(rig.time_writes() == 1);
    trust_host_clock();
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("decided") == 0);
    CHECK(rig.seen("sent") == 0);
    CHECK(rig.time_writes() == 1);  // the client's alone
    CHECK(rig.last_time_write() == kClientTimeWrite);
    CHECK(rig.driver->get_utc_date() == kClientUtc);
}

TEST_CASE("ZWO host clock - a change seen during a connect re-syncs once the connect returns",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig(std::nullopt, 5000);
    REQUIRE(rig.mount.ok());
    // The connect has decided not to write the time (untrusted) and reads the mount clock, holding
    // set_connected_mutex_, when the host clock becomes trusted.
    rig.mount.hold_reply(":GMTI");
    auto connecting = std::async(std::launch::async, [&] { rig.driver->set_connected(true); });
    REQUIRE(rig.mount.wait_until_held(kReal));
    trust_host_clock();
    rig.driver->on_host_clock_changed(kBecameTrusted);
    rig.mount.release();
    connecting.get();
    REQUIRE(rig.driver->get_connected());
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("sent") == 1);
    CHECK(rig.time_writes() == 1);
}

TEST_CASE("ZWO host clock - a change seen during a disconnect is not inherited by the next connect",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Gate gate;  // outlives the driver, whose destructor may start a teardown through the spawn below
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    // The disconnect holds set_connected_mutex_ while it starts its teardown body.
    alpacacore::vendor::zwo::set_teardown_spawn_for_testing(*rig.driver, [&](std::function<void()> fn) {
        gate.block();
        return std::thread(std::move(fn));
    });
    auto disconnecting = std::async(std::launch::async, [&] { rig.driver->set_connected(false); });
    REQUIRE(gate.wait_entered());
    rig.driver->on_host_clock_changed(kStepped);
    gate.release();
    disconnecting.get();
    REQUIRE_FALSE(rig.driver->get_connected());
    rig.connect();
    CHECK(rig.time_writes() == 2);  // the reconnect's own write
    rig.driver.reset();
    CHECK(rig.seen("started") == 0);
    CHECK(rig.time_writes() == 2);
}

TEST_CASE("ZWO host clock - a change the hook defers just as a connect returns still re-syncs",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig(std::nullopt, 5000);
    REQUIRE(rig.mount.ok());
    // The connect holds set_connected_mutex_ while it reads the mount clock (untrusted: no time write).
    rig.mount.hold_reply(":GMTI");
    auto connecting = std::async(std::launch::async, [&] { rig.driver->set_connected(true); });
    REQUIRE(rig.mount.wait_until_held(kReal));
    trust_host_clock();
    // The hook's try_lock fails; it sets its pending flag only after the connect has looked for it and returned.
    Signal deferred;  // on_stage runs before the Rig records the stage
    Signal connect_returned;
    std::atomic<bool> deferred_waited{false};
    rig.on_stage = [&](std::string_view stage) {
        if (stage != "deferred" || deferred_waited.exchange(true)) return;
        deferred.notify();
        (void)connect_returned.wait();
    };
    auto hook = std::async(std::launch::async, [&] { rig.driver->on_host_clock_changed(kBecameTrusted); });
    REQUIRE(deferred.wait());
    rig.mount.release();
    connecting.get();
    REQUIRE(rig.driver->get_connected());
    REQUIRE(rig.time_writes() == 0);
    connect_returned.notify();
    hook.get();
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("sent") == 1);
    CHECK(rig.time_writes() == 1);
}

TEST_CASE("ZWO host clock - a change the hook defers just as a disconnect returns is not inherited by the next connect",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Gate gate;  // outlives the driver, whose destructor may start a teardown through the spawn below
    Rig rig;
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    // The disconnect holds set_connected_mutex_ while it starts its teardown body.
    alpacacore::vendor::zwo::set_teardown_spawn_for_testing(*rig.driver, [&](std::function<void()> fn) {
        gate.block();
        return std::thread(std::move(fn));
    });
    auto disconnecting = std::async(std::launch::async, [&] { rig.driver->set_connected(false); });
    REQUIRE(gate.wait_entered());
    // The hook's try_lock fails; it sets its pending flag only after the disconnect has looked for it and returned.
    Signal deferred;  // on_stage runs before the Rig records the stage
    Signal disconnect_returned;
    std::atomic<bool> deferred_waited{false};
    rig.on_stage = [&](std::string_view stage) {
        if (stage != "deferred" || deferred_waited.exchange(true)) return;
        deferred.notify();
        (void)disconnect_returned.wait();
    };
    auto hook = std::async(std::launch::async, [&] { rig.driver->on_host_clock_changed(kStepped); });
    REQUIRE(deferred.wait());
    gate.release();
    disconnecting.get();
    REQUIRE_FALSE(rig.driver->get_connected());
    disconnect_returned.notify();
    hook.get();
    rig.on_stage = nullptr;
    rig.connect();
    CHECK(rig.time_writes() == 2);  // the reconnect's own write
    rig.driver.reset();
    CHECK(rig.seen("started") == 0);
    CHECK(rig.time_writes() == 2);
}

TEST_CASE("ZWO host clock - a change left pending behind a newer connect is that connect's to cover",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Gate gate;  // outlives the driver, whose destructor may start a teardown through the spawn below
    Rig rig(std::nullopt, 5000);
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    alpacacore::vendor::zwo::set_teardown_spawn_for_testing(*rig.driver, [&](std::function<void()> fn) {
        gate.block();
        return std::thread(std::move(fn));
    });
    auto disconnecting = std::async(std::launch::async, [&] { rig.driver->set_connected(false); });
    REQUIRE(gate.wait_entered());
    // The disconnect holds the transition lock, so the hook leaves its flag (its own pick-up fails too).
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(rig.seen("deferred") == 1);
    // The disconnect has released the lock and not yet looked for the flag when a connect takes the lock: the
    // disconnect's pick-up fails, so the flag outlives it into the connect, which writes the time itself (held here).
    std::future<void> connecting;
    std::atomic<bool> armed{true};
    rig.on_stage = [&](std::string_view stage) {
        if (stage != "released" || !armed.exchange(false)) return;
        rig.mount.hold_reply(":SMTI");
        connecting = std::async(std::launch::async, [&] { rig.driver->set_connected(true); });
        (void)rig.mount.wait_until_held(kReal);
    };
    gate.release();
    disconnecting.get();
    REQUIRE(connecting.valid());
    REQUIRE(rig.mount.wait_until_held(kReal));
    rig.mount.release();
    connecting.get();
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.time_writes() == 2);  // the connect's own write
    rig.driver.reset();
    CHECK(rig.seen("started") == 0);
    CHECK(rig.time_writes() == 2);
}

TEST_CASE("ZWO host clock - a pulse that starts while the re-sync waits for the link gets no clock write under it",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard trusted(true);
    Rig rig(std::nullopt, 5000);
    rig.connect();
    REQUIRE(rig.time_writes() == 1);
    // A client command holds the I/O lock, so the re-sync's exchange queues behind it.
    rig.mount.hold_reply(":GVP");
    auto client = std::async(std::launch::async, [&] { return rig.driver->command_string(":GVP#", false); });
    REQUIRE(rig.mount.wait_until_held(kReal));
    rig.driver->on_host_clock_changed(kStepped);
    // Past every check before the exchange; the pulse marks itself before its ":Mg" waits for the I/O lock too.
    REQUIRE(rig.wait_for_stage("exchange"));
    rig.driver->pulse_guide(0, 500);
    REQUIRE(rig.driver->get_is_pulse_guiding());
    rig.mount.release();
    (void)client.get();
    // The check inside the exchange sees the pulse: the re-sync waits instead of writing.
    REQUIRE(rig.wait_for_stage("waiting"));
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, kReal));
    CHECK(rig.time_writes() == 1);
    // The pulse, its 500 ms hold, then the re-sync's next look.
    REQUIRE(rig.advance_until([&] { return rig.time_writes() == 2; }, 250ms, 10s));
    CHECK(rig.last_index_of(":SMTI") > rig.last_index_of(":Mg"));
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    REQUIRE(rig.wait_for_stage("done"));
    CHECK(rig.seen("sent") == 1);
}

TEST_CASE("ZWO host clock - SiderealTime and the pier sides follow the mount clock an untrusted connect left alone",
          "[zwo][telescope][clock][issue2419]") {
    HostClockTrustGuard untrusted(false);
    Rig rig;
    // The mount clock is 12 h behind the host's, so local sidereal time by either clock is about 12 h apart.
    const auto mount_clock = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - 12h;
    rig.mount.set_time_reply(time_reply_for(mount_clock));
    rig.connect();
    REQUIRE(rig.time_writes() == 0);
    REQUIRE(rig.driver->get_utc_date() == mount_clock);  // the fake task clock has not moved since the read
    const double longitude = rig.driver->get_site_longitude();
    const double mount_lst = alpacacore::util::local_sidereal_hours(mount_clock, longitude);
    const double host_lst = alpacacore::util::local_sidereal_hours(std::chrono::system_clock::now(), longitude);
    REQUIRE(std::abs(alpacacore::util::wrap_hour_angle(host_lst - mount_lst)) > 11.0);

    CHECK(std::abs(alpacacore::util::wrap_hour_angle(rig.driver->get_sidereal_time() - mount_lst)) < 1e-6);
    // Hour angle +3 h by the mount clock is west of the meridian (pierEast); by the host clock it would be -9 h.
    const double west_ra = std::fmod(mount_lst - 3.0 + 24.0, 24.0);
    CHECK(rig.driver->get_destination_side_of_pier(west_ra, 20.0) == 0);
    const double east_ra = std::fmod(mount_lst + 3.0, 24.0);
    CHECK(rig.driver->get_destination_side_of_pier(east_ra, 20.0) == 1);
    const double ra = rig.driver->get_right_ascension();
    CHECK(rig.driver->get_side_of_pier() ==
          alpacacore::util::pier_side_for_hour_angle(alpacacore::util::wrap_hour_angle(mount_lst - ra)));
}

#endif  // _WIN32

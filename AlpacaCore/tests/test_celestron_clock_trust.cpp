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
//
// Lessons from iOptron's review rounds (#2419, #2432) applied to the NexStar hand-controller driver: an untrusted
// host clock leaves the mount clock alone at connect; a step or the trust edge re-syncs it off the timer thread; the
// re-sync never writes under a driver motion, checked inside the write's own exchange; a client UTCDate write after
// the decision wins; a disconnect drains the slot. The mount is a TCP fake whose replies are counted per command.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

#ifndef _WIN32
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace cel = alpacacore::vendor::celestron;
using alpacacore::test::FakeTaskClock;
using alpacacore::util::HostClockEvent;
using Clock = std::chrono::system_clock;

const HostClockEvent kStepped{HostClockEvent::Kind::Stepped, std::chrono::milliseconds(5000)};
const HostClockEvent kBecameTrusted{HostClockEvent::Kind::BecameTrusted, std::chrono::milliseconds(0)};
// RC51's mount on 2026-11-19 read 6 h 48 min behind the real time: the fake mount's own clock runs that far behind
// the host, so a UTCDate that reads the host time proves the re-sync wrote it.
constexpr std::chrono::seconds kMountBehind{6 * 3600 + 48 * 60};

// The NexStar hand controller, enough for a connect, a MoveAxis and the time commands.
struct ClockMount {
    std::atomic<int> time_writes{0};        // "H"
    std::atomic<int> time_reads{0};         // "h"
    std::atomic<int> site_writes{0};        // "W"
    std::atomic<int> transactions{0};       // every command
    std::atomic<bool> h_mute{false};        // the time write goes unanswered
    std::atomic<bool> p_mute{false};        // the pier-side read goes unanswered
    std::atomic<bool> w_hold{false};        // the location read blocks on w_latch (the connect's last read)
    std::atomic<bool> goto_held{false};     // a GOTO stays in progress until this clears
    std::atomic<bool> aligned{false};       // "J": the hand controller holds a star alignment
    std::atomic<bool> j_mute{false};        // "J" goes unanswered
    std::atomic<bool> j_garbled{false};     // "J" answers with a byte that is neither 0 nor 1
    std::atomic<int> alignment_queries{0};  // "J"
    alpacacore::test::Latch w_latch;
};

std::string clock_reply(ClockMount& st, const std::string& chunk) {
    st.transactions.fetch_add(1);
    if (chunk.empty()) return "0#";
    switch (chunk[0]) {
        case 'm':  // CGX-L: German equatorial
            return std::string(1, static_cast<char>(24)) + "#";
        case 'V':
            return std::string("\x05\x23#", 3);
        case 'w':
            if (st.w_hold.load()) st.w_latch.block();
            return std::string(8, '\0') + "#";
        case 'W':
            st.site_writes.fetch_add(1);
            return "#";
        case 'e':
        case 'E':
            return "12AB0500,20000500#";
        case 'z':
        case 'Z':
            return "20AB0500,10000500#";
        case 'r':
        case 'R':
        case 'b':
        case 'B':  // GOTO
            return "#";
        case 'L':  // GOTO in progress
            return st.goto_held.load() ? "1#" : "0#";
        case 'J':
            st.alignment_queries.fetch_add(1);
            if (st.j_mute.load()) return "";
            if (st.j_garbled.load()) return "?#";
            return st.aligned.load() ? "1#" : "0#";
        case 'p':
            if (st.p_mute.load()) return "";
            return "W#";
        case 't':
            return std::string("\x02#");
        case 'h': {  // the mount's own clock, kMountBehind behind the host, zone 0
            st.time_reads.fetch_add(1);
            const std::time_t mount = Clock::to_time_t(Clock::now() - kMountBehind);
            std::tm utc{};
            gmtime_r(&mount, &utc);
            std::string reply;
            for (const int field :
                 {utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_mon + 1, utc.tm_mday, utc.tm_year % 100, 0, 0}) {
                reply.push_back(static_cast<char>(field));
            }
            return reply + "#";
        }
        case 'H':
            st.time_writes.fetch_add(1);
            if (st.h_mute.load()) return "";
            return "#";
        case 'P': {  // AUX passthrough: P len dev op ...
            const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
            const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
            if (op == 0xFE && device == 0x32) return std::string("\x01\x00#", 3);  // the autoguider port
            if (op == 0xFE) return std::string("\x00\x00#", 3);                    // no other bus accessories
            if (op == 0x26) return "#";                                            // MC_AUX_GUIDE
            if (op == 0x02 || op == 0x17) return "#";                              // MC_GOTO_FAST / SLOW
            if (op == 0x13) return st.goto_held.load() ? std::string("\x00#", 2) : std::string("\xFF#");
            return std::string("\xFF#");
        }
        default:
            return "#";
    }
}

template <typename Pred>
bool eventually(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// The Celestron log lines at Info and above, for the case's lifetime.
class CelestronLog {
public:
    CelestronLog() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (component != "Celestron") return;
                std::lock_guard<std::mutex> lock(mutex_);
                lines_.push_back({level, std::string(message)});
            });
    }
    ~CelestronLog() { alpacacore::logging::set_log_sink(previous_); }
    CelestronLog(const CelestronLog&) = delete;
    CelestronLog& operator=(const CelestronLog&) = delete;
    int count(const std::string& text, std::optional<alpacacore::logging::LogLevel> level = std::nullopt) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [&](const Line& line) {
            return (!level || line.level == *level) && line.text.find(text) != std::string::npos;
        }));
    }
    bool contains(const std::string& text) { return count(text) > 0; }

private:
    struct Line {
        alpacacore::logging::LogLevel level;
        std::string text;
    };
    alpacacore::logging::LogSink previous_ = alpacacore::logging::get_log_sink();
    std::mutex mutex_;
    std::vector<Line> lines_;
};

constexpr auto kUntrustedWarn = "leaving the mount clock alone until the host clock is set or a client writes UTCDate";
constexpr auto kWaitsForMotion = "the mount clock re-sync waits for the motion to end";
constexpr auto kAlignedLeftAlone =
    "the hand controller is star-aligned, so the mount clock re-sync leaves its clock alone";
constexpr auto kAlignmentQueryFailed = "the hand controller's alignment query (J) failed";

// A driver on the fake mount and a task clock. Members are destroyed in reverse: the driver (whose re-sync body
// calls the step hook) goes before the counters and the server.
struct ClockRig {
    FakeTaskClock clock;
    std::shared_ptr<ClockMount> st = std::make_shared<ClockMount>();
    alpacacore::test::FakeMountServer server{[st = st](const std::string& c) { return clock_reply(*st, c); }};
    std::atomic<int> decided{0};
    std::atomic<int> written{0};
    std::atomic<int> finished{0};
    std::mutex step_mutex;
    std::function<void(cel::ClockResyncStep)> on_step;  // the case's own action at a step; guarded by step_mutex
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit ClockRig(bool sync_time = true, int response_timeout_ms = 1000) {
        REQUIRE(server.ok());
        cel::ConnectionInfo info;
        info.type = cel::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = response_timeout_ms;
        driver = cel::create_celestron_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, sync_time,
                                                           cel::CelestronAlignmentSetting::Auto, clock);
        cel::set_clock_resync_hook_for_testing(*driver, [this](cel::ClockResyncStep step) {
            std::function<void(cel::ClockResyncStep)> action;
            {
                std::lock_guard<std::mutex> lock(step_mutex);
                action = on_step;
            }
            if (action) action(step);
            if (step == cel::ClockResyncStep::Decided) ++decided;
            if (step == cel::ClockResyncStep::Written) ++written;
            if (step == cel::ClockResyncStep::Finished) ++finished;
        });
    }
    ~ClockRig() {
        if (driver) driver->set_connected(false);  // the wrapper is a process-wide singleton
    }
    ClockRig(const ClockRig&) = delete;
    ClockRig& operator=(const ClockRig&) = delete;

    void at_step(std::function<void(cel::ClockResyncStep)> action) {
        std::lock_guard<std::mutex> lock(step_mutex);
        on_step = std::move(action);
    }
    // Connects and parks the poll body on the clock, so only the case's own calls reach the mount.
    void connect() {
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        cel::set_status_poll_paused_for_testing(*driver, true);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    }
    void advance(std::chrono::nanoseconds by) {
        clock.advance(by);
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    }
    // Steps the task clock a quarter second at a time, once a body is parked on it, until `pred` holds.
    template <typename Pred>
    bool advance_until(Pred pred) {
        for (int step = 0; step < 400; ++step) {
            if (pred()) return true;
            (void)clock.wait_for_waiters(1, std::chrono::milliseconds(100));
            clock.advance(std::chrono::milliseconds(250));
            (void)clock.wait_for_woken_settled(std::chrono::seconds(5));
        }
        return pred();
    }
};

// An RA one hour west of the meridian at the rig's longitude: the GOTO needs no meridian flip.
double west_of_meridian_ra_hours() {
    const double days = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() / 86400.0;
    const double d = 2440587.5 + days - 2451545.0;
    const double lst_degrees =
        std::fmod(std::fmod(280.46061837 + 360.98564736629 * d + 151.215297, 360.0) + 360.0, 360.0);
    return std::fmod(lst_degrees / 15.0 - 1.0 + 24.0, 24.0);
}

std::chrono::seconds distance(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::seconds>(a > b ? a - b : b - a);
}

}  // namespace

TEST_CASE("Celestron host clock - an untrusted connect sends no time write and keeps the site write",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    CHECK(rig.st->time_writes.load() == 0);
    CHECK(rig.st->site_writes.load() == 1);  // the configured site still goes out
    CHECK(rig.st->time_reads.load() >= 1);   // and UTCDate is seeded from the mount's own clock
    CHECK(log.count(kUntrustedWarn, alpacacore::logging::LogLevel::Warn) == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now() - kMountBehind) < std::chrono::seconds(5));
}

TEST_CASE("Celestron host clock - a trusted connect writes the host time into the mount",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(rig.st->site_writes.load() == 1);
    CHECK(log.count(kUntrustedWarn) == 0);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
}

TEST_CASE("Celestron host clock - a host clock that becomes trusted while connected re-syncs the mount clock",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 0);
    REQUIRE(distance(rig.driver->get_utc_date(), Clock::now() - kMountBehind) < std::chrono::seconds(5));

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    CHECK(log.contains("Host clock became trusted; the mount clock was set from the host clock"));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a host clock step while connected re-syncs the mount clock",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 1);

    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->alignment_queries.load() == 1);  // the re-sync asked, and an unaligned hand controller gets "H"
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(log.contains("Host clock stepped by 5000 ms; the mount clock was set from the host clock"));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a trusted connect to an aligned hand controller still writes the host time",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    ClockRig rig;
    rig.st->aligned = true;
    rig.connect();
    CHECK(rig.st->time_writes.load() == 1);  // only the re-sync, which no client asked for, skips an aligned one
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
}

TEST_CASE("Celestron host clock - a re-sync leaves an aligned hand controller's clock alone",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.st->aligned = true;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 0);

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->alignment_queries.load() == 1);
    CHECK(rig.st->time_writes.load() == 0);
    CHECK(rig.written.load() == 0);
    // Nothing published: UTCDate still reads the mount's own clock.
    CHECK(distance(rig.driver->get_utc_date(), Clock::now() - kMountBehind) < std::chrono::seconds(5));
    CHECK(log.count(kAlignedLeftAlone, alpacacore::logging::LogLevel::Info) == 1);

    // A client's UTCDate write still goes to the aligned hand controller.
    const auto client = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now() + std::chrono::hours(3));
    rig.driver->set_utc_date(client);
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), client) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a re-sync whose alignment query fails writes nothing",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig(true, 300);
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 0);
    SECTION("J goes unanswered") { rig.st->j_mute = true; }
    SECTION("J answers with a byte that is neither 0 nor 1") { rig.st->j_garbled = true; }

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->alignment_queries.load() == 1);
    CHECK(rig.st->time_writes.load() == 0);
    CHECK(rig.written.load() == 0);
    CHECK(log.count(kAlignmentQueryFailed, alpacacore::logging::LogLevel::Warn) == 1);
    CHECK(rig.driver->get_connected());
    CHECK(distance(rig.driver->get_utc_date(), Clock::now() - kMountBehind) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a host clock change starts nothing while disconnected, untrusted or with sync off",
          "[celestron][clock][telescope][unit]") {
    SECTION("never connected") {
        alpacacore::test::HostClockTrustGuard trusted(true);
        ClockRig rig;
        rig.driver->on_host_clock_changed(kStepped);
        rig.driver->on_host_clock_changed(kBecameTrusted);
        rig.driver.reset();  // the destructor joins any body
        CHECK(rig.finished.load() == 0);
        CHECK(rig.st->transactions.load() == 0);
    }
    SECTION("after a disconnect") {
        alpacacore::test::HostClockTrustGuard trusted(true);
        ClockRig rig;
        rig.connect();
        rig.driver->set_connected(false);
        const int writes = rig.st->time_writes.load();
        rig.driver->on_host_clock_changed(kStepped);
        rig.driver.reset();
        CHECK(rig.finished.load() == 0);
        CHECK(rig.st->time_writes.load() == writes);
    }
    SECTION("syncTimeOnConnect off") {
        alpacacore::test::HostClockTrustGuard trusted(true);
        ClockRig rig(false);
        rig.connect();
        rig.driver->on_host_clock_changed(kStepped);
        rig.driver->on_host_clock_changed(kBecameTrusted);
        rig.driver->set_connected(false);  // joins any body
        CHECK(rig.finished.load() == 0);
        CHECK(rig.st->time_writes.load() == 0);
    }
    SECTION("host clock still untrusted") {
        alpacacore::test::HostClockTrustGuard untrusted(false);
        ClockRig rig;
        rig.connect();
        rig.driver->on_host_clock_changed(kStepped);
        rig.driver->set_connected(false);
        CHECK(rig.finished.load() == 0);
        CHECK(rig.st->time_writes.load() == 0);
    }
}

TEST_CASE("Celestron host clock - a re-sync waits out a MoveAxis and writes once it ends",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    rig.driver->move_axis(0, 1.0);

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // the paused poll and the re-sync
    CHECK(rig.st->time_writes.load() == 0);
    rig.advance(std::chrono::seconds(1));  // still moving: it checks again and waits again
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    CHECK(rig.st->time_writes.load() == 0);

    rig.driver->move_axis(0, 0.0);
    rig.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a re-sync waiting out a MoveAxis asks the alignment only for its write",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    rig.driver->move_axis(0, 1.0);
    const auto queries_before = rig.st->alignment_queries.load();

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    for (int poll = 0; poll < 3; ++poll) {
        REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // the paused poll and the re-sync
        rig.advance(std::chrono::seconds(1));                             // still moving: no wire turn
    }
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    CHECK(rig.st->alignment_queries.load() == queries_before);

    rig.driver->move_axis(0, 0.0);
    rig.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->alignment_queries.load() == queries_before + 1);
    CHECK(rig.st->time_writes.load() == 1);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a MoveAxis that starts after the re-sync was decided gets no clock write",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    // The race window: the mount was still when the host clock changed, and a client's MoveAxis lands before the
    // re-sync's write.
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step == cel::ClockResyncStep::Decided) rig.driver->move_axis(1, 2.0);
    });

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion) || rig.finished.load() == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    CHECK(rig.st->time_writes.load() == 0);

    rig.driver->move_axis(1, 0.0);
    rig.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a MoveAxis that starts just before the re-sync's exchange gets no clock write",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    // The motion lands after any check the body could make before its exchange: only one inside it sees it.
    std::atomic<bool> moved{false};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step == cel::ClockResyncStep::Exchange && !moved.exchange(true)) rig.driver->move_axis(1, 2.0);
    });

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion) || rig.finished.load() == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    CHECK(rig.st->time_writes.load() == 0);

    rig.driver->move_axis(1, 0.0);
    rig.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a re-sync waits out a GOTO and writes once it lands",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    rig.st->goto_held = true;
    rig.st->aligned = true;  // the slew-safety gate
    rig.driver->slew_to_coordinates_async(west_of_meridian_ra_hours(), -40.0);
    REQUIRE(rig.driver->get_slewing());
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // the slew body is past its J read
    rig.st->aligned = false;  // the re-sync leaves an aligned hand controller alone

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    REQUIRE(rig.clock.wait_for_waiters(3, std::chrono::seconds(5)));  // the paused poll, the slew and the re-sync
    CHECK(rig.st->time_writes.load() == 0);
    for (int step = 0; step < 8; ++step) rig.advance(std::chrono::milliseconds(250));  // 2 s, still under way
    CHECK(rig.driver->get_slewing());
    CHECK(rig.st->time_writes.load() == 0);

    rig.st->goto_held = false;
    REQUIRE(rig.advance_until([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

// The re-sync's motion check reads the driver's slewing flag as last set, without asking the mount. SynScan's V3 GOTO
// left it true once no client polled Slewing, which held every later re-sync; here the async body clears it at the
// landing, so a GOTO that landed unobserved holds nothing.
TEST_CASE("Celestron host clock - a re-sync after a GOTO that landed with no client polling Slewing writes at once",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 1);  // the trusted connect's own write
    rig.st->goto_held = true;
    rig.st->aligned = true;  // the slew-safety gate
    rig.driver->slew_to_coordinates_async(west_of_meridian_ra_hours(), -40.0);
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // the paused poll and the slew body
    for (int step = 0; step < 4; ++step) rig.advance(std::chrono::milliseconds(250));  // the body sees it under way

    // The GOTO lands; the body reads it and ends. Nothing in the case reads Slewing.
    rig.st->goto_held = false;
    bool landed = false;
    for (int step = 0; step < 80 && !landed; ++step) {
        rig.advance(std::chrono::milliseconds(250));
        landed = rig.clock.waiter_count() == 1;  // settled: the body exited, only the paused poll is left
    }
    REQUIRE(landed);
    rig.st->aligned = false;  // the re-sync leaves an aligned hand controller alone

    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(log.count(kWaitsForMotion) == 0);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a re-sync waits out a PulseGuide and writes once it ends",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.st->time_writes.load() == 1);
    rig.driver->pulse_guide(0, 2000);  // one hardware chunk: only the pulse window marks it
    REQUIRE(rig.driver->get_is_pulse_guiding());

    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    rig.advance(std::chrono::seconds(1));
    rig.advance(std::chrono::seconds(1));  // 2 s in: the pulse and its completion delay are still running
    CHECK(rig.st->time_writes.load() == 1);

    REQUIRE(rig.advance_until([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 2);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    rig.driver->set_connected(false);
}

TEST_CASE(
    "Celestron host clock - a host clock change while a PulseGuide's first write is going out gets no clock "
    "write until the pulse ends",
    "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.st->time_writes.load() == 1);
    // The guide write is on the wire and its pulse state not yet published: the re-sync runs to its exchange then.
    std::atomic<bool> fired{false};
    std::atomic<bool> waited{false};
    std::atomic<int> writes_meanwhile{-1};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step != cel::ClockResyncStep::PulseSent || fired.exchange(true)) return;
        rig.driver->on_host_clock_changed(kStepped);
        waited = eventually([&] { return log.contains(kWaitsForMotion) || rig.finished.load() == 1; });
        writes_meanwhile = rig.st->time_writes.load();
    });
    rig.driver->pulse_guide(0, 2000);
    REQUIRE(fired.load());
    CHECK(waited.load());
    CHECK(writes_meanwhile.load() == 1);
    CHECK(log.contains(kWaitsForMotion));

    REQUIRE(rig.advance_until([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 2);
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a client UTCDate write still waiting for the wire when the re-sync is decided wins",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    const auto client = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now() + std::chrono::hours(3)) +
                        std::chrono::milliseconds(250);
    // The client's write is decided but not yet on the wire (as behind a status poll's exchange) when the host clock
    // changes; the re-sync's exchange comes after the client's.
    std::atomic<bool> client_done{false};
    std::atomic<bool> body_waited{false};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step == cel::ClockResyncStep::ClientWriting) {
            alpacacore::util::HostClockTrust::mark_set("test");
            rig.driver->on_host_clock_changed(kBecameTrusted);
        } else if (step == cel::ClockResyncStep::Exchange && !body_waited.exchange(true)) {
            (void)eventually([&] { return client_done.load(); });
        }
    });

    rig.driver->set_utc_date(client);
    client_done = true;
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);  // the client's alone
    CHECK(rig.written.load() == 0);
    CHECK(distance(rig.driver->get_utc_date(), client) < std::chrono::seconds(5));
    CHECK(log.contains("leaves the mount clock to the client"));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE(
    "Celestron host clock - a client UTCDate write that publishes after a later re-sync write leaves UTCDate "
    "on the mount's clock",
    "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    ClockRig rig;
    rig.connect();
    const auto client = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now() + std::chrono::hours(3)) +
                        std::chrono::milliseconds(250);
    // The client's write is on the wire first; the host clock changes and the whole re-sync runs before the client
    // publishes. The mount holds the host time, so UTCDate must too.
    std::atomic<bool> body_done{false};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step != cel::ClockResyncStep::ClientWritten) return;
        alpacacore::util::HostClockTrust::mark_set("test");
        rig.driver->on_host_clock_changed(kBecameTrusted);
        body_done = eventually([&] { return rig.finished.load() == 1; });
    });

    rig.driver->set_utc_date(client);
    REQUIRE(body_done.load());
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(rig.written.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a host clock change as a set_connected() call leaves still re-syncs the mount clock",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 0);
    // The change comes from the timer thread while the call's exit holds the gate, after it found nothing pending.
    std::atomic<bool> fired{false};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step != cel::ClockResyncStep::ExitChecked || fired.exchange(true)) return;
        std::thread timer([&] {
            alpacacore::util::HostClockTrust::mark_set("test");
            rig.driver->on_host_clock_changed(kBecameTrusted);
        });
        timer.join();
    });

    rig.driver->set_connected(true);  // already connected: returns at once
    REQUIRE(fired.load());
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a host clock change as a set_connected() call takes the gate still re-syncs",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 0);
    // The trust edge comes from the timer thread right after a redundant Connected=true took the gate: the hook's
    // try_lock fails and it leaves the change pending, which only that call's exit can act on.
    std::atomic<bool> fired{false};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step != cel::ClockResyncStep::Gated || fired.exchange(true)) return;
        std::thread timer([&] {
            alpacacore::util::HostClockTrust::mark_set("test");
            rig.driver->on_host_clock_changed(kBecameTrusted);
        });
        timer.join();
    });

    rig.driver->set_connected(true);  // already connected: returns at once
    REQUIRE(fired.load());
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a re-sync superseded after its write still publishes it for UTCDate",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(distance(rig.driver->get_utc_date(), Clock::now() - kMountBehind) < std::chrono::seconds(5));
    // The first body's write is on the mount; before it publishes, a MoveAxis starts and the next host clock change
    // supersedes it with a body that waits that motion out.
    std::atomic<bool> fired{false};
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step != cel::ClockResyncStep::Written || fired.exchange(true)) return;
        rig.driver->move_axis(0, 1.0);
        std::thread timer([&] { rig.driver->on_host_clock_changed(kStepped); });
        timer.join();
    });

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));  // the first body; the second is waiting
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));  // the mount's clock

    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // the paused poll and the second body
    rig.driver->move_axis(0, 0.0);
    rig.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.finished.load() == 2; }));
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a re-sync leaves the mount clock alone when a motion outlasts its bound",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    rig.driver->move_axis(0, 1.0);

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    rig.clock.advance(std::chrono::minutes(15));
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 0);
    CHECK(log.count("the mount was still moving after 15 min", alpacacore::logging::LogLevel::Warn) == 1);
    rig.driver->move_axis(0, 0.0);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a client UTCDate write before the re-sync's write wins on the mount and in UTCDate",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    const auto client = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now() + std::chrono::hours(3)) +
                        std::chrono::milliseconds(250);
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step == cel::ClockResyncStep::Decided) rig.driver->set_utc_date(client);
    });

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);  // the client's alone
    CHECK(rig.written.load() == 0);
    CHECK(distance(rig.driver->get_utc_date(), client) < std::chrono::seconds(5));
    CHECK(log.contains("leaves the mount clock to the client"));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a client UTCDate write after the re-sync's write keeps its value in UTCDate",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    ClockRig rig;
    rig.connect();
    const auto client = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now() + std::chrono::hours(3)) +
                        std::chrono::milliseconds(250);
    // Later on the wire than the re-sync's write, and published before the re-sync publishes.
    rig.at_step([&](cel::ClockResyncStep step) {
        if (step == cel::ClockResyncStep::Written) rig.driver->set_utc_date(client);
    });

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(distance(rig.driver->get_utc_date(), client) < std::chrono::seconds(5));
    rig.at_step(nullptr);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a disconnect ends a waiting re-sync and a reconnect inherits none",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    rig.driver->move_axis(0, 1.0);
    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains(kWaitsForMotion); }));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));

    rig.driver->set_connected(false);  // joins the waiting body
    CHECK(rig.finished.load() == 1);
    CHECK(rig.st->time_writes.load() == 0);

    rig.connect();  // trusted now: the connect writes the clock itself
    CHECK(rig.st->time_writes.load() == 1);
    rig.advance(std::chrono::seconds(2));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(rig.finished.load() == 1);
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a host clock change during a held connect re-syncs as that connect returns",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    ClockRig rig(true, 3000);
    rig.st->w_hold = true;  // the connect's location read, after its time-write decision
    std::thread connecting([&] { rig.driver->set_connected(true); });
    REQUIRE(rig.st->w_latch.wait_entered());

    alpacacore::util::HostClockTrust::mark_set("test");
    const auto took = alpacacore::test::timed([&] { rig.driver->on_host_clock_changed(kBecameTrusted); });
    CHECK(took < std::chrono::milliseconds(500));  // it did not wait for the connect
    rig.st->w_latch.release();
    connecting.join();
    REQUIRE(rig.driver->get_connected());

    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(distance(rig.driver->get_utc_date(), Clock::now()) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

TEST_CASE("Celestron host clock - a failed re-sync write is logged and leaves Connected and the link fault alone",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    CelestronLog log;
    ClockRig rig(true, 300);
    rig.connect();
    rig.st->h_mute = true;

    alpacacore::util::HostClockTrust::mark_set("test");
    rig.driver->on_host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 1);
    CHECK(log.count("the mount clock re-sync failed", alpacacore::logging::LogLevel::Warn) == 1);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(distance(rig.driver->get_utc_date(), Clock::now() - kMountBehind) < std::chrono::seconds(5));
    rig.driver->set_connected(false);
}

// A blocking SlewToCoordinates publishes Slewing before its pier read and its GOTO. When either throws, the slew is
// stopped and its flags cleared, as an async body's failure does: a stale slewing_cached_ or slew_force_until_ held
// every later re-sync for its whole bound and then gave up.
TEST_CASE("Celestron host clock - a blocking SlewToCoordinates whose dispatch throws holds no later re-sync",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig(true, 300);
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 1);  // the trusted connect's own write
    rig.st->aligned = true;                    // the slew-safety gate
    rig.st->p_mute = true;                     // the dispatch's pier read times out, after Slewing was published
    CHECK_THROWS(rig.driver->slew_to_coordinates(west_of_meridian_ra_hours(), -40.0));
    rig.st->p_mute = false;
    rig.st->aligned = false;  // the re-sync leaves an aligned hand controller alone
    // The failure is kept for Slewing, as the async body's is, until the next motion command.
    CHECK_THROWS(rig.driver->get_slewing());

    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(log.count(kWaitsForMotion) == 0);
    rig.driver->set_connected(false);
}

// The blocking wait's 120 s timeout is the same failure: it stops the mount and clears Slewing on an answered stop.
TEST_CASE("Celestron host clock - a blocking SlewToCoordinates that times out holds no later re-sync",
          "[celestron][clock][telescope][unit]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    CelestronLog log;
    ClockRig rig;
    rig.connect();
    REQUIRE(rig.st->time_writes.load() == 1);
    rig.st->goto_held = true;  // the GOTO never lands
    rig.st->aligned = true;    // the slew-safety gate
    std::atomic<bool> done{false};
    std::string error;
    std::thread slewing([&] {
        try {
            rig.driver->slew_to_coordinates(west_of_meridian_ra_hours(), -40.0);
        } catch (const std::exception& e) {
            error = e.what();
        }
        done = true;
    });
    for (int step = 0; step < 200 && !done.load(); ++step) {
        (void)rig.clock.wait_for_waiters(2, std::chrono::milliseconds(200));  // the paused poll and the wait
        rig.clock.advance(std::chrono::seconds(1));
        (void)rig.clock.wait_for_woken_settled(std::chrono::seconds(5));
    }
    REQUIRE(eventually([&] { return done.load(); }));
    slewing.join();
    rig.st->aligned = false;  // the re-sync leaves an aligned hand controller alone
    CHECK(error.find("Slew timed out") != std::string::npos);
    CHECK_THROWS(rig.driver->get_slewing());  // the failure is kept for Slewing

    rig.driver->on_host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return rig.finished.load() == 1; }));
    CHECK(rig.st->time_writes.load() == 2);
    CHECK(log.count(kWaitsForMotion) == 0);
    rig.driver->set_connected(false);
}

#endif  // _WIN32

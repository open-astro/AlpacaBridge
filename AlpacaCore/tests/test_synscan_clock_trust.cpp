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
// A SynScan hand controller keeps its own clock and aims every GOTO by it, so with syncTimeOnConnect on the driver
// writes the host clock into it (`H`) only while util::HostClockTrust::trusted(), and writes it again from
// on_host_clock_changed() once the host clock is trusted or steps. These cases force the trust answer through the
// ClientUtcWarning probe seam and wait on the driver's task clock (FakeTaskClock), never on a sleep.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
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
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace sc = alpacacore::vendor::synscan;
using alpacacore::util::HostClockEvent;
using std::chrono::milliseconds;

constexpr milliseconds kBound{5000};

// Forces HostClockTrust::trusted() through the kernel probe seam and clears the latch, on entry and on exit.
class TrustGuard {
public:
    explicit TrustGuard(bool synchronized) : synchronized_(std::make_shared<std::atomic<bool>>(synchronized)) {
        alpacacore::util::HostClockTrust::reset_for_testing();
        auto flag = synchronized_;
        alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([flag] { return flag->load(); });
    }
    ~TrustGuard() {
        alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr);
        alpacacore::util::HostClockTrust::reset_for_testing();
    }
    TrustGuard(const TrustGuard&) = delete;
    TrustGuard& operator=(const TrustGuard&) = delete;
    void set(bool synchronized) { synchronized_->store(synchronized); }

private:
    std::shared_ptr<std::atomic<bool>> synchronized_;
};

// Captures the SynScan log lines; remembers the thread of the re-sync's "waits for the motion" line.
class LogCapture {
public:
    LogCapture() {
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Info);
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (component != "SynScan") return;
                std::lock_guard<std::mutex> lock(mutex_);
                lines_.push_back({level, std::string(message)});
                if (message.find("re-sync waits for the motion to end") != std::string_view::npos) {
                    waiter_ = std::this_thread::get_id();
                    cv_.notify_all();
                }
            });
    }
    ~LogCapture() {
        alpacacore::logging::set_log_sink(previous_sink_);
        alpacacore::logging::set_log_level(previous_level_);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    int count(alpacacore::logging::LogLevel level, std::string_view text) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [&](const auto& line) {
            return line.first == level && line.second.find(text) != std::string::npos;
        }));
    }
    std::optional<std::thread::id> wait_for_motion_waiter(milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, bound, [this] { return waiter_.has_value(); });
        return waiter_;
    }

private:
    alpacacore::logging::LogSink previous_sink_ = alpacacore::logging::get_log_sink();
    alpacacore::logging::LogLevel previous_level_ = alpacacore::logging::get_log_level();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::pair<alpacacore::logging::LogLevel, std::string>> lines_;
    std::optional<std::thread::id> waiter_;
};

// The handset: records every command, answers the connect's queries, and reports the mount clock 2020-01-02
// 03:04:05 UTC (zone 0, no DST) to `h`. A 'w' (site read) can be held until released, and so can the next 'p' (pier
// side read) once armed. `L` answers goto_in_progress (a GOTO started from the keypad, which the driver never sent).
struct FakeHandset {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> commands;
    std::atomic<bool> goto_in_progress{false};
    std::atomic<char> alignment_reply{'0'};  // the `J` byte: '0' not aligned, '\x01' aligned, anything else garbled
    bool hold_site_read = false;
    bool site_read_entered = false;
    bool hold_next_pier_read = false;
    bool pier_read_held = false;

    int count(char prefix) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(),
                                              [prefix](const std::string& c) { return !c.empty() && c[0] == prefix; }));
    }
    void release_site_read() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            hold_site_read = false;
        }
        cv.notify_all();
    }
    bool wait_site_read_entered(milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, bound, [this] { return site_read_entered; });
    }
    void hold_next_pier_side_read() {
        std::lock_guard<std::mutex> lock(mutex);
        hold_next_pier_read = true;
    }
    bool wait_pier_read_held(milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, bound, [this] { return pier_read_held; });
    }
    void release_pier_read() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pier_read_held = false;
        }
        cv.notify_all();
    }

    alpacacore::test::FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) -> std::string {
            if (chunk.empty()) return "0#";
            {
                std::unique_lock<std::mutex> lock(mutex);
                commands.push_back(chunk);
                if (chunk[0] == 'w') {
                    site_read_entered = true;
                    cv.notify_all();
                    cv.wait(lock, [this] { return !hold_site_read; });
                }
                if (chunk[0] == 'p' && hold_next_pier_read) {
                    hold_next_pier_read = false;
                    pier_read_held = true;
                    cv.notify_all();
                    cv.wait(lock, [this] { return !pier_read_held; });
                }
            }
            switch (chunk[0]) {
                case 'K':
                    return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
                case 'e':
                case 'z':
                    return "12AB0500,20000500#";
                case 'E':  // the V3 (16-bit) position reads
                case 'Z':
                    return "1234,4000#";
                case 'L':
                    return goto_in_progress.load() ? "1#" : "0#";
                case 'J':
                    return std::string(1, alignment_reply.load()) + "#";
                case 'h':
                    return std::string{3, 4, 5, 1, 2, 20, 0, 0} + "#";
                case 'w':
                    return std::string(8, '\0') + "#";
                case 'm':
                    return std::string(1, static_cast<char>(50)) + "#";
                case 't':
                    return std::string(1, '\x02') + "#";
                case 'H':
                case 'R':  // the V3 GOTO
                case 'W':
                case 'P':
                case 'M':
                case 'T':
                    return "#";
                default:
                    return "0#";
            }
        };
    }
};

// Each re-sync body's end, with whether it wrote the mount clock.
class ResyncDone {
public:
    void record(bool wrote) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_.push_back(wrote);
        }
        cv_.notify_all();
    }
    std::optional<bool> wait(std::size_t n, milliseconds bound) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, bound, [&] { return results_.size() >= n; })) return std::nullopt;
        return results_[n - 1];
    }
    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return results_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<bool> results_;
};

const auto kMountClock = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + std::chrono::hours(3) +
                         std::chrono::minutes(4) + std::chrono::seconds(5);

bool near_host_now(std::chrono::system_clock::time_point utc) {
    const auto diff = utc - std::chrono::system_clock::now();
    return diff < std::chrono::seconds(30) && diff > -std::chrono::seconds(30);
}

// A fake clock, the handset and the driver, built in that order and destroyed in reverse (the driver first).
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    FakeHandset handset;
    alpacacore::test::FakeMountServer server{handset.responder()};
    ResyncDone done;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit Rig(bool sync_time_on_connect, sc::SynScanVersion version = sc::SynScanVersion::V4) {
        REQUIRE(server.ok());
        sc::ConnectionInfo conn;
        conn.type = sc::ConnectionType::Network;
        conn.host = "127.0.0.1";
        conn.tcp_port = server.port();
        conn.response_timeout_ms = 5000;
        driver = sc::create_synscan_telescope_with_site(0, conn, version, -33.5, 151.0, std::nullopt,
                                                        sync_time_on_connect, sc::SynScanAlignmentSetting::Auto, clock);
        sc::set_clock_resync_done_hook_for_testing(*driver, [this](bool wrote) { done.record(wrote); });
    }
    ~Rig() {
        handset.release_site_read();
        handset.release_pier_read();
        driver->set_connected(false);
        driver.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void connect() {
        REQUIRE(alpacacore::test::connect_and_park(*driver, clock, kBound));
        REQUIRE(driver->get_connected());
        sc::set_status_poll_paused_for_testing(*driver, true);
    }
};

}  // namespace

TEST_CASE("SynScan host clock - an untrusted connect leaves the mount clock alone and keeps the site write",
          "[synscan][clock]") {
    TrustGuard trust(false);
    LogCapture log;
    Rig rig(true);
    rig.connect();

    CHECK(rig.handset.count('H') == 0);  // no time write
    CHECK(rig.handset.count('W') == 1);  // the configured site still goes out
    CHECK(rig.handset.count('h') == 1);  // and the mount clock is still read for UTCDate
    CHECK(rig.driver->get_utc_date() == kMountClock);
    CHECK(log.count(alpacacore::logging::LogLevel::Warn,
                    "leaving the mount clock alone until the host clock is set or a client writes UTCDate") == 1);
}

TEST_CASE("SynScan host clock - a trusted connect writes the host clock into the mount", "[synscan][clock]") {
    TrustGuard trust(true);
    Rig rig(true);
    rig.connect();

    CHECK(rig.handset.count('H') == 1);
    CHECK(rig.handset.count('W') == 1);
    CHECK(rig.handset.count('h') == 0);  // the write established UTC; no sample
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("SynScan host clock - the host clock becoming trusted while connected re-syncs the mount clock",
          "[synscan][clock]") {
    TrustGuard trust(false);
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 0);
    REQUIRE(rig.driver->get_utc_date() == kMountClock);

    trust.set(true);
    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('H') == 1);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("SynScan host clock - a host clock step while connected re-syncs the mount clock", "[synscan][clock]") {
    TrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(5000)});
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('J') == 1);  // asked, and the handset said it is not aligned
    CHECK(rig.handset.count('H') == 2);
    CHECK(log.count(alpacacore::logging::LogLevel::Info,
                    "Host clock stepped by 5000 ms; the mount clock was set from the host clock") == 1);
}

// A star-aligned hand controller aims by a pointing model built on its clock: the re-sync asks `J` in its own wire
// turn and leaves the clock alone. The connect still writes it (the session starts there).
TEST_CASE("SynScan host clock - a trusted connect to an aligned handset still writes the mount clock",
          "[synscan][clock]") {
    alpacacore::test::HostClockTrustGuard trust(true);
    Rig rig(true);
    rig.handset.alignment_reply.store('\x01');
    rig.connect();

    CHECK(rig.handset.count('H') == 1);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("SynScan host clock - a re-sync leaves an aligned handset's clock alone", "[synscan][clock]") {
    alpacacore::test::HostClockTrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);
    rig.handset.alignment_reply.store('\x01');

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(5000)});
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(false));
    CHECK(rig.handset.count('J') == 1);
    CHECK(rig.handset.count('H') == 1);  // no re-sync write
    CHECK(log.count(alpacacore::logging::LogLevel::Info,
                    "Host clock stepped by 5000 ms; the hand controller is star aligned, so the re-sync leaves its "
                    "clock alone; a client UTCDate write or a reconnect still sets it") == 1);
    CHECK(log.count(alpacacore::logging::LogLevel::Info, "the mount clock was set from the host clock") == 0);

    // A client UTCDate write still sets an aligned handset's clock.
    const auto client = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(client);
    CHECK(rig.handset.count('H') == 2);
}

TEST_CASE("SynScan host clock - a re-sync whose alignment query fails writes nothing", "[synscan][clock]") {
    alpacacore::test::HostClockTrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);
    rig.handset.alignment_reply.store('X');  // neither aligned nor not aligned: no answer

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(false));
    CHECK(rig.handset.count('J') == 1);
    CHECK(rig.handset.count('H') == 1);
    CHECK(log.count(alpacacore::logging::LogLevel::Warn,
                    "Host clock became trusted; the hand controller's alignment query failed, so the re-sync leaves "
                    "the mount clock alone: Invalid reply to SynScan alignment query") == 1);
}

TEST_CASE("SynScan host clock - no re-sync with syncTimeOnConnect off", "[synscan][clock]") {
    TrustGuard trust(true);
    Rig rig(false);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 0);

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(-4000)});
    CHECK(sc::clock_resync_starts_for_testing(*rig.driver) == 0);  // started on the calling thread, or never
    CHECK(rig.handset.count('H') == 0);
}

TEST_CASE("SynScan host clock - no re-sync while disconnected", "[synscan][clock]") {
    TrustGuard trust(true);
    Rig rig(true);

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(5000)});
    CHECK(sc::clock_resync_starts_for_testing(*rig.driver) == 0);

    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);  // the connect's own write; no re-sync inherited from before it
    rig.driver->set_connected(false);
    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    CHECK(sc::clock_resync_starts_for_testing(*rig.driver) == 0);
    CHECK(rig.handset.count('H') == 1);
}

TEST_CASE("SynScan host clock - a host clock change while it is still untrusted writes nothing", "[synscan][clock]") {
    TrustGuard trust(false);
    Rig rig(true);
    rig.connect();

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(5000)});
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(false));
    CHECK(rig.handset.count('H') == 0);
    CHECK(rig.driver->get_utc_date() == kMountClock);
}

TEST_CASE("SynScan host clock - a re-sync waits for a MoveAxis to end before it writes the mount clock",
          "[synscan][clock]") {
    TrustGuard trust(false);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    rig.driver->move_axis(0, 0.5);
    REQUIRE(rig.driver->get_slewing());

    trust.set(true);
    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    const auto body = log.wait_for_motion_waiter(kBound);
    REQUIRE(body.has_value());
    REQUIRE(rig.clock.wait_for_waiter_thread(*body, kBound));  // parked in its motion wait
    CHECK(rig.handset.count('H') == 0);
    CHECK(rig.done.size() == 0);

    rig.driver->move_axis(0, 0.0);
    REQUIRE_FALSE(rig.driver->get_slewing());
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('H') == 1);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

// The re-sync's first motion check runs before its wire turn and can find nothing: a PulseGuide holds the turn from
// its pier side read to its rate command and sets its pulse flags only then. The check at the turn is what keeps
// the `H` out of the pulse.
TEST_CASE("SynScan host clock - a re-sync queued behind a PulseGuide's wire turn waits for the pulse",
          "[synscan][clock]") {
    TrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);

    std::mutex queued_mutex;
    std::condition_variable queued_cv;
    int queued = 0;
    struct ClearTurnHook {  // before the hook's captures die, on every path
        alpacacore::TelescopeDriver& driver;
        ~ClearTurnHook() { sc::set_wire_turn_wait_hook_for_testing(driver, nullptr); }
    } clear_turn_hook{*rig.driver};
    sc::set_wire_turn_wait_hook_for_testing(*rig.driver, [&] {
        {
            std::lock_guard<std::mutex> lock(queued_mutex);
            ++queued;
        }
        queued_cv.notify_all();
    });
    rig.handset.hold_next_pier_side_read();
    std::exception_ptr pulse_error;
    std::thread pulse([&] {
        try {
            rig.driver->pulse_guide(0, 500);  // North: the Dec axis, which reads the pier side
        } catch (...) {
            pulse_error = std::current_exception();
        }
    });
    struct JoinPulse {
        FakeHandset& handset;
        std::thread& thread;
        ~JoinPulse() {
            handset.release_pier_read();
            if (thread.joinable()) thread.join();
        }
    } join_pulse{rig.handset, pulse};
    REQUIRE(rig.handset.wait_pier_read_held(kBound));  // the pulse holds the wire turn here
    const int queued_by_pulse = [&] {
        std::lock_guard<std::mutex> lock(queued_mutex);
        return queued;
    }();

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(2000)});
    {
        // The re-sync found no motion yet and took a ticket behind the pulse.
        std::unique_lock<std::mutex> lock(queued_mutex);
        REQUIRE(queued_cv.wait_for(lock, kBound, [&] { return queued > queued_by_pulse; }));
    }
    sc::set_wire_turn_wait_hook_for_testing(*rig.driver, nullptr);
    rig.handset.release_pier_read();
    pulse.join();
    REQUIRE_FALSE(pulse_error);

    const auto body = log.wait_for_motion_waiter(kBound);
    REQUIRE(body.has_value());
    REQUIRE(rig.clock.wait_for_waiter_thread(*body, kBound));  // at its turn it saw the pulse and waits
    // The paused status poll, the re-sync and the pulse's stop body, which the pulse started after its turn: each
    // advance below must find the stop body's wait already stamped from the pulse start.
    REQUIRE(rig.clock.wait_for_waiters(3, kBound));
    CHECK(rig.handset.count('H') == 1);
    CHECK(rig.done.size() == 0);

    // The pulse's stop body wakes and ends; the pulse still counts until its completion delay is over (1.5 s).
    rig.clock.advance(milliseconds(500));
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    rig.clock.advance(milliseconds(500));  // the re-sync wakes inside the completion delay and waits again
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    CHECK(rig.handset.count('H') == 1);
    CHECK(rig.done.size() == 0);

    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('H') == 2);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

// On a V3 handset an async GOTO body waits for the GOTO's end and its landing gate (#2412), on the task clock. The
// GOTO aims at where the fake already points ("1234,4000"), so it lands without the handset ever reporting it in
// progress. The re-sync asks the handset at its turn.
TEST_CASE("SynScan host clock - a re-sync after a landed V3 GOTO writes the mount clock", "[synscan][clock]") {
    TrustGuard trust(true);
    Rig rig(true, sc::SynScanVersion::V3);
    // The slew body's end; shared, so a body thread never outlives what it signals.
    struct SlewEnd {
        std::mutex mutex;
        std::condition_variable cv;
        bool ended = false;
        std::atomic<std::thread::id> thread{};
    };
    const auto slew_end = std::make_shared<SlewEnd>();
    sc::set_slew_spawn_for_testing(*rig.driver, [slew_end](std::function<void()> body) {
        return std::thread([slew_end, body = std::move(body)] {
            slew_end->thread.store(std::this_thread::get_id());
            body();
            {
                std::lock_guard<std::mutex> lock(slew_end->mutex);
                slew_end->ended = true;
            }
            slew_end->cv.notify_all();
        });
    });
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);

    rig.driver->slew_to_coordinates_async(0x1234 / 65536.0 * 24.0, 90.0);
    const auto ended = [&] {
        std::lock_guard<std::mutex> lock(slew_end->mutex);
        return slew_end->ended;
    };
    // Steps only while the body's own thread waits on the clock, so no step lands while it is on the socket.
    const auto real_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!ended() && std::chrono::steady_clock::now() < real_deadline) {
        const auto body = slew_end->thread.load();
        if (body == std::thread::id{} || !rig.clock.wait_for_waiter_thread(body, milliseconds(50))) continue;
        rig.clock.advance(milliseconds(250));
        REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    }
    {
        std::unique_lock<std::mutex> lock(slew_end->mutex);
        REQUIRE(slew_end->cv.wait_for(lock, kBound, [&] { return slew_end->ended; }));
    }
    REQUIRE(rig.handset.count('R') == 1);
    rig.clock.advance(std::chrono::seconds(12));  // past the slew's forced-Slewing window
    sc::poll_status_now_for_testing(*rig.driver);
    REQUIRE_FALSE(rig.driver->get_slewing());  // landed: the handset's `L` says so

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(2000)});
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('H') == 2);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

TEST_CASE("SynScan host clock - a client UTCDate write after the host clock change wins over the re-sync",
          "[synscan][clock]") {
    TrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);
    // Holds the re-sync after it was decided. A pulse, not a MoveAxis: a client UTCDate write is refused while a
    // MoveAxis runs (#2412), and a pulse guide is motion to the re-sync only.
    rig.driver->pulse_guide(0, 30000);

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(3000)});
    const auto body = log.wait_for_motion_waiter(kBound);
    REQUIRE(body.has_value());
    REQUIRE(rig.clock.wait_for_waiter_thread(*body, kBound));

    const auto client = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(client);  // never waits for the re-sync's motion wait
    REQUIRE(rig.handset.count('H') == 2);
    rig.driver->abort_slew();  // ends the pulse
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(false));
    CHECK(rig.handset.count('H') == 2);
    CHECK(rig.driver->get_utc_date() == client + std::chrono::seconds(1));
}

TEST_CASE("SynScan host clock - a disconnect ends a re-sync waiting for motion without a write", "[synscan][clock]") {
    TrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    rig.driver->move_axis(1, 0.5);

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    const auto body = log.wait_for_motion_waiter(kBound);
    REQUIRE(body.has_value());
    REQUIRE(rig.clock.wait_for_waiter_thread(*body, kBound));
    const int writes = rig.handset.count('H');

    rig.driver->set_connected(false);
    CHECK(rig.done.size() == 1);  // drained: the body ended before the disconnect returned
    CHECK(rig.done.wait(1, milliseconds(0)) == std::optional<bool>(false));
    CHECK(rig.handset.count('H') == writes);
}

TEST_CASE("SynScan host clock - a host clock change during a connect is acted on once the connect returns",
          "[synscan][clock]") {
    TrustGuard trust(false);
    Rig rig(true);
    {
        std::lock_guard<std::mutex> lock(rig.handset.mutex);
        rig.handset.hold_site_read = true;  // the `w` comes after the connect's trust check
    }
    std::thread connecting([&] { rig.driver->set_connected(true); });
    REQUIRE(rig.handset.wait_site_read_entered(kBound));
    CHECK(rig.handset.count('H') == 0);

    trust.set(true);
    // The hook never waits for the connect: it must return while the handshake is still held.
    auto hook = std::async(std::launch::async, [&] {
        rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::BecameTrusted, milliseconds(0)});
    });
    REQUIRE(hook.wait_for(kBound) == std::future_status::ready);
    CHECK(sc::clock_resync_starts_for_testing(*rig.driver) == 0);

    rig.handset.release_site_read();
    connecting.join();
    REQUIRE(rig.driver->get_connected());
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('H') == 1);
}

// The re-sync's first motion check runs before it takes its wire turn; a GOTO started from the handset's keypad
// between that check and the turn is one the driver never sent, so only the `L` the re-sync asks at its turn sees it.
// The turn wait hook starts it there, on the re-sync's own thread, after the hook fired and before the `H` could go
// out.
TEST_CASE("SynScan host clock - a handset GOTO that starts before the re-sync's wire turn defers the write",
          "[synscan][clock]") {
    alpacacore::test::HostClockTrustGuard trust(true);
    LogCapture log;
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);

    std::atomic<bool> armed{true};
    struct ClearTurnHook {  // before `armed` dies, on every path
        alpacacore::TelescopeDriver& driver;
        ~ClearTurnHook() { sc::set_wire_turn_wait_hook_for_testing(driver, nullptr); }
    } clear_turn_hook{*rig.driver};
    sc::set_wire_turn_wait_hook_for_testing(*rig.driver, [&] {
        if (armed.exchange(false)) rig.handset.goto_in_progress.store(true);
    });
    const int slew_reads = rig.handset.count('L');

    rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(2000)});
    const auto body = log.wait_for_motion_waiter(kBound);
    REQUIRE(body.has_value());
    REQUIRE(rig.clock.wait_for_waiter_thread(*body, kBound));  // at its turn the `L` said slewing, and it waits
    CHECK_FALSE(armed.load());                                 // the GOTO started in the re-sync's own turn wait
    CHECK(rig.handset.count('L') == slew_reads + 1);
    CHECK(rig.handset.count('H') == 1);
    CHECK(rig.done.size() == 0);

    sc::set_wire_turn_wait_hook_for_testing(*rig.driver, nullptr);
    rig.handset.goto_in_progress.store(false);  // the keypad GOTO lands
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(rig.done.wait(1, kBound) == std::optional<bool>(true));
    CHECK(rig.handset.count('H') == 2);
    CHECK(near_host_now(rig.driver->get_utc_date()));
}

// A change the timer thread records while another thread holds the transition lock stays pending until that holder
// lets go; when a connect takes the lock first, the connect's own trust check covers the change, and the connect drops
// it rather than re-syncing again once it returns. The connect-start hook leaves one pending at exactly that point.
TEST_CASE("SynScan host clock - a reconnect drops a host clock change left pending at its start", "[synscan][clock]") {
    alpacacore::test::HostClockTrustGuard trust(true);
    Rig rig(true);
    rig.connect();
    REQUIRE(rig.handset.count('H') == 1);
    rig.driver->set_connected(false);
    REQUIRE(rig.done.size() == 0);

    std::atomic<int> hook_runs{0};
    std::atomic<bool> change_returned{false};
    // The timer thread's call lives out here, not in the hook: a hook that waited for transition_mutex_ would
    // deadlock a future destroyed inside the hook (its destructor joins the call, under the connect's lock), and the
    // case would hang instead of failing. Out here it is joined only after the connect has let go.
    std::future<void> change;
    struct ClearConnectHook {  // before the hook's captures die, on every path
        alpacacore::TelescopeDriver& driver;
        ~ClearConnectHook() { sc::set_connect_start_hook_for_testing(driver, nullptr); }
    } clear_connect_hook{*rig.driver};
    sc::set_connect_start_hook_for_testing(*rig.driver, [&] {
        ++hook_runs;
        // While this connect holds the transition lock: the call records the change and returns at once.
        change = std::async(std::launch::async, [&] {
            rig.driver->on_host_clock_changed(HostClockEvent{HostClockEvent::Kind::Stepped, milliseconds(4000)});
        });
        change_returned.store(change.wait_for(kBound) == std::future_status::ready);
    });

    rig.connect();
    CHECK(hook_runs.load() == 1);
    CHECK(change_returned.load());
    REQUIRE(change.valid());
    REQUIRE(change.wait_for(kBound) == std::future_status::ready);  // joined after the connect let go
    CHECK(sc::clock_resync_starts_for_testing(*rig.driver) == 0);   // started on the connecting thread, or never
    CHECK(rig.handset.count('H') == 2);                             // the reconnect's own write, and only it
    CHECK(rig.done.size() == 0);
}

#endif  // !_WIN32

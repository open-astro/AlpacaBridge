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

// Motion initiators and a goto body blocked in one exchange, SynScan (decision 0006, "Claim"): a new initiator
// replaces the body in flight instead of cancelling and joining it. The fake holds the first goto command, so the
// body is inside one exchange when the second initiator is called.
//
// The protocol wrapper holds its own mutex across each exchange, so an initiator that sends a command of its own
// (MoveAxis, AbortSlew, Unpark during a Park) waits out the one exchange in flight whatever the driver does. What it
// must not wait for is the old body: its driver lock, its tail, its later exchanges. Those initiators are checked by
// a state getter that sends nothing and answers while the exchange is held (the body holds no driver lock across it),
// and by returning soon after the hold ends. The two initiators that send nothing during a slew
// (SlewToCoordinatesAsync, Park) must return inside the hold.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeSynScanState {
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    alpacacore::util::TaskClock::clock::time_point now() const { return clock->now(); }
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<bool> goto_stopped{false};
    std::atomic<int> gate_ms{0};  // >0: the next goto command is held this long before it is answered
    std::atomic<Clock::rep> gate_entered{0};
    std::atomic<Clock::rep> gate_left{0};
    std::atomic<bool> report_slewing{false};  // true: slewing queries answer "slewing" whatever the fake clock says
    std::atomic<int> gate_l_ms{0};  // >0: the next slewing query is held this long, then answered "not slewing"
    std::atomic<Clock::rep> gate_l_entered{0};
    // true: every slewing query, a held one too, is answered with garbage
    std::atomic<bool> fail_l{false};
    std::atomic<int> model{50};  // the handset's model id ('m'): 50 is equatorial, 5 an AZ-EQ mount
    std::mutex command_mutex;
    std::vector<std::string> commands;
    void record(const std::string& command) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.push_back(command);
    }
    int command_count(char prefix) {
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [prefix](const auto& command) {
            return !command.empty() && command[0] == prefix;
        }));
    }
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (goto_count.load() == 0) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return !goto_stopped.load() && now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder synscan_responder(const std::shared_ptr<FakeSynScanState>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'P' || chunk[0] == 'M' || chunk[0] == 'e' || chunk[0] == 'r' || chunk[0] == 'R' ||
            chunk[0] == 'T') {
            st->record(chunk);
        }
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#" (the connect-time link check)
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            case 'r':
            case 'R':
            case 'b':
            case 'B': {
                const int hold = st->gate_ms.exchange(0);
                if (hold > 0) {
                    st->gate_entered.store(Clock::now().time_since_epoch().count());
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                    st->gate_left.store(Clock::now().time_since_epoch().count());
                }
                st->goto_started.store(st->now().time_since_epoch().count());
                st->goto_stopped.store(false);
                st->goto_count.fetch_add(1);
                return "#";
            }
            case 'L': {
                const int hold = st->gate_l_ms.exchange(0);
                if (hold > 0) {
                    st->gate_l_entered.store(Clock::now().time_since_epoch().count());
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                }
                if (st->fail_l.load()) return "?#";  // neither 0 nor 1: the handset read throws
                if (hold > 0) return "0#";
                return st->report_slewing.load() || st->goto_in_progress() ? "1#" : "0#";
            }
            case 'M':  // cancel goto
                st->goto_stopped.store(true);
                return "#";
            case 'T':  // tracking mode write
            case 'P':  // passthrough
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(st->model.load())) + "#";
            case 't':  // tracking mode read: sidereal tracking on
                return std::string(1, '\x01') + "#";
            case 'h':  // handset time, 8 binary bytes: 12:00:00 on 1 Jan 2026, UTC (a short answer times out)
                return std::string("\x0c\x00\x00\x01\x01\x1a\x00\x00", 8) + "#";
            case 'w':
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 6000;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

struct Rig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeSynScanState> st = std::make_shared<FakeSynScanState>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    explicit Rig(int model = 50) {
        st->clock = &clock;
        st->model.store(model);
        server = std::make_unique<alpacacore::test::FakeMountServer>(synscan_responder(st));
        REQUIRE(server->ok());
        driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server->port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    }
    ~Rig() { driver->set_connected(false); }
};

long ms_since(Clock::time_point t0) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

constexpr int kHoldMs = 2500;

struct Timing {
    long probe_ms = -1;      // a getter that sends nothing, called while the old body's exchange is held
    long took_ms = -1;       // the second initiator
    long past_hold_ms = -1;  // the second initiator's return after the hold ended; -1 when it returned inside the hold
};

Timing time_second_initiator(const char* name, const std::function<void(alpacacore::TelescopeDriver&)>& first,
                             const std::function<void(alpacacore::TelescopeDriver&)>& second, int model = 50) {
    Rig rig(model);
    rig.st->gate_ms.store(kHoldMs);
    first(*rig.driver);
    REQUIRE(wait_until([&] { return rig.st->gate_entered.load() != 0; }, 5000));
    const auto entered = Clock::time_point(Clock::duration(rig.st->gate_entered.load()));
    Timing timing;
    // AtPark takes the driver lock and sends nothing: it answers only if the body does not hold the lock across its
    // exchange.
    const auto probe_t0 = Clock::now();
    (void)rig.driver->get_at_park();
    timing.probe_ms = ms_since(probe_t0);
    const long into_hold = ms_since(entered);
    std::atomic<long> took{-1};
    std::atomic<long> past_hold{-1};
    std::atomic<bool> threw{false};
    std::thread t([&] {
        const auto t0 = Clock::now();
        try {
            second(*rig.driver);
        } catch (...) {
            threw = true;
        }
        const auto returned = Clock::now();
        took = std::chrono::duration_cast<std::chrono::milliseconds>(returned - t0).count();
        const auto left = rig.st->gate_left.load();
        if (left != 0) {
            past_hold = std::chrono::duration_cast<std::chrono::milliseconds>(returned -
                                                                              Clock::time_point(Clock::duration(left)))
                            .count();
        }
    });
    t.join();
    timing.took_ms = took.load();
    timing.past_hold_ms = past_hold.load();
    std::printf(
        "initiator %-16s hold=%d ms, probe answered after %ld ms, called %ld ms into the hold, returned after %ld ms "
        "(%ld ms past the hold end), threw=%d\n",
        name, kHoldMs, timing.probe_ms, into_hold, timing.took_ms, timing.past_hold_ms, threw.load() ? 1 : 0);
    std::fflush(stdout);
    return timing;
}

// A wire-sending initiator may wait out the one exchange in flight (the margin covers its own few commands on a slow
// or sanitized runner), never the old body.
constexpr long kPastHoldMarginMs = 2000;

}  // namespace

TEST_CASE("SynScan initiator supersede - SlewToCoordinatesAsync during a blocked goto exchange",
          "[synscan][initiator-supersede]") {
    const auto timing = time_second_initiator(
        "slew->slew", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
        [](auto& d) { d.slew_to_coordinates_async(6.5, 25.0); });
    CHECK(timing.probe_ms < kHoldMs / 2);
    CHECK(timing.took_ms < kHoldMs / 2);
}

TEST_CASE("SynScan initiator supersede - MoveAxis during a blocked goto exchange", "[synscan][initiator-supersede]") {
    const auto timing = time_second_initiator(
        "slew->MoveAxis", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); },
        [](auto& d) { d.move_axis(0, 0.5); });
    CHECK(timing.probe_ms < kHoldMs / 2);
    CHECK(timing.past_hold_ms < kPastHoldMarginMs);
}

TEST_CASE("SynScan initiator supersede - AbortSlew during a blocked goto exchange", "[synscan][initiator-supersede]") {
    const auto timing = time_second_initiator(
        "slew->AbortSlew", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); }, [](auto& d) { d.abort_slew(); });
    CHECK(timing.probe_ms < kHoldMs / 2);
    CHECK(timing.past_hold_ms < kPastHoldMarginMs);
}

TEST_CASE("SynScan initiator supersede - Unpark during a Park whose goto exchange is blocked",
          "[synscan][initiator-supersede]") {
    const auto timing = time_second_initiator("park->Unpark", [](auto& d) { d.park(); }, [](auto& d) { d.unpark(); });
    CHECK(timing.probe_ms < kHoldMs / 2);
    CHECK(timing.past_hold_ms < kPastHoldMarginMs);
}

TEST_CASE("SynScan initiator supersede - Park during a blocked goto exchange", "[synscan][initiator-supersede]") {
    const auto timing = time_second_initiator(
        "slew->Park", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); }, [](auto& d) { d.park(); });
    CHECK(timing.probe_ms < kHoldMs / 2);
    CHECK(timing.took_ms < kHoldMs / 2);
}

// An AZ-EQ mount with alignment Auto has no known geometry, so Park saves its target as RA/Dec, the position then.
TEST_CASE("SynScan initiator supersede - Park on an AZ-EQ mount during a blocked goto exchange",
          "[synscan][initiator-supersede]") {
    const auto timing = time_second_initiator(
        "slew->Park AZ-EQ", [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); }, [](auto& d) { d.park(); }, 5);
    CHECK(timing.took_ms < kHoldMs / 2);
}

// Park reads no tracking mode, so on a fresh connect AbortSlew's is the first since connect. AbortSlew may wait for
// the exchange in flight, but without the driver lock: a getter answers meanwhile.
TEST_CASE("SynScan initiator supersede - AbortSlew during a Park on a fresh connect",
          "[synscan][initiator-supersede]") {
    Rig rig;
    rig.st->gate_ms.store(kHoldMs);
    rig.driver->park();
    REQUIRE(wait_until([&] { return rig.st->gate_entered.load() != 0; }, 5000));
    std::thread abort([&] { rig.driver->abort_slew(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto probe_t0 = Clock::now();
    (void)rig.driver->get_at_park();
    const long probe_ms = ms_since(probe_t0);
    abort.join();
    std::printf("AbortSlew during Park: probe answered after %ld ms (hold %d ms)\n", probe_ms, kHoldMs);
    CHECK(probe_ms < kHoldMs / 2);
}

// The old body is inside its second slewing query (held) when MoveAxis replaces it. The query then answers "not
// slewing", the one answer that would take the body to its tail (clear Slewing, publish a measurement). A superseded
// body must send nothing after the exchange: the handset position read ('e') is what the tail would send. The status
// poll is parked on the fake clock, so it sends no 'e' of its own while the clock stands still.
TEST_CASE("SynScan initiator supersede - a superseded slew body sends nothing after its held exchange",
          "[synscan][initiator-supersede]") {
    Rig rig;
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    rig.st->report_slewing.store(true);
    // The initiator publishes an 8 s Slewing window during which the body asks the handset nothing: step past it.
    rig.clock.advance(std::chrono::seconds(9));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // the body polls once and sleeps 250 ms again
    rig.st->gate_l_ms.store(kHoldMs);
    for (int i = 0; i < 3 && rig.st->gate_l_entered.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rig.clock.advance(std::chrono::milliseconds(260));
    }
    REQUIRE(wait_until([&] { return rig.st->gate_l_entered.load() != 0; }, 5000));
    const int reads_before = rig.st->command_count('e');
    const int gotos_before = rig.st->command_count('r') + rig.st->command_count('R');
    rig.driver->move_axis(0, 0.5);  // waits out the held exchange, then owns the axes
    // The old body wakes behind MoveAxis; give it real time to run its tail if it were going to.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(rig.st->command_count('e') == reads_before);
    CHECK(rig.st->command_count('r') + rig.st->command_count('R') == gotos_before);
}

// The ownership check and the wire send must be one step (decision 0006, "Claim"; wire turn). A status-poll exchange
// holds the wrapper mutex while the slew starts; the poll's next exchanges and the slew body's goto then queue on the
// wrapper with the driver lock released, and AbortSlew queues its stops right behind them. A body that checked
// ownership before it queued must still not send its goto after AbortSlew's stop. Any other order is legal: the goto
// before the stops, or no goto at all. Without a wire turn the order is up to which waiter the wrapper mutex wakes
// first (std::mutex is not FIFO), so the case runs short trials on one rig. Without the turn about one trial in a
// hundred reorders and the case fails in roughly one run in six: it catches a regression by repetition, not every run.
TEST_CASE("SynScan initiator supersede - a queued goto never reaches the mount after AbortSlew",
          "[synscan][initiator-supersede]") {
    constexpr int kTrials = 16;
    constexpr int kPollHoldMs = 150;
    Rig rig;
    int reordered = 0;
    int moving_after_abort = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
        rig.clock.advance(std::chrono::seconds(5));  // the last trial's goto, if any, is over on the fake clock
        rig.st->gate_l_entered.store(0);
        rig.st->gate_l_ms.store(kPollHoldMs);  // the next status-poll slewing query holds the wrapper mutex
        for (int i = 0; i < 100 && rig.st->gate_l_entered.load() == 0; ++i) {
            rig.clock.advance(std::chrono::milliseconds(1100));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(wait_until([&] { return rig.st->gate_l_entered.load() != 0; }, 2000));
        {
            std::lock_guard<std::mutex> l(rig.st->command_mutex);
            rig.st->commands.clear();
        }
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
        // The body's goto now queues on the wrapper behind the poll's remaining exchanges. Calling AbortSlew at once
        // usually claims before the body's ownership check, which proves nothing; 20 ms lands in the window.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        rig.driver->abort_slew();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // a late goto would arrive now
        std::string order;
        {
            std::lock_guard<std::mutex> l(rig.st->command_mutex);
            for (const auto& command : rig.st->commands) order += command.empty() ? '?' : command[0];
        }
        const auto first_stop = order.find('M');
        const auto last_goto = order.find_last_of("rR");
        if (first_stop != std::string::npos && last_goto != std::string::npos && last_goto > first_stop) {
            ++reordered;
            std::printf("trial %d: wire order %s, goto after AbortSlew's stop\n", trial, order.c_str());
        }
        if (rig.st->goto_in_progress()) ++moving_after_abort;
    }
    std::fflush(stdout);
    CHECK(reordered == 0);
    CHECK(moving_after_abort == 0);
}

// A Park body superseded inside a slewing query that then fails touches no driver state (decision 0006, rule 6): the
// count of failed polls belongs to the next Park, and the warning that reports each counted failure names it. Unpark
// claims while the query is held and sends its stop after it.
TEST_CASE("SynScan initiator supersede - a superseded Park body does not count its failed status poll",
          "[synscan][initiator-supersede]") {
    std::atomic<int> counted{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink([&](alpacacore::logging::LogLevel, std::string_view, std::string_view message) {
        if (message.find("Park slew status poll failed") != std::string_view::npos) ++counted;
    });
    Rig rig;
    rig.driver->park();
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    rig.st->report_slewing.store(true);
    rig.clock.advance(std::chrono::seconds(9));  // past the 8 s Slewing window: the Park polls ask the handset now
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    rig.st->fail_l.store(true);
    rig.st->gate_l_ms.store(kHoldMs);
    for (int i = 0; i < 3 && rig.st->gate_l_entered.load() == 0; ++i) {
        rig.clock.advance(std::chrono::milliseconds(260));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(wait_until([&] { return rig.st->gate_l_entered.load() != 0; }, 5000));
    const auto entered = Clock::time_point(Clock::duration(rig.st->gate_l_entered.load()));
    std::thread unpark([&] { rig.driver->unpark(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(ms_since(entered) < kHoldMs);  // Unpark claimed while the query was still held
    // The Park body's thread exits once its query has failed.
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::milliseconds(kHoldMs + 5000)));
    unpark.join();
    CHECK(counted.load() == 0);
}

// AbortSlew's claim supersedes the Park body, which then leaves the park state alone, so a failed stop must end the
// park itself: nothing is left to complete it. The link drops once the park GOTO is out, so every stop fails. The fake
// clock stands still, so the dropped link is never declared lost; a relink would clear the park state itself.
TEST_CASE("SynScan initiator supersede - a failed AbortSlew during a Park ends the park",
          "[synscan][initiator-supersede]") {
    Rig rig;
    rig.driver->park();
    REQUIRE(wait_until([&] { return rig.st->goto_count.load() == 1; }, 5000));
    REQUIRE(rig.server->drop_connections());
    const auto error_of = [](const std::function<void()>& call) -> std::pair<int, std::string> {
        try {
            call();
        } catch (const alpacacore::AlpacaException& ex) {
            return {ex.error_code(), ex.what()};
        }
        return {0, ""};
    };
    const auto aborted = error_of([&] { rig.driver->abort_slew(); });
    REQUIRE(aborted.first == alpacacore::AlpacaError::DriverException);
    REQUIRE(aborted.second.rfind("AbortSlew stop failed: ", 0) == 0);
    // Refused only while a park is in flight or done; on the dropped link it fails on the wire instead.
    CHECK(error_of([&] { rig.driver->set_tracking(true); }).first != alpacacore::AlpacaError::InvalidWhileParked);
    // A Park that starts clears the stored AbortSlew error at once; Park during a park returns doing nothing.
    rig.driver->park();
    CHECK(error_of([&] { (void)rig.driver->get_slewing(); }).second.rfind("AbortSlew stop failed: ", 0) != 0);
}

#endif  // _WIN32

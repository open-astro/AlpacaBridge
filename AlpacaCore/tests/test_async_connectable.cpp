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

// AsyncConnectable is the shared connection-thread base for all 26 vendor
// drivers (issue #100). These tests encode two contracts that a 2026-07
// ConformU session found were NOT actually honored by a caller:
//
// 1. get_connecting() is the only signal guaranteed to stay true for a
//    task's entire lifetime. get_connected() is NOT safe as a completion
//    signal: a driver following the documented driver-lifecycle.instructions.md pattern (clear
//    connected_ at the START of teardown, before a throwing SDK close) will
//    have get_connected() read false while get_connecting() is still true.
//    A router disconnect-completion wait that polled `get_connected() &&
//    get_connecting()` exited as soon as connected_ flipped, well before the
//    task actually finished -- letting a client's next Connect() race the
//    still-running disconnect and get silently dropped by the rule below.
// 2. A connect() racing an in-flight disconnect is QUEUED (pending_connect_)
//    and run by the task tail; a disconnect() racing an in-flight connect is
//    NEVER dropped (recorded and honored by the task tail), and always
//    outranks a queued connect.

#include <alpacacore/async_connectable.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "catch2_compat.h"

namespace {

using namespace std::chrono_literals;

// Minimal driver double. Mirrors the QHY camera's real pattern: connected_
// clears at the START of a (simulated slow) disconnect, and is set only at
// the END of a (simulated slow) connect -- see qhy_camera_driver.cpp
// set_connected_impl().
class TestConnectable : public alpacacore::AsyncConnectable {
public:
    TestConnectable() : AsyncConnectable("Test") {}
    ~TestConnectable() override { shutdown_connection(); }

    // A lost link reads false while the driver still holds its session, as
    // the OnStep and Sky-Watcher drivers do (decision 0009).
    bool get_connected() const override { return connected_.load() && !link_lost_.load(); }
    bool get_connecting() const { return connection_task_active(); }

    void set_connected(bool connect) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect) {
            if (record_disconnect_if_connect_in_flight(connected_.load())) {
                return;
            }
            in_disconnect_task = true;
            if (!connected_.load()) {
                return;
            }
            connected_.store(false);  // cleared FIRST, before the "slow" teardown
            link_lost_.store(false);
            std::this_thread::sleep_for(op_delay_);
            if (fail_disconnect_.load()) {
                // Mirrors a throwing SDK close mid-teardown: connected_ is
                // already cleared (driver-lifecycle.instructions.md pattern) but the teardown did
                // not complete.
                throw std::runtime_error("simulated teardown failure");
            }
            ++disconnect_completions_;
            return;
        }
        // A connect on a thread that already ran a disconnect is the task
        // tail running a connect that was queued behind that disconnect.
        if (in_disconnect_task) {
            ++tail_connects_;
        }
        if (consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected_.load()) {
            return;
        }
        std::this_thread::sleep_for(op_delay_);
        if (fail_connect_.load()) {
            // A driver that refuses a connect and says why -- the shape every
            // real driver's guard has (issue #358).
            throw std::runtime_error("site latitude and longitude must be set before connecting");
        }
        connected_.store(true);  // set LAST, after the "slow" connect work
        link_lost_.store(lose_link_on_connect_.load());
        ++connect_completions_;
    }

    // This double is not an AlpacaDriver, so there is no virtual to forward
    // to -- it reads the protected accessor directly. A real driver uses
    // ALPACA_EXPOSE_CONNECT_ERROR().
    using alpacacore::AsyncConnectable::connecting_error;
    using alpacacore::AsyncConnectable::last_connect_error;

    void connect() { start_connection_task(true); }
    void disconnect() { start_connection_task(false); }

    std::chrono::milliseconds op_delay_{50};
    std::atomic<bool> fail_disconnect_{false};
    std::atomic<bool> fail_connect_{false};
    std::atomic<bool> lose_link_on_connect_{false};  ///< the link is lost as the connect ends
    std::atomic<int> connect_completions_{0};
    std::atomic<int> disconnect_completions_{0};
    std::atomic<int> tail_connects_{0};  ///< connects run by a task tail after a disconnect

private:
    // Fresh per task thread: set once this thread has run a disconnect.
    static inline thread_local bool in_disconnect_task = false;
    std::mutex mutex_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> link_lost_{false};
};

void wait_until_idle(TestConnectable& d, std::chrono::milliseconds budget = 2s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (d.get_connecting() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
}

}  // namespace

TEST_CASE("AsyncConnectable - basic connect/disconnect cycle", "[async_connectable][unit]") {
    TestConnectable d;
    REQUIRE_FALSE(d.get_connected());

    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());
    REQUIRE_FALSE(d.get_connecting());

    d.disconnect();
    wait_until_idle(d);
    REQUIRE_FALSE(d.get_connected());
    REQUIRE_FALSE(d.get_connecting());
}

TEST_CASE("AsyncConnectable - get_connected() is not a valid completion signal", "[async_connectable][unit]") {
    // Regression test for the 2026-07 router bug: a caller must not treat
    // get_connected()==false as "the disconnect task is done". This proves
    // the opposite window exists -- connected_ is false while connecting is
    // still true -- so any waiter must poll get_connecting() alone.
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());

    d.disconnect();
    // Give the task time to clear connected_ (near-instant) but not enough
    // to finish the simulated slow teardown (100ms).
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(d.get_connected());
    CHECK(d.get_connecting());  // <-- the signal a waiter must actually trust

    wait_until_idle(d);
    REQUIRE_FALSE(d.get_connecting());
    REQUIRE(d.disconnect_completions_.load() == 1);
}

TEST_CASE("AsyncConnectable - connect racing an in-flight disconnect is queued", "[async_connectable][unit]") {
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());

    d.disconnect();
    std::this_thread::sleep_for(10ms);  // disconnect is now in flight
    d.connect();                        // queued; the task tail runs it

    wait_until_idle(d);
    // The caller's newest instruction wins: the queued connect runs after
    // the disconnect finishes, so the device ends up connected again.
    CHECK(d.get_connected());
    CHECK(d.connect_completions_.load() == 2);
    CHECK(d.disconnect_completions_.load() == 1);
}

TEST_CASE("AsyncConnectable - disconnect racing an in-flight connect is never dropped", "[async_connectable][unit]") {
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.connect();
    std::this_thread::sleep_for(10ms);  // connect is now in flight
    d.disconnect();                     // must be recorded, not dropped

    wait_until_idle(d);
    // The device must end up disconnected: the deferred disconnect the task
    // tail runs after a no-op-looking connect completes.
    CHECK_FALSE(d.get_connected());
}

TEST_CASE("AsyncConnectable - a disconnect queued against a connect reaches a driver whose link was lost",
          "[async_connectable][unit]") {
    // Issue #1289 item 6: the link is lost while the connect runs, so
    // Connected reads false when the task tail looks. The queued Disconnect
    // must still reach set_connected(false), or the lost session is never
    // reset.
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.lose_link_on_connect_.store(true);
    d.connect();
    std::this_thread::sleep_for(10ms);  // connect is now in flight
    d.disconnect();                     // queued against it

    wait_until_idle(d);
    CHECK(d.connect_completions_.load() == 1);
    CHECK_FALSE(d.get_connected());
    CHECK(d.disconnect_completions_.load() == 1);  // the teardown ran
}

TEST_CASE("AsyncConnectable - disconnect/connect/disconnect chain lands disconnected", "[async_connectable][unit]") {
    // Issue #136: the third-alternating-request reconciliation. A disconnect
    // running, a connect queued against it, then a SECOND disconnect — the
    // newest instruction must cancel the queued connect, or the tail
    // reconnects against the caller's latest word.
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());

    d.disconnect();
    std::this_thread::sleep_for(10ms);  // disconnect in flight
    d.connect();                        // queued
    std::this_thread::sleep_for(10ms);
    d.disconnect();  // must cancel the queued connect

    wait_until_idle(d);
    CHECK_FALSE(d.get_connected());
    CHECK(d.connect_completions_.load() == 1);  // the queued connect never ran
}

TEST_CASE("AsyncConnectable - failed disconnect drops a queued connect", "[async_connectable][unit]") {
    // Issue #136: if the teardown throws with a pending_connect_ queued, the
    // tail must NOT fire the connect right after the failed teardown — the
    // hardware state is unknown. The dropped connect is visible (Connected
    // stays false) and retryable.
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());

    d.fail_disconnect_.store(true);
    d.disconnect();
    std::this_thread::sleep_for(10ms);  // failing disconnect in flight
    d.connect();                        // queued; must be dropped

    wait_until_idle(d);
    CHECK_FALSE(d.get_connecting());
    CHECK(d.connect_completions_.load() == 1);  // no reconnect after the failed teardown
}

TEST_CASE("AsyncConnectable - lifecycle stress with chained pending flags", "[async_connectable][stress-guard]") {
    // TSan-covered regression for the pending_connect_ queuing path and the
    // tail's chained-flag draining (issue #136): hammer alternating async
    // requests from several threads while readers poll. Run under the
    // sanitizers-tsan CI job; even without TSan this catches crashes, hangs,
    // and std::terminate.
    //
    // Tagged [stress-guard], NOT [stress] (issue #322). This file is
    // unconditional in TEST_SOURCES, so while this case carried [stress] the
    // vendor zero-coverage grep was VACUOUS: with every vendor target absent,
    // `alpacacore_tests "[stress]"` still matched this one case and printed
    // "All tests passed (2 assertions in 1 test case)", which the grep accepts
    // -- so sanitizers-tsan went green with zero vendor concurrency coverage,
    // the exact failure that grep exists to catch. [stress] now means "a
    // vendor driver registration"; core/harness self-tests like this one use
    // [stress-guard] and run under their own TSan invocation, so this case
    // keeps its TSan coverage without propping up the vendor threshold.
    TestConnectable d;
    d.op_delay_ = 2ms;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&d, &stop, i]() {
            int n = i;
            while (!stop.load()) {
                ((n++ % 2) == 0) ? d.connect() : d.disconnect();
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&d, &stop]() {
            while (!stop.load()) {
                static_cast<void>(d.get_connecting());
                static_cast<void>(d.get_connected());
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    std::this_thread::sleep_for(750ms);
    stop.store(true);
    for (auto& t : threads) {
        t.join();
    }
    wait_until_idle(d);

    // Whether the hammering ever lands a connect behind a running disconnect
    // depends on timing (under TSan on arm64 it did not), so drive the chained
    // path once on purpose: queue a connect behind a slow disconnect. The
    // task's tail must run it; without pending_connect_ it is dropped.
    d.op_delay_ = 300ms;
    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());
    d.tail_connects_.store(0);
    d.disconnect();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (d.get_connected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);  // connected_ clears as the teardown starts
    }
    d.connect();  // queued behind the in-flight disconnect
    wait_until_idle(d);
    CHECK(d.tail_connects_.load() == 1);
    CHECK(d.get_connected());

    // Converge deterministically: the final instruction is a disconnect and
    // it must win regardless of what the hammering left queued.
    d.disconnect();
    wait_until_idle(d);
    CHECK_FALSE(d.get_connected());
    CHECK_FALSE(d.get_connecting());
}

// Issue #358: the reason a connect failed is kept, not just the fact.
TEST_CASE("AsyncConnectable - a failed connect keeps the driver's reason", "[async_connectable][unit]") {
    TestConnectable d;

    // Nothing has happened yet, so there is no reason to report. The router
    // falls back to its old constant on an empty string, so "" has to mean
    // "no failure stands" and never "failed for reasons unknown".
    CHECK(d.last_connect_error().empty());

    d.fail_connect_.store(true);
    d.connect();
    wait_until_idle(d);
    CHECK(!d.get_connected());
    // Verbatim, not a category: the whole point is that the sentence telling
    // the operator what to fix reaches them instead of stopping at the log.
    CHECK(d.last_connect_error() == "site latitude and longitude must be set before connecting");

    // A later successful connect clears it. Cleared at the START of the
    // attempt, so a client polling during a retry cannot read the previous
    // failure's text and attribute it to the attempt in flight.
    d.fail_connect_.store(false);
    d.connect();
    wait_until_idle(d);
    CHECK(d.get_connected());
    CHECK(d.last_connect_error().empty());
}

TEST_CASE("AsyncConnectable - a failed disconnect does not write the connect reason", "[async_connectable][unit]") {
    TestConnectable d;

    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());
    REQUIRE(d.last_connect_error().empty());

    // A teardown failure is reported on its own path and has no client waiting
    // on a reason here. Were it allowed to write this string, the next thing a
    // client read would be an unrelated teardown message presented as the
    // reason its connect failed -- worse than the constant it replaces.
    d.fail_disconnect_.store(true);
    d.disconnect();
    wait_until_idle(d);
    CHECK(!d.get_connected());
    CHECK(d.last_connect_error().empty());
}

// Issue #776: after a failed Connect(), the completion property (Connecting)
// has to report the failure until the client's next Connect or Disconnect,
// not fall back to false as if the connect had simply finished.
TEST_CASE("AsyncConnectable - a failed connect stands until the next request", "[async_connectable][unit]") {
    TestConnectable d;
    CHECK(d.connecting_error().empty());

    d.fail_connect_.store(true);
    d.connect();
    wait_until_idle(d);
    REQUIRE_FALSE(d.get_connecting());
    CHECK(d.connecting_error() == "site latitude and longitude must be set before connecting");
    // Read twice: reporting it is not consuming it.
    CHECK(d.connecting_error() == "site latitude and longitude must be set before connecting");

    // A Disconnect resets it; LastConnectError (the management API's copy)
    // keeps the reason, as before.
    d.disconnect();
    wait_until_idle(d);
    CHECK(d.connecting_error().empty());
    CHECK(d.last_connect_error() == "site latitude and longitude must be set before connecting");

    // A new Connect resets it at once, before the attempt finishes.
    d.connect();
    wait_until_idle(d);
    REQUIRE(!d.connecting_error().empty());
    d.fail_connect_.store(false);
    d.op_delay_ = 200ms;
    d.connect();
    CHECK(d.connecting_error().empty());
    wait_until_idle(d);
    CHECK(d.get_connected());
    CHECK(d.connecting_error().empty());
}

TEST_CASE("AsyncConnectable - a connect failure superseded by a disconnect is not reported",
          "[async_connectable][unit]") {
    TestConnectable d;
    d.op_delay_ = 200ms;
    d.fail_connect_.store(true);
    d.connect();
    std::this_thread::sleep_for(20ms);
    REQUIRE(d.get_connecting());
    // The client has moved on: its newest request is the Disconnect, which
    // succeeds, so Connecting must not then raise the older connect's error.
    d.disconnect();
    wait_until_idle(d);
    CHECK_FALSE(d.get_connected());
    CHECK(d.connecting_error().empty());
    CHECK(d.last_connect_error() == "site latitude and longitude must be set before connecting");
}

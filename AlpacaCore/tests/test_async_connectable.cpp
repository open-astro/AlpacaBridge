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
//    signal: a driver following the documented AGENTS.md pattern (clear
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

    bool get_connected() const override { return connected_.load(); }
    bool get_connecting() const { return connection_task_active(); }

    void set_connected(bool connect) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect) {
            if (record_disconnect_if_connect_in_flight(connected_.load())) {
                return;
            }
            if (!connected_.load()) {
                return;
            }
            connected_.store(false);  // cleared FIRST, before the "slow" teardown
            std::this_thread::sleep_for(op_delay_);
            if (fail_disconnect_.load()) {
                // Mirrors a throwing SDK close mid-teardown: connected_ is
                // already cleared (AGENTS.md pattern) but the teardown did
                // not complete.
                throw std::runtime_error("simulated teardown failure");
            }
            ++disconnect_completions_;
            return;
        }
        if (consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected_.load()) {
            return;
        }
        std::this_thread::sleep_for(op_delay_);
        connected_.store(true);  // set LAST, after the "slow" connect work
        ++connect_completions_;
    }

    void connect() { start_connection_task(true); }
    void disconnect() { start_connection_task(false); }

    std::chrono::milliseconds op_delay_{50};
    std::atomic<bool> fail_disconnect_{false};
    std::atomic<int> connect_completions_{0};
    std::atomic<int> disconnect_completions_{0};

private:
    std::mutex mutex_;
    std::atomic<bool> connected_{false};
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

TEST_CASE("AsyncConnectable - lifecycle stress with chained pending flags", "[async_connectable][stress]") {
    // TSan-covered regression for the pending_connect_ queuing path and the
    // tail's chained-flag draining (issue #136): hammer alternating async
    // requests from several threads while readers poll. Run under the
    // sanitizers-tsan CI job; even without TSan this catches crashes, hangs,
    // and std::terminate.
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

    // Converge deterministically: the final instruction is a disconnect and
    // it must win regardless of what the hammering left queued.
    d.disconnect();
    wait_until_idle(d);
    CHECK_FALSE(d.get_connected());
    CHECK_FALSE(d.get_connecting());
}

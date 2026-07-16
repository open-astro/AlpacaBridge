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
// 2. A connect() racing an in-flight disconnect is deliberately DROPPED
//    (not queued); a disconnect() racing an in-flight connect is NEVER
//    dropped (recorded and honored by the task tail).

#include <alpacacore/async_connectable.h>

#include <atomic>
#include <chrono>
#include <thread>

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

TEST_CASE("AsyncConnectable - connect racing an in-flight disconnect is dropped", "[async_connectable][unit]") {
    TestConnectable d;
    d.op_delay_ = 100ms;
    d.connect();
    wait_until_idle(d);
    REQUIRE(d.get_connected());

    d.disconnect();
    std::this_thread::sleep_for(10ms);  // disconnect is now in flight
    d.connect();                        // deliberately dropped, not queued

    wait_until_idle(d);
    CHECK_FALSE(d.get_connected());
    // Only the original disconnect ran to completion; the racing connect
    // never spawned a task at all.
    CHECK(d.connect_completions_.load() == 1);
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

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

#pragma once

#include <alpacacore/alpacadriver.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace alpacacore::test {

/**
 * Reusable connect/disconnect/operate concurrency stress harness (issue #101).
 *
 * The #1 driver-review bug class is concurrency — use-after-close, TOCTOU on
 * locks, dropped racing disconnects, destructor vs connection-thread races.
 * ConformU is single-threaded and the ASan/UBSan job is single-threaded too,
 * so nothing automated exercised these paths before this harness. Run the
 * [stress] tests under the `sanitizers-tsan` CI job (or locally with
 * RUN_TSAN=1 ./scripts/ci_preflight.sh) to turn latent data races into hard
 * failures; even without TSan they catch crashes and std::terminate.
 *
 * Registering a new driver is one TEST_CASE: build a factory returning the
 * driver and an `operate` callback exercising its operational surface, then
 * call the scenarios below. Every callback failure is swallowed — operations
 * racing a disconnect are EXPECTED to throw NotConnected; what must never
 * happen is a crash, a hang, or a TSan report.
 */
struct StressOptions {
    int lifecycle_threads = 4;                // hammer connect()/disconnect()/set_connected()
    int op_threads = 4;                       // hammer the operate callback
    std::chrono::milliseconds duration{750};  // per-scenario wall clock
};

/// Hammer one driver instance from many threads: async connect/disconnect,
/// sync set_connected (the ASCOM Connected setter path — it bypasses the
/// async task gate, which is exactly where the record/consume gates matter),
/// status reads, and operational calls, all concurrently.
inline void run_lifecycle_stress(AlpacaDriver& driver, const std::function<void(AlpacaDriver&)>& operate,
                                 const StressOptions& opt = {}) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(opt.lifecycle_threads + opt.op_threads));

    for (int i = 0; i < opt.lifecycle_threads; ++i) {
        threads.emplace_back([&driver, &stop, i]() {
            int n = 0;
            while (!stop.load()) {
                try {
                    switch ((i + n) % 5) {
                        case 0:
                            driver.connect();
                            break;
                        case 1:
                            driver.disconnect();
                            break;
                        case 2:
                            driver.set_connected(true);
                            break;
                        case 3:
                            driver.set_connected(false);
                            break;
                        default:
                            static_cast<void>(driver.get_connecting());
                            static_cast<void>(driver.get_connected());
                            break;
                    }
                } catch (const std::exception&) {
                    // Expected: connects can fail, ops can race a disconnect.
                    // The harness asserts absence of crashes/races, not success.
                }
                ++n;
                // Yield so the scheduler interleaves rather than one thread
                // monopolizing the driver mutex for the whole window.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    for (int i = 0; i < opt.op_threads; ++i) {
        threads.emplace_back([&driver, &operate, &stop]() {
            while (!stop.load()) {
                try {
                    operate(driver);
                } catch (const std::exception&) {
                    // NotConnected etc. while racing a disconnect — expected.
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }

    std::this_thread::sleep_for(opt.duration);
    stop.store(true);
    for (auto& t : threads) {
        t.join();
    }
}

/// Post-storm convergence: drive the driver to the wanted connected state
/// with sync retries. Right after a storm the last async task may still be in
/// flight, so a single sync set_connected can legitimately no-op against the
/// protocol's pending-disconnect machinery (a first connect may consume a
/// stale racing disconnect and stay down — that IS the contract). A correct
/// driver converges in a couple of retries; a wedged one exhausts the budget.
inline bool settle_connected(AlpacaDriver& driver, bool want,
                             std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            driver.set_connected(want);
        } catch (const std::exception&) {
            // e.g. a connect losing a race with an in-flight teardown.
        }
        if (driver.get_connected() == want && !driver.get_connecting()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Same condition as the loop exit: a budget-expired driver with a task
    // still in flight is wedged, not settled — without the get_connecting()
    // conjunct, want=false would vacuously "pass" on a stuck driver whose
    // in-flight task could still flip the state after the check.
    return driver.get_connected() == want && !driver.get_connecting();
}

/// The issue-#100 destructor race: destroy the driver while its async connect
/// task is (or may still be) in flight. A driver missing the shutting_down_
/// teardown contract leaves an unjoined thread -> std::terminate; a driver
/// whose task touches members after they're destroyed is a TSan/ASan report.
inline void run_destruction_during_connect_stress(const std::function<std::unique_ptr<AlpacaDriver>()>& make,
                                                  int iterations = 100) {
    for (int i = 0; i < iterations; ++i) {
        auto driver = make();
        driver->connect();  // spawns the async connection task
        if ((i % 2) != 0) {
            // Half the time, also race a disconnect against the connect so the
            // pending-disconnect machinery is live at destruction time.
            driver->disconnect();
        }
        driver.reset();  // ~Driver must join the in-flight task, every time
    }
}

/// Deterministic protocol check (PR #115 round 4): connect() on an ALREADY
/// CONNECTED device spawns a no-op task; a disconnect racing it is recorded
/// against that task and must still be honored — the no-op connect must not
/// consume the pending flag (it performs no transition), leaving the task
/// tail to run the deferred disconnect. A driver where the flag is eaten
/// sits Connected until the deadline and fails the caller's assert.
/// Returns the settled connected state; the caller asserts it is false.
/// use_sync_disconnect selects the disconnect entry point: the async route
/// (disconnect() -> start_connection_task(false), which records via the
/// task-spawn path) or the SYNC route (set_connected(false) — the ASCOM
/// Connected=false PUT, which records via obligation 4 and tears hardware
/// down itself). Both must survive the racing no-op connect task; they
/// exercise different recording paths in the base (round-6 finding: the sync
/// route's record was skipped when the device was still connected, letting
/// the no-op task reconnect).
inline bool connected_then_connect_disconnect_settles_disconnected(
    AlpacaDriver& driver, bool use_sync_disconnect = false,
    std::chrono::milliseconds settle_budget = std::chrono::seconds(10)) {
    if (!settle_connected(driver, true)) {
        return true;  // could not reach the precondition; surface as failure
    }
    driver.connect();  // no-op connect task on a connected device
    if (use_sync_disconnect) {
        driver.set_connected(false);  // sync PUT path: records + tears down itself
    } else {
        driver.disconnect();  // async path: recorded against the in-flight task
    }
    const auto deadline = std::chrono::steady_clock::now() + settle_budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!driver.get_connecting() && !driver.get_connected()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return driver.get_connected();
}

/// Deterministic protocol check: a disconnect issued while a connect is in
/// flight must never be dropped — the device must settle Disconnected.
/// The deferred-disconnect tail runs AFTER the task publishes Idle (by
/// design), so this polls for the final state itself: a correct driver goes
/// (and stays) disconnected well inside the budget; a driver that drops the
/// racing disconnect sits Connected until the deadline and fails the assert.
/// Returns the settled connected state; the caller asserts it is false.
inline bool connect_then_disconnect_settles_disconnected(
    AlpacaDriver& driver, std::chrono::milliseconds settle_budget = std::chrono::seconds(10)) {
    driver.connect();
    driver.disconnect();
    const auto deadline = std::chrono::steady_clock::now() + settle_budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!driver.get_connecting() && !driver.get_connected()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return driver.get_connected();
}

}  // namespace alpacacore::test

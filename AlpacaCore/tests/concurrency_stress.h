// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

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
    int lifecycle_threads = 4;               // hammer connect()/disconnect()/set_connected()
    int op_threads = 4;                      // hammer the operate callback
    std::chrono::milliseconds duration{750}; // per-scenario wall clock
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
                        case 0: driver.connect(); break;
                        case 1: driver.disconnect(); break;
                        case 2: driver.set_connected(true); break;
                        case 3: driver.set_connected(false); break;
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
    return driver.get_connected() == want;
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

/// Deterministic protocol check: a disconnect issued while a connect is in
/// flight must never be dropped — the device must settle Disconnected.
/// The deferred-disconnect tail runs AFTER the task publishes Idle (by
/// design), so this polls for the final state itself: a correct driver goes
/// (and stays) disconnected well inside the budget; a driver that drops the
/// racing disconnect sits Connected until the deadline and fails the assert.
/// Returns the settled connected state; the caller asserts it is false.
inline bool connect_then_disconnect_settles_disconnected(AlpacaDriver& driver,
                                                         std::chrono::milliseconds settle_budget =
                                                             std::chrono::seconds(10)) {
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

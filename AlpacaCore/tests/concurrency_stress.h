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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <utility>
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
 * happen is a crash, a hang, or a TSan report. Wrap each call inside
 * `operate` with StressCallGuard (below) rather than a local try/catch, so a
 * fail-fast path doesn't have one throw silently skip the calls after it.
 */
struct StressOptions {
    int lifecycle_threads = 4;                // hammer connect()/disconnect()/set_connected()
    int op_threads = 4;                       // hammer the operate callback
    std::chrono::milliseconds duration{750};  // per-scenario wall clock
};

/**
 * Per-call guard for an operate callback (issue #322).
 *
 * `run_lifecycle_stress` below wraps the WHOLE operate callback in one
 * try/catch, not each call inside it — on a fail-fast path where every call
 * throws, the first throw skips every call after it, and the storm exercises
 * one line instead of nine. Every open `[stress]` registration PR at the time
 * of writing (#316-#320) therefore defined its own local
 *
 *     template <typename Fn> void call(Fn&& fn) { try { fn(); } catch (const std::exception&) {} }
 *
 * and the catch type has been argued in both directions by review, on the
 * same PR batch, within a day: widen to `std::exception` so a non-Alpaca
 * throw escaping teardown can't skip the rest of the callback, or narrow to
 * `AlpacaException` so an unexpected exception type isn't silently
 * indistinguishable from the expected `NotConnected`. Both are correct in
 * isolation and pull in opposite directions; a bare catch can't do both, but
 * a guard that RECORDS what it swallowed can:
 *
 * - An AlpacaException whose error_code() is in the expected set (NotConnected
 *   by default — a racing disconnect is what every registration expects to
 *   hit) is swallowed silently.
 * - An AlpacaException with any other code is swallowed but COUNTED.
 * - Any other std::exception is swallowed but COUNTED.
 * - A non-std::exception throw is not caught here. run_lifecycle_stress's own
 *   catch around the operate call is also `catch (const std::exception&)`, so
 *   this never reaches it either — it std::terminates the whole test binary,
 *   exactly as it would without this guard. This guard existing must not
 *   change that.
 *
 * A registration ends with
 *     INFO(guard.report());
 *     CHECK(guard.unexpected_count() == 0);
 * (CHECK has no message argument in Catch2 v2 or v3 -- INFO attaches
 * guard.report() to the next assertion's failure output instead, so a
 * failure names what it saw).
 * The constructor's expected_codes REPLACES the default {NotConnected}, it
 * does not add to it — pass {NotConnected, PropertyNotImplemented} (not just
 * {PropertyNotImplemented}) where the driver's contract needs a wider set
 * (e.g. a getter that answers without a connection), or every racing-
 * disconnect NotConnected in the storm is counted as a regression and the
 * case fails nondeterministically. So a widened set is a visible, complete
 * per-file decision, not a silent default. Thread-safe: op_threads hits this
 * concurrently.
 *
 * NotImplemented, PropertyNotImplemented and MethodNotImplemented all share
 * the same numeric AlpacaError code (alpaca_errors.h) -- opting into any ONE
 * of them for readability admits all three. Fine in practice (the three mean
 * closely related things), but don't read the expected set as more precise
 * than the codes actually are.
 *
 * Neither copyable nor movable (it owns a std::mutex) — a registration must
 * capture it by reference in the operate lambda, not by value.
 *
 * mutex_ is NOT held across fn(): operator() invokes it outside any lock and
 * takes mutex_ only inside record(), after fn() has returned or thrown. That
 * is deliberate -- holding it across the call would funnel every op_thread
 * through one lock and change what the storm actually exercises. So calling
 * report()/unexpected_count() from inside a guarded call does not deadlock,
 * and neither does nesting guarded calls; they just lock and unlock in turn.
 *
 * What IS true: a read taken while the storm is still running is a torn
 * snapshot, not a hang -- count_ and samples_ are consistent with each other
 * at that instant but say nothing about calls still in flight. Read them
 * after the threads join, which is the only point they mean anything.
 *
 * The constructor is explicit, so brace-init needs its own parens:
 *     StressCallGuard guard({AlpacaError::NotConnected, AlpacaError::InvalidValue});
 * not `StressCallGuard guard = {...};`, which won't compile.
 */
class StressCallGuard {
public:
    explicit StressCallGuard(std::initializer_list<int> expected_codes = {AlpacaError::NotConnected})
        : expected_codes_(expected_codes) {}

    template <typename Fn>
    void operator()(Fn&& fn) {
        try {
            std::forward<Fn>(fn)();
        } catch (const AlpacaException& ex) {
            if (is_expected(ex.error_code())) {
                return;
            }
            record_alpaca(ex.error_code(), ex.what());
        } catch (const std::exception& ex) {
            // typeid(ex).name() is the ABI-mangled name on libstdc++ (e.g.
            // "St12out_of_range", not "std::out_of_range") -- fine for a
            // CHECK message, but don't mistake the prefix in a report() line
            // for garbage output.
            record(typeid(ex).name(), ex.what());
        }
    }

    int unexpected_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    /// The first few recorded "<type>: <what()>" lines, newline-joined —
    /// meant for `INFO(guard.report());` immediately before the closing
    /// CHECK (see the class doc: CHECK takes no message argument), not for
    /// parsing.
    std::string report() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            if (i != 0) {
                out << '\n';
            }
            out << samples_[i];
        }
        if (count_ > static_cast<int>(samples_.size())) {
            out << "\n... and " << (count_ - static_cast<int>(samples_.size())) << " more";
        }
        return out.str();
    }

private:
    static constexpr std::size_t kMaxSamples = 8;

    bool is_expected(int code) const {
        for (int expected : expected_codes_) {
            if (expected == code) {
                return true;
            }
        }
        return false;
    }

    // Both overloads format INSIDE the lock, after the cap test, so a storm
    // that throws long past kMaxSamples pays a counter bump and nothing else.
    // Taking a ready-made std::string instead would move the formatting to
    // the call site, where it happens on every throw however full the sample
    // buffer is -- and an early return in here could not skip it, because the
    // argument is already built by then. Under TSan that allocation is
    // instrumented and the operate threads hit this path hard.
    void record(const char* type, const char* what) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        if (samples_.size() < kMaxSamples) {
            samples_.push_back(std::string(type) + ": " + what);
        }
    }

    void record_alpaca(int code, const char* what) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        if (samples_.size() < kMaxSamples) {
            samples_.push_back("AlpacaException(code=" + std::to_string(code) + "): " + what);
        }
    }

    // const, not just conventionally read-only: is_expected() reads this
    // without mutex_ (safe only because it's never written after
    // construction), and const makes that invariant load-bearing rather than
    // something a later "add an expect() mutator" change could quietly break
    // into a data race.
    const std::vector<int> expected_codes_;
    mutable std::mutex mutex_;
    int count_ = 0;
    std::vector<std::string> samples_;
};

// Documenting this pattern in THIS header is now safe: scripts/
// check_stress_registration.py reads AlpacaCore/tests/*.h as plain text, but
// it strips C and C++ comments first (issue #386), so an illustrative Catch2
// case macro written into a comment here is documentation rather than a stray
// registration. That was not true when this header was written -- the first
// draft of this comment spelled the macro out as an example and failed CI for
// a case that does not exist at runtime. String literals are NOT stripped, so
// an example built out of a real string constant still counts; keep examples
// in comments. The shape of a vendor registration, spelled out here now that
// it can be:
//
//     TEST_CASE("MyVendor camera lifecycle", "[myvendor][camera][stress]") {
//         alpacacore::test::StressCallGuard guard;
//         ...
//     }
//
// That example is also this repo's live proof that the comment filter works:
// it carries a real vendor-shaped tag string in a file the stray-[stress] rule
// scans, so the gate would fail on it if the filter were ever removed.

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

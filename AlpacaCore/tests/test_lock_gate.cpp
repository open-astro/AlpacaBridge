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

// Tests for util::LockGate and util::StateLock, the decision 0016 lock gate the
// OnStep and Sky-Watcher telescope drivers share (#1729, #1730).
//
// "Free" below means another thread can take the mutex at once: the test
// thread never try_locks a mutex it may own. Every wait on another thread has a
// real-time bound, so a defect fails the case instead of hanging the binary.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/lock_gate.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "catch2_compat.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::util::kLockGateTableSize;
using alpacacore::util::LockGate;
using alpacacore::util::StateLock;

namespace {

using std::chrono::milliseconds;

// One driver's two locks and its gate.
struct Driver {
    std::mutex io;
    std::mutex state;
    LockGate gate{io, state, "LockGateTest"};
};

// True when another thread can take `m` right now.
bool free_elsewhere(std::mutex& m) {
    return std::async(std::launch::async,
                      [&m] {
                          if (!m.try_lock()) {
                              return false;
                          }
                          m.unlock();
                          return true;
                      })
        .get();
}

// The error code `fn` throws as an AlpacaException, or -1 when it throws none.
template <typename F>
int alpaca_error_of(F&& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    }
    return -1;
}

}  // namespace

// Decision 0016 Rule 6 (#1730): with this thread's table full the gate cannot see what the thread holds, so
// an exchange would keep the state lock held. The OnStep and Sky-Watcher StateLock is this one, so this case
// is their full-table test too.
TEST_CASE("LockGate - a full lock table refuses before it takes any lock", "[lock_gate][unit]") {
    std::array<std::unique_ptr<Driver>, kLockGateTableSize> held_drivers;
    std::vector<StateLock> held;
    held.reserve(kLockGateTableSize);
    for (auto& driver : held_drivers) {
        driver = std::make_unique<Driver>();
        held.emplace_back(driver->gate, StateLock::Mode::State);
    }
    Driver ninth;

    // A StateLock in either mode refuses, holding nothing.
    CHECK(alpaca_error_of([&] { StateLock lock(ninth.gate); }) == AlpacaError::DriverException);
    CHECK(alpaca_error_of([&] { StateLock lock(ninth.gate, StateLock::Mode::State); }) == AlpacaError::DriverException);
    CHECK(free_elsewhere(ninth.io));
    CHECK(free_elsewhere(ninth.state));
    CHECK_FALSE(ninth.gate.holds_io());
    CHECK_FALSE(ninth.gate.holds_state());

    // One slot freed: the ninth gate locks.
    held.back().unlock();
    StateLock lock(ninth.gate, StateLock::Mode::State);
    CHECK(ninth.gate.holds_state());
}

TEST_CASE("LockGate - an exchange that throws gives the locks back as it found them", "[lock_gate][unit]") {
    Driver driver;

    // exchange() under a command's locks: the state lock is dropped for the call and back after the throw.
    {
        StateLock lock(driver.gate);
        bool dropped = false;
        CHECK_THROWS_AS(driver.gate.exchange([&] {
            dropped = !driver.gate.holds_state() && free_elsewhere(driver.state);
            throw std::runtime_error("wire failed");
        }),
                        std::runtime_error);
        CHECK(dropped);
        CHECK(driver.gate.holds_state());
        CHECK(driver.gate.holds_io());
        CHECK_FALSE(free_elsewhere(driver.state));
        CHECK_FALSE(free_elsewhere(driver.io));
    }
    CHECK_FALSE(driver.gate.holds_state());
    CHECK(free_elsewhere(driver.state));
    CHECK(free_elsewhere(driver.io));
}

TEST_CASE("LockGate - the operation lock is taken before the state lock", "[lock_gate][stress-guard]") {
    Driver driver;

    // A command waiting for the operation lock holds no state lock meanwhile.
    {
        std::unique_lock<std::mutex> io(driver.io);
        std::atomic<bool> starting{false};
        std::atomic<bool> locked{false};
        std::thread command([&] {
            starting = true;
            StateLock lock(driver.gate);
            locked = true;
        });
        while (!starting.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(milliseconds(50));
        CHECK_FALSE(locked.load());
        CHECK(free_elsewhere(driver.state));
        io.unlock();
        command.join();
        CHECK(locked.load());
    }
}

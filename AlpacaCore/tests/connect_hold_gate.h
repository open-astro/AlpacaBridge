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

// A connect hold the test releases itself (issue #1757). The fakes' connect holds (an open delay, a held
// handshake reply) call connect_hold(d) where they used to sleep d. With no ConnectHoldGate alive that is the
// same sleep. While one is alive and not released, connect_hold() waits for the test's release() instead, and
// wait_entered() tells the test the connect has reached the hold. A case that must act inside the hold ("a
// Disconnect during a connect returns at once") waits for that before it acts: a Disconnect recorded before the
// connect thread reaches its driver's first cancel point drops the connect at once, so Connecting can read false
// right after disconnect() returns, and a timed hold can end in a scheduler pause. With both waits on the gate,
// no scheduler pause changes what the case sees. The wait is bounded by kCap: a driver whose disconnect
// joins the connect (the bug those cases pin) then returns after kCap, fails the case's timing check and does
// not deadlock the test.
//
// One gate per process at a time (each ctest entry is its own process); the hold sites are fake threads, so
// the state is process-wide, not per fake.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace alpacacore::test {

class ConnectHoldGate {
public:
    /// The longest a hold site waits for release(); far past every case's timing bound.
    static constexpr std::chrono::seconds kCap{5};

    ConnectHoldGate() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.armed = true;
        s.released = false;
        s.entered = 0;
    }
    ~ConnectHoldGate() {
        auto& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.armed = false;
            s.released = true;
        }
        s.cv.notify_all();
    }
    ConnectHoldGate(const ConnectHoldGate&) = delete;
    ConnectHoldGate& operator=(const ConnectHoldGate&) = delete;

    /// Ends every hold now waiting, and every later one while this gate lives.
    void release() {
        auto& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.released = true;
        }
        s.cv.notify_all();
    }

    /// True once a hold site is waiting on this gate (the connect is inside its hold), within `timeout`.
    bool wait_entered(std::chrono::milliseconds timeout) const {
        auto& s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        return s.cv.wait_for(lock, timeout, [&s] { return s.entered > 0; });
    }

    /// A fake's connect hold: `duration` with no gate alive, else until release() (at most kCap). A zero
    /// duration is no hold at all, gate or not: only a site the test configured to hold waits on the gate.
    template <class Rep, class Period>
    static void hold(std::chrono::duration<Rep, Period> duration) {
        if (duration.count() <= 0) return;
        auto& s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.armed || s.released) {
            lock.unlock();
            std::this_thread::sleep_for(duration);
            return;
        }
        ++s.entered;
        s.cv.notify_all();
        s.cv.wait_for(lock, kCap, [&s] { return s.released || !s.armed; });
    }

    /// A window-type hold (a streamer's start-up hold) reports that the connect is now waiting inside it, the
    /// counterpart of a blocking hold() call entering the gate.
    static void note_entered() {
        auto& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.armed || s.released) return;
            ++s.entered;
        }
        s.cv.notify_all();
    }

    /// True while a gate is alive and not released: a hold that is a window rather than one blocking call (a
    /// streamer's start-up hold) stays shut while this reads true.
    static bool holding() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.armed && !s.released;
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool armed = false;
        bool released = true;
        int entered = 0;
    };
    static State& state() {
        static State s;
        return s;
    }
};

/// The hold sites' call: see ConnectHoldGate.
template <class Rep, class Period>
void connect_hold(std::chrono::duration<Rep, Period> duration) {
    ConnectHoldGate::hold(duration);
}

}  // namespace alpacacore::test

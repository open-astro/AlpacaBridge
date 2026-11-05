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

// A transmit queue that never empties (#2040): what TIOCOUTQ reports on a USB bridge whose endpoint has stopped
// taking data (a Q-Focuser enumerated without its 12 V supply, #2030; the CFW3's CP2102, #2038). A pty always
// drains, so a test installs this probe through util::tty_outq_probe_for_testing() for the length of a
// StalledOutqGuard; the guard removes it, and Catch2 runs cases one at a time in a process.

#include <alpacacore/util/serial_link.h>

#include <atomic>

namespace alpacacore::test {

/// How many times the stalled probe was asked since the last guard was made.
inline std::atomic<int>& stalled_outq_probe_calls() {
    static std::atomic<int> calls{0};
    return calls;
}

inline bool stalled_outq_probe(int, int& pending) {
    stalled_outq_probe_calls().fetch_add(1);
    pending = 5;
    return true;
}

struct StalledOutqGuard {
    StalledOutqGuard() {
        stalled_outq_probe_calls().store(0);
        alpacacore::util::tty_outq_probe_for_testing().store(&stalled_outq_probe);
    }
    ~StalledOutqGuard() { alpacacore::util::tty_outq_probe_for_testing().store(nullptr); }
    StalledOutqGuard(const StalledOutqGuard&) = delete;
    StalledOutqGuard& operator=(const StalledOutqGuard&) = delete;
};

}  // namespace alpacacore::test

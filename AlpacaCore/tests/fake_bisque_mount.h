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

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace alpacacore::test {

/**
 * Stateful TheSkyX stand-in for the Bisque driver tests (issue #865). Plug
 * responder() into a FakeMountServer: it keeps the parked and slewing state
 * the driver's JavaScript calls change, so a park or a slew runs to
 * completion without a canned reply per call.
 *
 * A park or slew is "in flight" until the driver has polled for it
 * `park_polls_needed` / `slew_polls_needed` times; a poll count of zero makes
 * the first poll report completion. fail_slew_polls > 0 makes that many
 * IsSlewComplete polls answer a TheSkyX error first.
 *
 * Counters are atomics: the server's handler thread writes them while the
 * test thread reads.
 */
class FakeBisqueMount {
public:
    std::atomic<int> park_polls_needed{0};
    std::atomic<int> slew_polls_needed{0};
    std::atomic<int> fail_slew_polls{0};

    /// What IsTracking answers (TheSkyX reports the tracking rate; 1 is on).
    std::atomic<bool> tracking{true};

    std::atomic<bool> parked{false};
    std::atomic<bool> slewing{false};
    std::atomic<int> park_commands{0};
    std::atomic<int> slew_commands{0};
    std::atomic<int> park_polls{0};
    std::atomic<int> slew_polls{0};
    std::atomic<int> position_reads{0};
    std::atomic<int> altaz_reads{0};

    std::function<std::string(const std::string&)> responder() {
        return [this](const std::string& command) { return answer(command); };
    }

private:
    static bool has(const std::string& command, const char* text) { return command.find(text) != std::string::npos; }

    std::string answer(const std::string& command) {
        if (has(command, "ConnectAndDoNotUnpark")) return "1#";
        if (has(command, "GetRaDec")) {
            ++position_reads;
            return "|No error. Error = 0.5.5,20.0#";
        }
        if (has(command, "GetAzAlt")) {
            ++altaz_reads;
            return "|No error. Error = 0.100.0,45.0#";
        }
        if (has(command, "ParkAndDoNotDisconnect")) {
            ++park_commands;
            park_polls = 0;
            parking_ = true;
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "IsParked")) {
            if (parking_ && ++park_polls > park_polls_needed.load()) {
                parking_ = false;
                parked = true;
            }
            return parked ? "|No error. Error = 0.true#" : "|No error. Error = 0.false#";
        }
        if (has(command, "SlewToRaDec")) {
            ++slew_commands;
            slew_polls = 0;
            slewing = true;
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "IsSlewComplete")) {
            if (fail_slew_polls.load() > 0) {
                --fail_slew_polls;
                return "|No error. Error = 1.IsSlewComplete unavailable#";
            }
            if (slewing && ++slew_polls > slew_polls_needed.load()) slewing = false;
            return slewing ? "|No error. Error = 0.0#" : "|No error. Error = 0.1#";
        }
        if (has(command, "IsTracking")) return tracking ? "|No error. Error = 0.1#" : "|No error. Error = 0.0#";
        return "|No error. Error = 0.OK#";
    }

    std::atomic<bool> parking_{false};
};

}  // namespace alpacacore::test

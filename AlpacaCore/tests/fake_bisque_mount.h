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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
 * `latency_ms` > 0 delays every reply by that many real milliseconds, and `transactions` counts every command the
 * fake answers (or drops), so a case can tell a getter that asked TheSkyX from one served from memory. `mute`
 * makes the fake drop every command after the handshake with no reply, as a TheSkyX that stopped answering.
 * `pier_west` is what the Pier Side query answers (1 = west of the pier).
 *
 * `find_home_ms`, `set_tracking_ms`, `position_ms`, `is_parked_ms` and `slew_complete_ms` delay only the FindHome,
 * SetTracking, GetRaDec, IsParked and IsSlewComplete replies by that many real milliseconds (on top of `latency_ms`),
 * as a TheSkyX stuck in one call.
 *
 * Sync(ra, dec) moves the position the fake reports to the synced coordinates. With `land_on_target` set, a
 * SlewToRaDec also moves it there once the slew reports complete, as a mount arriving on its target does (off by
 * default: most cases read the fixed position).
 *
 * A DirectGuide pulse adds `guide_ra_step_hours` to the reported RA.
 *
 * Motion safety (#2410): Abort() ends a slew in flight unless `ignore_aborts` > 0 (each Abort then drops the count
 * by one and the slew goes on); `fail_abort` answers Abort() with a TheSkyX error. `drop_slew_reply` takes a
 * SlewToRaDec and starts the slew but sends no reply; `reject_slew` answers it with a TheSkyX error and starts
 * nothing. DoCommand(9) (MoveAxis start) and DoCommand(10)
 * (its stop) are counted; `fail_move_stop` answers the stop with a TheSkyX error. `ledger()` lists the motion and
 * park commands in the order they came, as "Abort", "SlewToRaDec", "MoveStart", "MoveStop" and "IsParked".
 *
 * A park in flight reads as a slew to IsSlewComplete, as TheSkyX's Park docs say to monitor it. With
 * `abort_ends_park` set, an Abort() that is not ignored also ends it (off by default: the #1838 cases let the park
 * finish after an AbortSlew).
 *
 * Counters are atomics: the server's handler thread writes them while the
 * test thread reads.
 */
class FakeBisqueMount {
public:
    std::atomic<int> park_polls_needed{0};
    std::atomic<int> slew_polls_needed{0};
    std::atomic<int> fail_slew_polls{0};
    /// While > 0, every IsTracking query answers a TheSkyX error (and the count drops by one).
    std::atomic<int> fail_tracking_reads{0};

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
    // Models the remote computer clock, not chart JD; this fake does not execute JavaScript.
    std::atomic<int64_t> utc_millis{946684800123};
    std::atomic<bool> use_computer_clock{true};
    std::atomic<int> utc_reads{0};
    std::atomic<int> transactions{0};
    std::atomic<int> latency_ms{0};
    std::atomic<int> find_home_ms{0};
    std::atomic<int> set_tracking_ms{0};
    std::atomic<int> position_ms{0};
    std::atomic<int> is_parked_ms{0};
    std::atomic<int> slew_complete_ms{0};
    std::atomic<bool> mute{false};
    std::atomic<bool> reject_connection{false};
    std::atomic<bool> pier_west{false};
    std::atomic<double> ra_hours{5.5};
    std::atomic<double> dec_degrees{20.0};
    std::atomic<bool> land_on_target{false};
    /// Added to the reported RA by every DirectGuide pulse, as a guide pulse moves the mount.
    std::atomic<double> guide_ra_step_hours{0.0};
    std::atomic<int> abort_commands{0};
    std::atomic<int> ignore_aborts{0};
    std::atomic<bool> fail_abort{false};
    std::atomic<bool> drop_slew_reply{false};
    std::atomic<bool> reject_slew{false};
    std::atomic<int> move_starts{0};
    std::atomic<int> move_stops{0};
    std::atomic<bool> fail_move_stop{false};
    std::atomic<bool> abort_ends_park{false};

    std::vector<std::string> ledger() const {
        std::lock_guard<std::mutex> lock(ledger_mutex_);
        return ledger_;
    }

    std::function<std::string(const std::string&)> responder() {
        return [this](const std::string& command) { return answer(command); };
    }

private:
    static bool has(const std::string& command, const char* text) { return command.find(text) != std::string::npos; }

    void note(const char* entry) {
        std::lock_guard<std::mutex> lock(ledger_mutex_);
        ledger_.emplace_back(entry);
    }

    std::string answer(const std::string& command) {
        ++transactions;
        if (has(command, "ConnectAndDoNotUnpark")) return reject_connection ? "0#" : "1#";
        if (mute) return "";
        if (latency_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms.load()));
        if (has(command, ".Abort()")) {
            ++abort_commands;
            note("Abort");
            if (fail_abort) return "|No error. Error = 1.Abort refused#";
            if (ignore_aborts.load() > 0) {
                --ignore_aborts;
            } else {
                slewing = false;
                if (abort_ends_park) parking_ = false;
            }
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "DoCommand(9,")) {
            ++move_starts;
            note("MoveStart");
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "DoCommand(10,")) {
            ++move_stops;
            note("MoveStop");
            return fail_move_stop ? "|No error. Error = 1.stop refused#" : "|No error. Error = 0.OK#";
        }
        if (has(command, "sky6StarChart.DocumentProperty(5)") && has(command, "new Date().getTime()")) {
            ++utc_reads;
            return "|No error. Error = 0." +
                   (use_computer_clock ? std::to_string(utc_millis.load()) : std::string("unavailable")) + "#";
        }
        if (has(command, "FindHome")) {
            if (find_home_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(find_home_ms.load()));
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "SetTracking") && set_tracking_ms.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(set_tracking_ms.load()));
        }
        if (has(command, "GetRaDec")) {
            if (position_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(position_ms.load()));
            ++position_reads;
            return "|No error. Error = 0." + std::to_string(ra_hours.load()) + "," +
                   std::to_string(dec_degrees.load()) + "#";
        }
        if (has(command, "Pier Side")) return pier_west ? "|No error. Error = 0.1#" : "|No error. Error = 0.0#";
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
            note("IsParked");
            if (is_parked_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(is_parked_ms.load()));
            if (parking_ && ++park_polls > park_polls_needed.load()) {
                parking_ = false;
                parked = true;
            }
            return parked ? "|No error. Error = 0.true#" : "|No error. Error = 0.false#";
        }
        if (has(command, "Sync(")) {
            double ra = 0.0, dec = 0.0;
            if (std::sscanf(command.c_str() + command.find("Sync("), "Sync(%lf, %lf", &ra, &dec) == 2) {
                ra_hours = ra;
                dec_degrees = dec;
            }
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "SlewToRaDec")) {
            ++slew_commands;
            note("SlewToRaDec");
            if (reject_slew) return "|No error. Error = 1.Slew refused#";
            std::sscanf(command.c_str() + command.find("SlewToRaDec("), "SlewToRaDec(%lf, %lf", &target_ra_,
                        &target_dec_);
            slew_polls = 0;
            slewing = true;
            if (drop_slew_reply) return "";
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "IsSlewComplete")) {
            if (slew_complete_ms.load() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(slew_complete_ms.load()));
            }
            if (fail_slew_polls.load() > 0) {
                --fail_slew_polls;
                return "|No error. Error = 1.IsSlewComplete unavailable#";
            }
            if (slewing && ++slew_polls > slew_polls_needed.load()) {
                slewing = false;
                if (land_on_target) {
                    ra_hours = target_ra_;
                    dec_degrees = target_dec_;
                }
            }
            return slewing || parking_ ? "|No error. Error = 0.0#" : "|No error. Error = 0.1#";
        }
        if (has(command, "DirectGuide")) {
            ra_hours = ra_hours.load() + guide_ra_step_hours.load();
            return "|No error. Error = 0.OK#";
        }
        if (has(command, "IsTracking") && fail_tracking_reads.load() > 0) {
            --fail_tracking_reads;
            return "|No error. Error = 1.IsTracking unavailable#";
        }
        if (has(command, "IsTracking")) return tracking ? "|No error. Error = 0.1#" : "|No error. Error = 0.0#";
        return "|No error. Error = 0.OK#";
    }

    std::atomic<bool> parking_{false};
    mutable std::mutex ledger_mutex_;
    std::vector<std::string> ledger_;  // under ledger_mutex_
    double target_ra_ = 0.0;  // written and read on the server's handler thread only
    double target_dec_ = 0.0;
};

}  // namespace alpacacore::test

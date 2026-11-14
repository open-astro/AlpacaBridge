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

// Stateful SynScan hand controller for the SynScan telescope driver tests (issue #2230). Plug responder() into a
// FakeMountServer: it answers the handset dialect the driver's wrapper sends and parses
// (synscan_protocol_wrapper.cpp), with the replies the SynScan Serial Communication Protocol v3.3 gives
// (AlpacaCore/external/SynScan/synscanserialcommunicationprotocol_version33.md) and, where a rig measured more than the
// document says, the handset behaviour in .github/instructions/synscan.instructions.md. A command the fake does not
// model gets no reply, as in fake_zwo_mount.h, so a test that needs one times out visibly instead of reading a guessed
// "0#".
//
// A GOTO runs kGotoDuration on the TaskClock passed in (a FakeTaskClock in the virtual-time cases), so a case advances
// the clock instead of sleeping. The *_ms gates and the tracking-write hold are transport latency in real time; they
// never read the TaskClock.
//
// Counters and knobs are atomics: the server's handler thread writes them while the test thread reads.
//
// KNOWN PARITY GAPS (what the real handset does and this fake does not):
// - One position pair answers both the equatorial (e/E) and the Alt-Az (z/Z) reads, and a landed Alt-Az GOTO (b/B)
//   moves that same pair. The handset keeps two frames related by the site and the time.
// - The position does not track the sky: it changes only on a landed GOTO, a miss_gotos landing or set_position().
//   A Sync (s/S) is acknowledged and changes nothing; on the handset it re-labels the position.
// - A GOTO enables tracking on the handset if it was stopped (protocol v3.3, "GOTO Commands" note); here the tracking
//   mode changes only on a T write or an RA variable-rate move.
// - Fixed-rate moves (P chr(2) ... 36/37) and Dec variable-rate moves are acknowledged and move nothing; an RA
//   variable-rate move replaces tracking (mode 0) but does not end a GOTO in flight, which only M does.
// - The pointing state (p) is a knob (pointing_state), not derived from the position and the meridian.
// - The clock (h) always answers 12:00:00 on 1 Jan 2026, UTC, and an H write does not change it.
// - The site (w) always answers 0/0; a W write does not change it.
// - J (alignment), the motor-controller version passthrough (P ... 254) and the slow-GOTO / set-position passthroughs
//   (P chr(4) ... 23 / 4) are not modelled: they get no reply. The driver under test does not send them.

#ifndef _WIN32

#include <alpacacore/util/task_clock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fake_mount_server.h"

namespace alpacacore::test {

/// Whether a GOTO moves the position the handset reports. No default: each test file states the scenario it runs.
enum class SynScanGotoLanding : std::uint8_t {
    OnTarget,  ///< a GOTO lands on its aim, so the driver's landing check (#1105) sees an arrival
    Stays,     ///< a GOTO is acknowledged and timed, but the reported position never moves
};

class FakeSynScanHandset {
public:
    using Clock = std::chrono::steady_clock;

    /// Virtual time a GOTO reports "in progress" on L.
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    /// RA counts (of 2^32) a missed GOTO lands past its aim: the ~49' miss of #1105.
    static constexpr std::uint32_t kMissCounts = 9764864;  // 2,947"

    /// @param clock the TaskClock the driver under test reads (the GOTO timer reads it too)
    /// @param landing whether a GOTO moves the reported position (no default: the scenario is the caller's)
    /// @param start_position the e/E/z/Z reply before any GOTO lands, "RRRRRRRR,DDDDDDDD#"
    FakeSynScanHandset(util::TaskClock& clock, SynScanGotoLanding landing, std::string start_position)
        : clock_(clock), position_(std::move(start_position)) {
        land_on_goto.store(landing == SynScanGotoLanding::OnTarget);
    }

    FakeSynScanHandset(const FakeSynScanHandset&) = delete;
    FakeSynScanHandset& operator=(const FakeSynScanHandset&) = delete;

    /// Plug into a FakeMountServer. The handset must outlive the server.
    FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) { return answer(chunk); };
    }

    // ---- State the handset holds ------------------------------------------------------------------------------

    /// The `T` mode the handset holds: `t` reads it back, `T` writes it. A test sets it directly to stand in for the
    /// keypad changing it outside the driver.
    std::atomic<int> tracking_mode{1};
    /// The `m` reply: 50 is the EQM-35 Pro (equatorial), 5/6 AZ-EQ mounts, 128 an AZ GOTO.
    std::atomic<unsigned char> model_id{50};
    /// The `p` reply ('E' or 'W', protocol v3.3 "Get Mount Pointing State"); 'E' is a northern mount, not flipped.
    std::atomic<char> pointing_state{'E'};
    /// A GOTO moves the reported position (SynScanGotoLanding::OnTarget). A case may change it mid-run.
    std::atomic<bool> land_on_goto{false};

    // ---- Fault knobs, all off by default ------------------------------------------------------------------------

    /// The handset answers nothing at all, every command (a handset that stopped answering).
    std::atomic<bool> silent{false};
    /// Position reads go unanswered once a GOTO was seen.
    std::atomic<bool> silent_position{false};
    /// A GOTO is acknowledged, but the mount stays put and L says idle.
    std::atomic<bool> no_motion{false};
    /// While set, a GOTO that started never ends, so a test controls exactly when L sees it end.
    std::atomic<bool> goto_never_ends{false};
    /// The next N precise GOTOs land kMissCounts RA counts past their aim.
    std::atomic<int> miss_gotos{0};
    /// Every GOTO goes unanswered (the wrapper times out and throws) and sets goto_held (#742: the test's cue).
    std::atomic<bool> reject_goto{false};
    std::atomic<bool> goto_held{false};
    /// >0: the next GOTO is held this long (real time) before it is answered; gate_entered / gate_left stamp it.
    std::atomic<int> gate_ms{0};
    std::atomic<Clock::rep> gate_entered{0};
    std::atomic<Clock::rep> gate_left{0};
    /// L answers "1" (slewing) whatever the GOTO timer says.
    std::atomic<bool> report_slewing{false};
    /// Every L answers "?" (neither 0 nor 1): the wrapper's slew-status read throws.
    std::atomic<bool> invalid_slew_status{false};
    /// The next N L queries go unanswered (a dropped reply), then the handset answers again.
    std::atomic<int> drop_slew_status{0};
    /// >0: the next L is held this long (real time), then answered "0"; gate_l_entered stamps it.
    std::atomic<int> gate_l_ms{0};
    std::atomic<Clock::rep> gate_l_entered{0};
    /// Every T write goes unanswered (the wrapper times out).
    std::atomic<bool> reject_tracking{false};
    /// A T write of mode 0 goes unanswered; tracking_stop_attempts counts those writes.
    std::atomic<bool> reject_tracking_stop{false};
    std::atomic<int> tracking_stop_attempts{0};
    /// The next T write is parked inside the handset until release_tracking_write().
    std::atomic<bool> hold_tracking_write{false};
    /// Every t read goes unanswered (the wrapper times out).
    std::atomic<bool> reject_tracking_read{false};
    /// >0: the next t read is held this long (real time) before it is answered; gate_t_entered stamps it.
    std::atomic<int> gate_t_ms{0};
    std::atomic<Clock::rep> gate_t_entered{0};
    /// >0: the next s/S sync is held this long (real time) before it is answered.
    std::atomic<int> gate_s_ms{0};
    std::atomic<Clock::rep> gate_s_entered{0};
    /// >0: the next W site write / H time write is held this long (real time) before it is answered.
    std::atomic<int> gate_w_ms{0};
    std::atomic<Clock::rep> gate_w_entered{0};
    std::atomic<int> gate_h_ms{0};
    std::atomic<Clock::rep> gate_h_entered{0};
    /// The w site read goes unanswered, so the driver has no site unless config or a client supplies one.
    std::atomic<bool> no_location{false};
    /// An RA variable-rate move leaves the handset's tracking mode alone. Not the handset's behaviour (it replaces
    /// tracking); one slot case keeps it while the driver defect it exposes is open (see that case).
    std::atomic<bool> ra_move_keeps_tracking{false};
    /// The command log records the status poll's reads (e, t) as well as every other logged prefix. A case that
    /// counts the whole log across a status poll turns it off.
    std::atomic<bool> log_status_reads{true};

    // ---- Observations -------------------------------------------------------------------------------------------

    /// Every command received, answered or not.
    std::atomic<int> requests{0};
    /// GOTOs accepted (answered).
    std::atomic<int> goto_count{0};
    /// True from an accepted GOTO until the next M.
    std::atomic<bool> goto_seen{false};
    /// Every L query, answered or not.
    std::atomic<int> slew_status_polls{0};

    /// Log of the commands whose first byte is one of kLoggedPrefixes, in wire order. `command_mutex` guards it.
    static constexpr const char* kLoggedPrefixes = "PTMrRets";
    std::mutex command_mutex;
    std::vector<std::string> commands;

    int command_count(char prefix) {
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [prefix](const std::string& command) {
            return !command.empty() && command[0] == prefix;
        }));
    }
    std::vector<std::string> command_snapshot() {
        std::lock_guard<std::mutex> lock(command_mutex);
        return commands;
    }
    /// Every accepted GOTO command (r, R, b, B), in order.
    std::vector<std::string> goto_snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return gotos_;
    }

    void set_position(std::string value) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_ = std::move(value);
    }

    /// True once a held tracking write has reached the handset (5 s real-time bound).
    bool wait_tracking_write_held() {
        std::unique_lock<std::mutex> lock(hold_mutex_);
        return hold_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return tracking_write_held_; });
    }
    void release_tracking_write() {
        std::lock_guard<std::mutex> lock(hold_mutex_);
        tracking_write_released_ = true;
        hold_cv_.notify_all();
    }

    bool goto_in_progress() const {
        if (!goto_seen.load() || no_motion.load()) return false;
        if (goto_never_ends.load()) return true;
        const auto started = Clock::time_point(Clock::duration(goto_started_.load()));
        return clock_.now() - started < kGotoDuration;
    }

private:
    static void hold_for(std::atomic<int>& gate, std::atomic<Clock::rep>* entered, std::atomic<Clock::rep>* left) {
        const int hold = gate.exchange(0);
        if (hold <= 0) return;
        if (entered) entered->store(Clock::now().time_since_epoch().count());
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        if (left) left->store(Clock::now().time_since_epoch().count());
    }

    void record(const std::string& chunk) {
        if (std::string(kLoggedPrefixes).find(chunk[0]) == std::string::npos) return;
        if ((chunk[0] == 'e' || chunk[0] == 't') && !log_status_reads.load()) return;
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.push_back(chunk);
    }

    void park_tracking_write() {
        std::unique_lock<std::mutex> lock(hold_mutex_);
        tracking_write_held_ = true;
        hold_cv_.notify_all();
        hold_cv_.wait(lock, [this] { return tracking_write_released_; });
    }

    // The aim of a GOTO as a position reply. Precise "rRRRRRRRR,DDDDDDDD"; a 16-bit "RRRRR,DDDD" is widened to 32 bits.
    static std::string aim_of(const std::string& chunk) {
        if (chunk.size() >= 18 && chunk[9] == ',') return chunk.substr(1, 17) + "#";
        if (chunk.size() >= 10 && chunk[5] == ',') return chunk.substr(1, 4) + "0000," + chunk.substr(6, 4) + "0000#";
        return {};
    }

    void land(const std::string& chunk) {
        std::string aim = aim_of(chunk);
        if (aim.empty()) return;
        if (miss_gotos.load() > 0 && aim.size() == 18) {
            miss_gotos.fetch_sub(1);
            char ra[9];
            std::snprintf(ra, sizeof ra, "%08X",
                          static_cast<unsigned>(std::stoul(aim.substr(0, 8), nullptr, 16)) + kMissCounts);
            aim = std::string(ra) + aim.substr(8);
        }
        position_ = std::move(aim);
    }

    std::string answer(const std::string& chunk) {
        requests.fetch_add(1);
        if (silent.load() || chunk.empty()) return "";
        record(chunk);
        switch (chunk[0]) {
            case 'K':  // echo: "K" & chr(x) -> chr(x) & "#" (the connect-time link check)
                return chunk.size() > 1 ? std::string(1, chunk[1]) + "#" : std::string();
            case 'V':  // firmware: six hex digits & "#" (04.42.00)
                return "042A00#";
            case 'm':  // model: chr(model) & "#"
                return std::string(1, static_cast<char>(model_id.load())) + "#";
            case 'p':  // pointing state: "E" or "W" & "#"
                return std::string(1, pointing_state.load()) + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z': {
                if (silent_position.load() && goto_seen.load()) return "";
                std::lock_guard<std::mutex> lock(mutex_);
                return position_;
            }
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                if (reject_goto.load()) {
                    goto_held.store(true);
                    return "";  // no reply at all: the response timeout fires inside the slew task
                }
                hold_for(gate_ms, &gate_entered, &gate_left);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    gotos_.push_back(chunk);
                    if (land_on_goto.load() && !no_motion.load()) land(chunk);
                }
                goto_started_.store(clock_.now().time_since_epoch().count());
                goto_seen.store(true);
                goto_count.fetch_add(1);
                return "#";
            case 'L': {  // GOTO in progress: ASCII "0" or "1" & "#"
                slew_status_polls.fetch_add(1);
                const bool held = gate_l_ms.load() > 0;
                hold_for(gate_l_ms, &gate_l_entered, nullptr);
                if (drop_slew_status.load() > 0) {
                    drop_slew_status.fetch_sub(1);
                    return "";
                }
                if (invalid_slew_status.load()) return "?#";
                if (held) return "0#";
                return report_slewing.load() || goto_in_progress() ? "1#" : "0#";
            }
            case 'M':  // cancel GOTO (protocol v3.3): the GOTO ends at once
                goto_seen.store(false);
                return "#";
            case 't':  // tracking mode: chr(mode) & "#"
                if (reject_tracking_read.load()) return "";
                hold_for(gate_t_ms, &gate_t_entered, nullptr);
                return std::string(1, static_cast<char>(tracking_mode.load())) + "#";
            case 'T':  // set tracking mode: "T" & chr(mode) -> "#"
                if (reject_tracking.load()) return "";
                if (reject_tracking_stop.load() && chunk.size() > 1 && chunk[1] == 0) {
                    tracking_stop_attempts.fetch_add(1);
                    return "";
                }
                if (hold_tracking_write.exchange(false)) park_tracking_write();
                if (chunk.size() > 1) tracking_mode.store(static_cast<unsigned char>(chunk[1]));
                return "#";
            case 'P':  // passthrough: "P" & chr(len) & chr(dev) & chr(op) & ...
                // A variable-rate move on the RA motor is the motor's own speed and replaces tracking on the handset,
                // and so does its zero-rate stop (EQM-35 Pro, HC 06.03.00, synscan.instructions.md "Pulse guiding").
                if (chunk.size() >= 6 && chunk[1] == 3 && static_cast<unsigned char>(chunk[2]) == 16 &&
                    !ra_move_keeps_tracking.load()) {
                    tracking_mode.store(0);
                }
                return "#";
            case 's':  // sync: "#"
            case 'S':
                hold_for(gate_s_ms, &gate_s_entered, nullptr);
                return "#";
            case 'h':  // time: eight binary bytes & "#": 12:00:00 on 1 Jan 2026, UTC, no DST
                return std::string("\x0c\x00\x00\x01\x01\x1a\x00\x00", 8) + "#";
            case 'H':  // set time: "#"
                hold_for(gate_h_ms, &gate_h_entered, nullptr);
                return "#";
            case 'w':  // site: eight binary bytes & "#" (0/0)
                if (no_location.load()) return "";
                return std::string(8, '\0') + "#";
            case 'W':  // set site: "#"
                hold_for(gate_w_ms, &gate_w_entered, nullptr);
                return "#";
            default:
                return "";  // not modelled: no reply, so a case that needs one times out visibly
        }
    }

    util::TaskClock& clock_;
    std::atomic<Clock::rep> goto_started_{0};
    std::mutex mutex_;  // position_, gotos_
    std::string position_;
    std::vector<std::string> gotos_;
    std::mutex hold_mutex_;
    std::condition_variable hold_cv_;
    bool tracking_write_held_ = false;
    bool tracking_write_released_ = false;
};

}  // namespace alpacacore::test

#endif  // !_WIN32

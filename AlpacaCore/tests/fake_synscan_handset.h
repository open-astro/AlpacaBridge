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
// Two modes. The scripted default (the constructor with a landing and a start position) is what the driver cases
// use. ConformU mode (the constructor with a FakeMountMotionModel::Config, #2692) answers from one
// FakeMountMotionModel instead: a GOTO moves the position over its travel time, a Sync re-labels it, tracking
// follows the sky, a variable-rate or fixed-rate P move moves its axis, and the site and clock reads and writes are
// kept. The KNOWN PARITY GAPS below are the scripted default's, except where a line says otherwise.
//
// KNOWN PARITY GAPS (what the real handset does and this fake does not):
// - ConformU mode only: an Alt-Az GOTO (b/B) is not modelled and gets no reply; a fixed-rate P move runs its axis
//   at a table rate and, unlike the handset above rate 2, does not replace tracking; a horizon is not refused (the
//   protocol has no refusal reply).
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
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fake_mount_motion_model.h"
#include "fake_mount_server.h"
#include "nexstar_hc_wire.h"

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

    /// ConformU mode (#2692): answers from a FakeMountMotionModel built from @p motion (see the file comment). The
    /// knobs that script the position (land_on_goto, no_motion, goto_never_ends, miss_gotos, drift_counts,
    /// set_position()) do not apply; the fault knobs (silent, reject_goto, gates, ...) do.
    /// @param clock the TaskClock the driver under test reads
    FakeSynScanHandset(util::TaskClock& clock, FakeMountMotionModel::Config motion)
        : clock_(clock), motion_(std::make_unique<FakeMountMotionModel>(std::move(motion))) {}

    /// The motion model of a ConformU-mode handset; null for the scripted default.
    FakeMountMotionModel* motion() const { return motion_.get(); }

    /// The model setup of the fake ConformU row: the made-up Sydney Opera House site (never a real observing site)
    /// and a 4 degree/s GOTO, about the top fixed rate of an EQM-35 class mount. Park and home are the driver's own
    /// GOTO targets here (the handset has no park command), so the model's are unused.
    static FakeMountMotionModel::Config conformu_motion_config() {
        FakeMountMotionModel::Config config;
        config.latitude_degrees = -33.856784;
        config.longitude_degrees = 151.215297;
        config.slew_deg_per_s = 4.0;
        return config;
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
    /// The next N cancels (M) are answered and logged but ignored, so a GOTO keeps running through them (#2412).
    std::atomic<int> ignore_cancels{0};
    /// Non-zero: each position read of the equatorial pair moves RA by this many counts, a mount that keeps moving
    /// through any stop (#2412). A precise ('e') read moves it 24-bit counts (about 77 arcsec per 1000); a 16-bit ('E')
    /// read moves it 16-bit counts (about 79 arcsec per 4). drift_reads counts the reads that drifted.
    std::atomic<int> drift_counts{0};
    std::atomic<int> drift_reads{0};
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
    static constexpr const char* kLoggedPrefixes = "PTMrRetsWH";
    std::mutex command_mutex;
    /// Runs on the handler thread before each command is answered (a test lands a command mid-exchange with it).
    /// Guarded by before_reply_mutex; the hook runs without it.
    std::mutex before_reply_mutex;
    std::function<void(const std::string&)> before_reply;
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
        if (motion_) return motion_->slewing();
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
        {
            std::function<void(const std::string&)> hook;
            {
                std::lock_guard<std::mutex> lock(before_reply_mutex);
                hook = before_reply;
            }
            if (hook) hook(chunk);
        }
        if (silent.load() || chunk.empty()) return "";
        record(chunk);
        if (motion_) {
            if (auto reply = answer_modelled(chunk)) return *reply;
        }
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
                if ((chunk[0] == 'e' || chunk[0] == 'E') && drift_counts.load() != 0) {
                    char moving[32];
                    const auto reads = static_cast<unsigned>(drift_reads.fetch_add(1) + 1);
                    const auto counts = static_cast<unsigned>(drift_counts.load());
                    if (chunk[0] == 'E') {
                        // 16-bit: the driver reads the last four digits before the comma and the first four after it.
                        std::snprintf(moving, sizeof(moving), "0000%04X,20000000#",
                                      (0x0500u + reads * counts) & 0xFFFFu);
                    } else {
                        std::snprintf(moving, sizeof(moving), "%06X00,20000500#",
                                      (0x12AB05u + reads * counts) & 0xFFFFFFu);
                    }
                    return moving;
                }
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
                    if (motion_) {
                        if (chunk[0] == 'b' || chunk[0] == 'B') return "";  // Alt-Az GOTO: not modelled
                        start_modelled_goto(chunk);
                    } else if (land_on_goto.load() && !no_motion.load()) {
                        land(chunk);
                    }
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
                if (ignore_cancels.load() > 0) {
                    ignore_cancels.fetch_sub(1);
                    return "#";
                }
                goto_seen.store(false);
                if (motion_) motion_->stop();
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
                if (motion_ && chunk.size() > 1) motion_->set_tracking(chunk[1] != 0);
                return "#";
            case 'P':  // passthrough: "P" & chr(len) & chr(dev) & chr(op) & ...
                // A variable-rate move on the RA motor is the motor's own speed and replaces tracking on the handset,
                // and so does its zero-rate stop (EQM-35 Pro, HC 06.03.00, synscan.instructions.md "Pulse guiding").
                if (chunk.size() >= 6 && chunk[1] == 3 && static_cast<unsigned char>(chunk[2]) == 16) {
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

    // ---- ConformU mode: the commands the motion model answers (#2692) ----

    // A GOTO also resumes tracking on the handset if it was stopped (protocol v3.3, "GOTO Commands" note).
    void start_modelled_goto(const std::string& chunk) {
        double ra_degrees = 0.0;
        double dec_degrees = 0.0;
        if (!hc_wire::parse_aim(chunk, ra_degrees, dec_degrees)) return;
        if (tracking_mode.load() == 0) tracking_mode.store(2);
        motion_->set_tracking(true);
        (void)motion_->goto_radec(ra_degrees / 15.0, dec_degrees);
    }

    // The variable-rate P move on the RA motor (dev 16) or Dec motor (dev 17), "P" 3 dev code hi lo 0 0. Code 6 is
    // positive; on RA the westward code is 7 at a southern site and 6 north of the equator (synscan.instructions.md
    // "Pulse guiding"), and the move REPLACES tracking, so the model's tracking goes off with it. On Dec the motor runs
    // the other way when the mount points west (pointing state 'E' is the OTA west of the pier).
    void variable_rate_move(const std::string& chunk) {
        using Axis = FakeMountMotionModel::Axis;
        const int dev = static_cast<unsigned char>(chunk[2]);
        const int code = static_cast<unsigned char>(chunk[3]);
        const double rate = ((static_cast<unsigned char>(chunk[4]) << 8) | static_cast<unsigned char>(chunk[5])) / 4.0 /
                            3600.0;  // deg/s
        const double positive = code == 6 ? rate : -rate;
        if (dev == 16) {
            const double west_sign = motion_->site_latitude_degrees() < 0.0 ? -1.0 : 1.0;
            tracking_mode.store(0);
            motion_->set_tracking(false);
            motion_->move_axis(Axis::Ra, -west_sign * positive);  // positive motor = west on a northern site
        } else if (dev == 17) {
            const bool pier_flipped = motion_->pier_side() == 1;  // OTA west of the pier, pointing state 'E'
            motion_->move_axis(Axis::Dec, pier_flipped ? positive : -positive);
        }
    }

    void fixed_rate_move(const std::string& chunk) {
        using Axis = FakeMountMotionModel::Axis;
        const int dev = static_cast<unsigned char>(chunk[2]);
        const int code = static_cast<unsigned char>(chunk[3]);
        const int rate = std::min<int>(static_cast<unsigned char>(chunk[4]), 9);
        const double deg_per_s =
            hc_wire::kFixedRateSidereal[rate] * FakeMountMotionModel::kSiderealDegPerSec * (code == 36 ? 1.0 : -1.0);
        if (dev == 16) {
            motion_->move_axis(Axis::Ra, -deg_per_s);  // positive motor = west
        } else if (dev == 17) {
            motion_->move_axis(Axis::Dec, deg_per_s);
        }
    }

    // nullopt: the command is answered by the shared switch below (the echo, the version, the model, the faults).
    std::optional<std::string> answer_modelled(const std::string& chunk) {
        switch (chunk[0]) {
            case 'e':
            case 'E': {
                if (silent_position.load() && goto_seen.load()) return std::string();
                const auto s = motion_->state();
                return hc_wire::axis_pair(s.ra_hours * 15.0, s.dec_degrees, chunk[0] == 'e');
            }
            case 'z':
            case 'Z':
                if (silent_position.load() && goto_seen.load()) return std::string();
                return hc_wire::axis_pair(motion_->azimuth_degrees(), motion_->altitude_degrees(), chunk[0] == 'z');
            case 'p':  // 'W' pointing west = OTA east of the pier (hour angle >= 0), see the driver's pier mapping
                return std::string(1, motion_->pier_side() == 0 ? 'W' : 'E') + "#";
            case 's':
            case 'S': {
                hold_for(gate_s_ms, &gate_s_entered, nullptr);
                double ra_degrees = 0.0;
                double dec_degrees = 0.0;
                if (hc_wire::parse_aim(chunk, ra_degrees, dec_degrees))
                    motion_->set_position(ra_degrees / 15.0, dec_degrees);
                return std::string("#");
            }
            case 'P':
                if (chunk.size() >= 6 && chunk[1] == 3)
                    variable_rate_move(chunk);
                else if (chunk.size() >= 5 && chunk[1] == 2)
                    fixed_rate_move(chunk);
                return std::string("#");
            case 'w':
                if (no_location.load()) return std::string();
                return hc_wire::site_bytes(*motion_) + "#";
            case 'W':
                hold_for(gate_w_ms, &gate_w_entered, nullptr);
                hc_wire::apply_site(*motion_, chunk);
                return std::string("#");
            case 'h':
                return hc_wire::time_bytes(*motion_, clock_offset_s_.load()) + "#";
            case 'H':
                hold_for(gate_h_ms, &gate_h_entered, nullptr);
                hc_wire::apply_time(*motion_, chunk, clock_offset_s_);
                return std::string("#");
            default:
                return std::nullopt;
        }
    }

    util::TaskClock& clock_;
    std::unique_ptr<FakeMountMotionModel> motion_;  // ConformU mode only
    std::atomic<std::int64_t> clock_offset_s_{0};   // handset clock less the model's, set by H
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

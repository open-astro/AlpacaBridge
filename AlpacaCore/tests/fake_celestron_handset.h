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

// Stateful Celestron NexStar hand controller for the NexStar telescope driver tests (issue #2230). Plug responder()
// into a FakeMountServer: it answers the hand-controller and AUX pass-through commands the driver's wrapper sends and
// parses (celestron_protocol_wrapper.cpp), with the replies in
// AlpacaCore/external/Celestron/nexstar_protocol_reference.md. A hand-controller command the fake does not model gets
// no reply, as in fake_zwo_mount.h, so a test that needs one times out visibly instead of reading a guessed "0#". An
// AUX pass-through the fake does not model gets one garbage byte before the '#', which is what the reference gives for
// an unknown command or an absent device ("Pass-Through Command Format", error detection).
//
// A GOTO (HC r/R/b/B or AUX MC_GOTO_FAST / MC_GOTO_SLOW) runs kGotoDuration on the TaskClock passed in (a
// FakeTaskClock in the virtual-time cases), so a case advances the clock instead of sleeping. The *_ms gates and holds
// are transport latency in real time; they never read the TaskClock.
//
// Counters and knobs are atomics: the server's handler thread writes them while the test thread reads.
//
// Two modes. The scripted default (the constructor with a tracking state and a position) is what the driver cases
// use. ConformU mode (the constructor with a FakeMountMotionModel::Config, #2692) answers from one
// FakeMountMotionModel instead: a GOTO (HC r/R, or the AUX MC_GOTO pair) moves the position over its travel time, a
// Sync re-labels it, tracking follows the sky, MTR_AUX_GUIDE pulses and variable-rate moves move their axis, and the
// site and clock reads and writes are kept. The KNOWN PARITY GAPS below are the scripted default's, except where a
// line says ConformU mode.
//
// KNOWN PARITY GAPS (what the real hand controller does and this fake does not):
// - ConformU mode: an Alt-Az GOTO (b/B) is not modelled and gets no reply. A variable- or fixed-rate move runs its axis
//   on top of tracking (RA positive raises RA, Dec positive is north on either pier); which way a real CGX-L runs for
//   each code and whether the move replaces tracking are not measured. A GOTO leaves the RA motor at rate 0 until a
//   `T` write (`goto_stops_tracking`, CGX-L fw 7.18: "axis 0 stays at rate=0 after goto completion", see
//   restore_tracking_after_slew in the driver), whatever `t` says. A horizon is not refused.
// - The reported position moves only on a landed GOTO (celestron_landing_model.h, #2409: the HC GOTO r/R, or a
//   pass-through motor GOTO pair) or set_position(); miss_landing keeps it, a GOTO that came to rest far from its
//   target. e/E/z/Z answer one pair for both frames. A Sync (s/S) is acknowledged and re-labels nothing.
// - The tracking mode is a knob (tracking_on): `t` answers EQ North or Off from it, and a `T` write is counted but not
//   read back. The hand controller reports the mode last written.
// - Every AUX address answers GET_VER (0xFE), so the connect-time bus probe finds every device (the GPS module
//   included); a real bus answers only the devices fitted. The DEC autoguider port (0x32) answers 1.0, as on a CGX-L
//   (reference, "Bus Enumeration"); every other address answers 0.0.
// - MTR_AUX_GUIDE (0x26) is acknowledged and moves nothing; MTR_IS_AUX_GUIDE_ACTIVE (0x27), the autoguide-rate,
//   position, set-position, PEC and GPS pass-throughs get the unknown-command garbage byte.
// - Rate moves (fixed 36/37, variable 6/7) are acknowledged and move nothing; they do not end a GOTO.
// - The clock (h) always answers 12:00:00 on 1 Jan 2026, UTC; the site (w) always answers 0/0; writes change neither.
// - J (alignment) answers aligned unless `aligned` is false, held for j_hold_ms when set.

#ifndef _WIN32

#include <alpacacore/util/task_clock.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "celestron_landing_model.h"
#include "fake_mount_motion_model.h"
#include "fake_mount_server.h"
#include "nexstar_hc_wire.h"

namespace alpacacore::test {

/// What the `t` query reports when the case starts. No default: each test file states the scenario it runs.
enum class CelestronTracking : std::uint8_t {
    Off,      ///< `t` answers 0
    EqNorth,  ///< `t` answers 2
};

class FakeCelestronHandset {
public:
    using Clock = std::chrono::steady_clock;

    /// Virtual time a GOTO reports "in progress" on L and MC_SLEW_DONE.
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);

    /// @param clock the TaskClock the driver under test reads (the GOTO timer reads it too)
    /// @param tracking what `t` answers until a case changes tracking_on (no default: the scenario is the caller's)
    /// @param position the e/E/z/Z reply, "RRRRRRRR,DDDDDDDD#"
    FakeCelestronHandset(util::TaskClock& clock, CelestronTracking tracking, std::string position)
        : clock_(clock), landing_(without_hash(std::move(position))) {
        tracking_on.store(tracking == CelestronTracking::EqNorth);
    }

    /// ConformU mode (#2692): answers from a FakeMountMotionModel built from @p motion (see the file comment). The
    /// knobs that script the position (tracking_on, miss_landing, unsettled, no_motion, goto_never_ends,
    /// set_position()) do not apply; the fault knobs (silent, reject_goto, gates, ...) do.
    /// @param clock the TaskClock the driver under test reads
    FakeCelestronHandset(util::TaskClock& clock, FakeMountMotionModel::Config motion)
        : clock_(clock), landing_(""), motion_(std::make_unique<FakeMountMotionModel>(std::move(motion))) {}

    /// The motion model of a ConformU-mode handset; null for the scripted default.
    FakeMountMotionModel* motion() const { return motion_.get(); }

    /// The model setup of the fake ConformU row: the made-up Sydney Opera House site (never a real observing site) and
    /// a 4 degree/s GOTO. Park and home are the driver's own GOTO and level-start targets, so the model's are unused.
    static FakeMountMotionModel::Config conformu_motion_config() {
        FakeMountMotionModel::Config config;
        config.latitude_degrees = -33.856784;
        config.longitude_degrees = 151.215297;
        config.slew_deg_per_s = 4.0;
        return config;
    }

    FakeCelestronHandset(const FakeCelestronHandset&) = delete;
    FakeCelestronHandset& operator=(const FakeCelestronHandset&) = delete;

    /// Plug into a FakeMountServer. The handset must outlive the server.
    FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) { return answer(chunk); };
    }

    // ---- State the hand controller holds ------------------------------------------------------------------------

    /// `t` answers EQ North (2) when on, Off (0) when not.
    std::atomic<bool> tracking_on{false};
    /// The `m` reply: 24 is a GEM id (CGX-L family).
    std::atomic<unsigned char> model_id{24};
    /// The `V` reply, chr(major) chr(minor): 5.35. 0.0 is no usable HC (the driver uses MC_GOTO_FAST / MC_SLEW_DONE).
    std::atomic<unsigned char> firmware_major{5};
    std::atomic<unsigned char> firmware_minor{35};
    /// The `p` reply: 'W' (OTA west of the pier, normal pointing) or 'E' on a GEM; an Alt-Az HC answers an undefined
    /// byte (reference, "Pier Side"). ConformU mode answers from the hour angle instead.
    std::atomic<char> pier_side{'W'};
    /// ConformU mode: a GOTO stops the RA motor's tracking until the next `T` write (see the file comment).
    std::atomic<bool> goto_stops_tracking{true};

    // ---- Fault knobs, all off by default ------------------------------------------------------------------------

    /// The handset answers nothing at all, every command, the stops included.
    std::atomic<bool> silent{false};
    /// Rate-0 variable-rate moves on the RA motor (0x10) that arrived while silent.
    std::atomic<int> muted_ra_stops{0};
    /// Flips silent on at the next GOTO, which then goes unanswered too (#742).
    std::atomic<bool> silent_from_goto{false};
    /// A GOTO is acknowledged, but the mount never moves: L and MC_SLEW_DONE say idle.
    std::atomic<bool> no_motion{false};
    /// While set, a GOTO that started never ends.
    std::atomic<bool> goto_never_ends{false};
    /// Every GOTO goes unanswered: the wrapper times out and throws inside the slew task.
    std::atomic<bool> reject_goto{false};
    /// >0: the next GOTO is held this long (real time) before it is answered.
    std::atomic<int> gate_ms{0};
    std::atomic<Clock::rep> gate_entered{0};
    std::atomic<Clock::rep> gate_left{0};
    /// Every cancel-GOTO (M) goes unanswered, so a stop fails.
    std::atomic<bool> reject_stop{false};
    /// >0: the next M is held this long (real time) before it is answered.
    std::atomic<int> stop_gate_ms{0};
    std::atomic<Clock::rep> stop_gate_entered{0};
    std::atomic<Clock::rep> stop_gate_left{0};
    /// false: J answers chr(0) (not aligned), so the slew-safety gate refuses a slew or Park until a Sync.
    std::atomic<bool> aligned{true};
    /// The (j_hold_skip+1)th J read is held j_hold_ms (real time); j_hold_ms < 0 means no hold.
    std::atomic<int> j_hold_ms{-1};
    std::atomic<int> j_hold_skip{0};
    std::atomic<Clock::rep> j_hold_entered{0};
    /// >0: the next p read is held this long (real time) before it is answered.
    std::atomic<int> pier_hold_ms{0};
    std::atomic<Clock::rep> pier_hold_entered{0};
    /// >0: the next position read is held this long (real time) before it is answered.
    std::atomic<int> e_hold_ms{0};
    /// The next position read goes unanswered.
    std::atomic<bool> reject_next_position_reply{false};
    /// The first L that sees a GOTO ended answers "0" and makes the position read after it go unanswered.
    std::atomic<bool> reject_position_after_slew{false};
    /// The next T write goes unanswered.
    std::atomic<bool> reject_next_tracking_write{false};
    /// The next L answers "01" (two characters).
    std::atomic<bool> malformed_next_slew_status{false};
    /// Every L answers "01" (1) or "0junk" (2); 0 is off.
    std::atomic<int> malformed_slew_status{0};
    /// The next / every MC_SLEW_DONE answers 0x01 (neither done nor busy).
    std::atomic<bool> invalid_next_mc_slew_status{false};
    std::atomic<bool> invalid_mc_slew_status{false};
    /// #2409: an accepted HC GOTO keeps the old reported position (a GOTO that comes to rest far from its target).
    std::atomic<bool> miss_landing{false};
    /// #2409: a GOTO is answered "1#" instead of "#" and starts nothing.
    std::atomic<bool> odd_goto_reply{false};
    /// This many cancel-GOTO commands (M) are answered but do not stop the GOTO.
    std::atomic<int> ignore_cancels{0};
    /// The next M is answered this late (real time, ms), then 0 again.
    std::atomic<int> first_cancel_delay_ms{0};
    /// Every position read is 40 arcsec further in RA than the last: the mount never holds still.
    std::atomic<bool> unsettled{false};
    std::atomic<int> drift_reads{0};
    /// Every position read goes unanswered.
    std::atomic<bool> reject_positions{false};
    /// MC_LEVEL_DONE answers "in progress" while set.
    std::atomic<bool> hold_home{false};
    /// goto_count when the last site (W) or time (H) write arrived; -1 before any.
    std::atomic<int> goto_count_at_write{-1};
    /// MC_LEVEL_START (home) goes unanswered.
    std::atomic<bool> reject_level_start{false};
    /// The next / every MC_LEVEL_DONE answers 0x01 (neither done nor in progress).
    std::atomic<bool> invalid_next_home_status{false};
    std::atomic<bool> invalid_home_status{false};

    // ---- Observations -------------------------------------------------------------------------------------------

    /// Every command received, answered or not. `transactions` is the same counter under the name one file's
    /// assertions use (the issue's rule 6 forbids editing an assertion to rename it).
    std::atomic<int> requests{0};
    std::atomic<int>& transactions = requests;
    /// Every position query (e/E/z/Z).
    std::atomic<int> position_reads{0};
    /// GOTOs accepted (answered), HC and AUX.
    std::atomic<int> goto_count{0};
    /// HC GOTOs (r/R/b/B) received, answered or not; goto_text() is the last one.
    std::atomic<int> goto_requests{0};
    /// True from an accepted GOTO until the next answered M.
    std::atomic<bool> goto_seen{false};
    /// Answered M commands.
    std::atomic<int> cancel_goto_count{0};
    /// Wire order: seq numbers every accepted GOTO and the first answered cancel.
    std::atomic<int> seq{0};
    std::atomic<int> goto_last_seq{0};
    std::atomic<int> cancel_first_seq{0};
    /// Every L query. `l_count` is the same counter under the name one file's assertions use (rule 6, above).
    std::atomic<int> slew_status_polls{0};
    std::atomic<int>& l_count = slew_status_polls;
    /// Every MC_SLEW_DONE (0x13) query.
    std::atomic<int> slew_done_count{0};
    /// Every MC_LEVEL_START / MC_LEVEL_DONE query.
    std::atomic<int> level_start_count{0};
    std::atomic<int> home_status_polls{0};
    /// Every J read.
    std::atomic<int> j_reads{0};
    /// Every T write, answered or not.
    std::atomic<int> t_count{0};

    /// Log of the T, P, M, W, H and S (sync) commands, in wire order.
    std::mutex command_mutex;
    std::vector<std::string> commands;

    int command_count(char prefix) {
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [prefix](const std::string& command) {
            return !command.empty() && command[0] == prefix;
        }));
    }
    /// MC_AUX_GUIDE pass-throughs sent to the RA (axis 0) or DEC (axis 1) motor.
    int guide_command_count(int axis) {
        const auto device = static_cast<unsigned char>(axis == 0 ? 0x10 : 0x11);
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [device](const std::string& command) {
            return command.size() >= 8 && command[0] == 'P' && static_cast<unsigned char>(command[2]) == device &&
                   static_cast<unsigned char>(command[3]) == 0x26;
        }));
    }
    std::string goto_text() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_goto_text_;
    }
    /// Pins the e/E/z/Z reply ("RRRRRRRR,DDDDDDDD#") whatever a GOTO does after it.
    void set_position(std::string value) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_ = std::move(value);
    }

    bool goto_in_progress() const {
        if (motion_) return motion_->slewing();
        if (!goto_seen.load() || no_motion.load()) return false;
        if (goto_never_ends.load()) return true;
        const auto started = Clock::time_point(Clock::duration(goto_started_.load()));
        return clock_.now() - started < kGotoDuration;
    }

private:
    static std::string without_hash(std::string position) {
        if (!position.empty() && position.back() == '#') position.pop_back();
        return position;
    }

    static void hold_for(std::atomic<int>& gate, std::atomic<Clock::rep>* entered, std::atomic<Clock::rep>* left) {
        const int hold = gate.exchange(0);
        if (hold <= 0) return;
        if (entered) entered->store(Clock::now().time_since_epoch().count());
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        if (left) left->store(Clock::now().time_since_epoch().count());
    }

    void hold_j() {
        j_reads.fetch_add(1);
        if (j_hold_ms.load() < 0) return;
        if (j_hold_skip.fetch_sub(1) > 0) return;
        const int hold = j_hold_ms.exchange(-1);
        if (hold < 0) return;
        j_hold_entered.store(Clock::now().time_since_epoch().count());
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
    }

    // The common tail of an HC or AUX GOTO: "" when it goes unanswered, else the acknowledgement.
    std::string accept_goto(const std::string& chunk) {
        if (silent_from_goto.load()) {
            silent.store(true);
            return "";
        }
        if (reject_goto.load()) return "";
        if (odd_goto_reply.load()) return "1#";
        if (motion_) {
            if (!start_modelled_goto(chunk)) return "";  // an Alt-Az GOTO: not modelled
        } else {
            landing_.land(chunk, miss_landing.load());
        }
        hold_for(gate_ms, &gate_entered, &gate_left);
        goto_started_.store(clock_.now().time_since_epoch().count());
        goto_seen.store(true);
        goto_count.fetch_add(1);
        goto_last_seq.store(seq.fetch_add(1) + 1);
        return "#";
    }

    std::string slew_status() {
        slew_status_polls.fetch_add(1);
        if (malformed_next_slew_status.exchange(false)) return "01#";
        if (malformed_slew_status.load() == 1) return "01#";
        if (malformed_slew_status.load() == 2) return "0junk#";
        if (goto_count.load() > 0 && !goto_in_progress() && reject_position_after_slew.exchange(false)) {
            reject_next_position_reply.store(true);
            return "0#";
        }
        return goto_in_progress() ? "1#" : "0#";
    }

    // AUX pass-through: "P" chr(len) chr(dev) chr(op) chr(d1) chr(d2) chr(d3) chr(reply bytes).
    std::string passthrough(const std::string& chunk) {
        const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
        const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
        switch (op) {
            case 0xFE:  // GET_VER: chr(major) chr(minor) '#'
                return device == 0x32 ? std::string("\x01\x00#", 3) : std::string("\x00\x00#", 3);
            case 0x0B:  // MC_LEVEL_START: ack
                level_start_count.fetch_add(1);
                if (reject_level_start.load()) return "";
                return "#";
            case 0x12:  // MC_LEVEL_DONE: 0x00 in progress, 0xFF done
                home_status_polls.fetch_add(1);
                if (invalid_next_home_status.exchange(false)) return std::string("\x01#", 2);
                if (invalid_home_status.load()) return std::string("\x01#", 2);
                if (hold_home.load()) return std::string("\x00#", 2);
                return std::string("\xFF#");
            case 0x02:  // MC_GOTO_FAST
            case 0x17:  // MC_GOTO_SLOW
                return accept_goto(chunk);
            case 0x13:  // MC_SLEW_DONE: 0x00 still slewing, 0xFF done
                slew_done_count.fetch_add(1);
                if (invalid_next_mc_slew_status.exchange(false)) return std::string("\x01#", 2);
                if (invalid_mc_slew_status.load()) return std::string("\x01#", 2);
                return goto_in_progress() ? std::string("\x00#", 2) : std::string("\xFF#");
            case 0x26:  // MTR_AUX_GUIDE: ack
            case 6:     // variable-rate move, positive / negative
            case 7:
            case 36:  // fixed-rate move, positive / negative
            case 37:
                if (motion_) modelled_motor_command(chunk);
                return "#";
            default:
                return std::string("\xFF#");  // unknown command: one garbage byte before the '#'
        }
    }

    std::string answer(const std::string& chunk) {
        requests.fetch_add(1);
        if (silent.load()) {
            // MoveAxis(0, 0)'s stop: the variable-rate move (0x06) at rate 0 on the RA motor (#2409).
            if (chunk.size() >= 6 && chunk[0] == 'P' && static_cast<unsigned char>(chunk[2]) == 0x10 &&
                static_cast<unsigned char>(chunk[3]) == 0x06 && chunk[4] == 0 && chunk[5] == 0) {
                muted_ra_stops.fetch_add(1);
            }
            return "";
        }
        if (chunk.empty()) return "";
        if (chunk[0] == 'T' || chunk[0] == 'P' || chunk[0] == 'M' || chunk[0] == 'W' || chunk[0] == 'H' ||
            chunk[0] == 's' || chunk[0] == 'S') {
            std::lock_guard<std::mutex> lock(command_mutex);
            commands.push_back(chunk);
        }
        if (chunk[0] == 'W' || chunk[0] == 'H') goto_count_at_write.store(goto_count.load());
        switch (chunk[0]) {
            case 'V':  // HC version: chr(major) chr(minor) '#'
                return std::string(1, static_cast<char>(firmware_major.load())) +
                       static_cast<char>(firmware_minor.load()) + "#";
            case 'm':  // model: chr(model) '#'
                return std::string(1, static_cast<char>(model_id.load())) + "#";
            case 'J':  // alignment complete: chr(1) '#' (the slew-safety gate requires it), chr(0) '#' when not
                hold_j();
                return std::string(aligned.load() ? "\x01#" : "\x00#", 2);
            case 'p':  // pier side: 'W' or 'E' '#'
                hold_for(pier_hold_ms, &pier_hold_entered, nullptr);
                if (motion_) return std::string(1, motion_->pier_side() == 0 ? 'W' : 'E') + "#";  // 'W' = pierEast
                return std::string(1, pier_side.load()) + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z': {
                position_reads.fetch_add(1);
                if (const int hold = e_hold_ms.exchange(0); hold > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                }
                if (reject_next_position_reply.exchange(false) || reject_positions.load()) return "";
                if (unsettled.load()) {
                    std::array<char, 32> text{};
                    std::snprintf(text.data(), text.size(), "%06X00,20000500#",
                                  static_cast<unsigned>(0x12AB05 + 0x200 * drift_reads.fetch_add(1)) & 0xFFFFFFu);
                    return text.data();
                }
                if (motion_) {
                    if (chunk[0] == 'z' || chunk[0] == 'Z') {
                        return hc_wire::axis_pair(motion_->azimuth_degrees(), motion_->altitude_degrees(),
                                                  chunk[0] == 'z');
                    }
                    const auto state = motion_->state();
                    return hc_wire::axis_pair(state.ra_hours * 15.0, state.dec_degrees, chunk[0] == 'e');
                }
                std::lock_guard<std::mutex> lock(mutex_);
                return position_.empty() ? landing_.position() + "#" : position_;
            }
            case 'r':
            case 'R':
            case 'b':
            case 'B': {
                goto_requests.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    last_goto_text_ = chunk;
                }
                return accept_goto(chunk);
            }
            case 'L':  // GOTO in progress: ASCII '0' or '1' '#'
                return slew_status();
            case 'M':  // cancel GOTO: '#'
                if (reject_stop.load()) return "";
                hold_for(stop_gate_ms, &stop_gate_entered, &stop_gate_left);
                if (const int delay = first_cancel_delay_ms.exchange(0); delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                {
                    int none = 0;
                    cancel_first_seq.compare_exchange_strong(none, seq.fetch_add(1) + 1);
                }
                cancel_goto_count.fetch_add(1);
                if (ignore_cancels.load() > 0) {
                    ignore_cancels.fetch_sub(1);
                    return "#";  // answered, but the GOTO runs on
                }
                goto_seen.store(false);
                if (motion_) motion_->stop();
                return "#";
            case 't':  // tracking mode: chr(mode) '#'
                if (motion_) return std::string(1, static_cast<char>(tracking_mode_.load())) + "#";
                return tracking_on.load() ? std::string("\x02#") : std::string("\x00#", 2);
            case 'T':  // set tracking mode: '#'
                t_count.fetch_add(1);
                if (reject_next_tracking_write.exchange(false)) return "";
                if (motion_ && chunk.size() > 1) {
                    tracking_mode_.store(static_cast<unsigned char>(chunk[1]));
                    motion_->set_tracking(chunk[1] != 0);
                }
                return "#";
            case 's':  // sync: '#'
            case 'S': {
                double ra_degrees = 0.0;
                double dec_degrees = 0.0;
                if (motion_ && hc_wire::parse_aim(chunk, ra_degrees, dec_degrees)) {
                    motion_->set_position(ra_degrees / 15.0, dec_degrees);
                }
                return "#";
            }
            case 'h':  // time: eight binary bytes '#': 12:00:00 on 1 Jan 2026, UTC, no DST
                if (motion_) return hc_wire::time_bytes(*motion_, clock_offset_s_.load()) + "#";
                return std::string("\x0c\x00\x00\x01\x01\x1a\x00\x00", 8) + "#";
            case 'H':  // set time: '#'
                if (motion_) hc_wire::apply_time(*motion_, chunk, clock_offset_s_);
                return "#";
            case 'w':  // site: eight binary bytes '#' (0/0 in the scripted default)
                if (motion_) return hc_wire::site_bytes(*motion_) + "#";
                return std::string(8, '\0') + "#";
            case 'W':  // set site: '#'
                if (motion_) hc_wire::apply_site(*motion_, chunk);
                return "#";
            case 'P':
                return passthrough(chunk);
            default:
                return "";  // not modelled: no reply, so a case that needs one times out visibly
        }
    }

    // ---- ConformU mode: the commands the motion model answers (#2692) ----

    // Starts the modelled GOTO of an HC r/R, or of the AUX MC_GOTO pair, whose Dec command (device 0x11) lands it, as
    // CelestronLanding reads that pair. False for an Alt-Az GOTO, which is not modelled.
    bool start_modelled_goto(const std::string& chunk) {
        double ra_degrees = 0.0;
        double dec_degrees = 0.0;
        if (chunk[0] == 'b' || chunk[0] == 'B') return false;
        if (chunk[0] == 'P') {
            landing_.land(chunk);
            if (static_cast<unsigned char>(chunk[2]) != 0x11 ||
                !hc_wire::parse_aim("r" + landing_.position(), ra_degrees, dec_degrees)) {
                return true;  // the RA half of the pair: held until the Dec half names the target
            }
        } else if (!hc_wire::parse_aim(chunk, ra_degrees, dec_degrees)) {
            return true;
        }
        if (goto_stops_tracking.load()) motion_->set_tracking(false);
        (void)motion_->goto_radec(ra_degrees / 15.0, dec_degrees);
        return true;
    }

    // MTR_AUX_GUIDE ("P" 3 dev 0x26 velocity duration_cs): velocity is a signed percentage of the sidereal rate, on top
    // of tracking; positive RA is west (lowers RA), and positive Dec is south on a west-pier ('W') mount, north on an
    // east-pier one. A variable-rate (6/7: "P" 3 dev code hi lo) or fixed-rate (36/37: "P" 2 dev code rate) move runs
    // the axis until its stop; see the file comment for what is not measured.
    void modelled_motor_command(const std::string& chunk) {
        using Axis = FakeMountMotionModel::Axis;
        const int dev = static_cast<unsigned char>(chunk[2]);
        const int code = static_cast<unsigned char>(chunk[3]);
        const Axis axis = dev == 0x10 ? Axis::Ra : Axis::Dec;
        if (dev != 0x10 && dev != 0x11) return;
        if (code == 0x26) {
            const double rate = static_cast<signed char>(chunk[4]) / 100.0 * FakeMountMotionModel::kSiderealDegPerSec;
            const auto duration = std::chrono::milliseconds(10 * static_cast<unsigned char>(chunk[5]));
            const bool west_pier = motion_->pier_side() == 0;
            motion_->pulse(axis, axis == Axis::Ra ? -rate : (west_pier ? -rate : rate), duration);
            return;
        }
        double deg_per_s = 0.0;
        if (code == 6 || code == 7) {
            const double rate =
                ((static_cast<unsigned char>(chunk[4]) << 8) | static_cast<unsigned char>(chunk[5])) / 4.0 / 3600.0;
            deg_per_s = code == 6 ? rate : -rate;
        } else {
            const int step = std::min<int>(static_cast<unsigned char>(chunk[4]), 9);
            deg_per_s = hc_wire::kFixedRateSidereal[step] * FakeMountMotionModel::kSiderealDegPerSec *
                        (code == 36 ? 1.0 : -1.0);
        }
        motion_->move_axis(axis, deg_per_s);
    }

    util::TaskClock& clock_;
    std::atomic<Clock::rep> goto_started_{0};
    std::mutex mutex_;  // position_, last_goto_text_
    std::string position_;  // pinned by set_position(); empty: the landing model answers
    CelestronLanding landing_;
    std::string last_goto_text_;
    std::unique_ptr<FakeMountMotionModel> motion_;  // ConformU mode only
    std::atomic<int> tracking_mode_{0};             // ConformU mode: the mode `t` reads back, as last written
    std::atomic<std::int64_t> clock_offset_s_{0};   // ConformU mode: handset clock less the model's, set by H
};

}  // namespace alpacacore::test

#endif  // !_WIN32

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

// Request/response pty fake of a WandererRotator Mini (V1 firmware angle format). The rotator
// speaks only when asked, so the streaming fake_serial_streamer.h cannot drive its connect. The
// reply bytes are the ones wandererastro_rotator_protocol_wrapper.cpp parses:
//
//   "1500001"        -> "WandererRotatorMiniA<fw>A<angle*1000>A<backlash>A<reverse>A"
//                       (do_handshake; five 'A'-terminated fields; firmware is the literal
//                       20250101, so a change to the wrapper's firmware floor shows up here;
//                       no '.' in the angle = V1 integer format)
//   "1500002"        -> no reply; the tracked angle goes to zero (set_zero)
//   "1600000"+n*10   -> no reply (set_backlash); "1700000"/"1700001" -> no reply (set_reverse)
//   "Stop"           -> ends a timed move where it stands and reports that angle; no reply when idle
//   <steps+1000000>  -> "<angle*1000>A", the absolute accumulated angle in the V1
//                       integer format (monitor_move). A negative move is six digits,
//                       a positive one seven, so the first digit gives the length:
//                       every config code above is >= 1500000, which no move can reach.
//
// The angle accumulates, signed, as the device's does (wandererastro.instructions.md); it is never
// wrapped. Past 400 degrees the wrapper's handshake resets it with "1500002", as on the device.
//
// By default a move completes at once, so the wrapper's monitor thread ends after its own 600 ms
// quiet window and nothing here sleeps (the concurrency storm). With ms_per_degree > 0 (the
// ConformU host and the tier-2 and tier-3 sweep hosts) the motor turns at that rate: the device is
// silent during the sweep and reports the final angle only when it arrives, and "Stop" ends the move
// at the angle it has reached and reports that at once. A move that arrives while one runs replaces
// it from where it stands, with no report for the replaced one.
//
// KNOWN PARITY GAPS: no acceleration or deceleration (the bench turns about 0.22 s per degree once
// moving; the INDI reference rate of 0.24 s the ConformU host uses is the slower of the two); no V2
// format (decimal degrees, relative completion report: the bench unit's firmware); no firmware
// 20261025 "NP"/"LIMIT" replies (motor power off is silence, as firmware 20250101 answers).
//
// Every command is recorded ("Stop" and the digit strings) in wire order.

#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "connect_hold_gate.h"
#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeWandererRotator {
public:
    /// @p ms_per_degree 0: every move completes at once. Above 0: a move takes |degrees| times this.
    explicit FakeWandererRotator(double ms_per_degree = 0.0)
        : pty_("FakeWandererRotator"), ms_per_degree_(ms_per_degree) {
        worker_ = std::thread([this] { run(); });
    }

    ~FakeWandererRotator() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    FakeWandererRotator(const FakeWandererRotator&) = delete;
    FakeWandererRotator& operator=(const FakeWandererRotator&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// The next handshake reply waits @p delay first (connect_hold_gate.h), so a connect stays in flight.
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }

    /// DC power off (true) or on: the serial link keeps working, but a move in flight stops where it
    /// stands with no report, and a move sent while the power is off does nothing and reports nothing.
    void set_motor_power_off(bool off) { motor_off_.store(off); }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    int count(const std::string& cmd) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c == cmd) ++n;
        }
        return n;
    }

    /// Handshakes that reached the wire: one per connect attempt that got that far.
    int handshakes() const { return count("1500001"); }

    /// Move commands that reached the wire (anything that is not a handshake, a config code or "Stop").
    int moves() const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (is_move(c)) ++n;
        }
        return n;
    }

private:
    using Clock = std::chrono::steady_clock;

    static bool is_move(const std::string& cmd) {
        if (cmd == "Stop" || cmd.empty()) return false;
        return std::atoi(cmd.c_str()) < 1500000;
    }

    void run() {
        std::string acc;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            const int ready = poll(&pfd, 1, 20);
            if (moving_ && motor_off_.load()) {
                angle_milli_ = angle_now_milli();  // the motor stopped here; the device says nothing
                moving_ = false;
            }
            if (moving_ && Clock::now() >= move_end_) {
                moving_ = false;
                angle_milli_ = move_target_milli_;
                reply(std::to_string(angle_milli_) + "A");
            }
            if (ready <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            acc.append(buf, static_cast<std::size_t>(n));
            drain(acc);
        }
    }

    // Consume every complete command at the front of @p acc; keep a partial tail.
    void drain(std::string& acc) {
        for (;;) {
            while (!acc.empty() && (acc.front() == '\n' || acc.front() == '\r')) acc.erase(0, 1);
            if (acc.empty()) return;
            if (acc.front() == 'S') {
                if (acc.size() < 4) return;
                handle(acc.compare(0, 4, "Stop") == 0 ? "Stop" : "");
                acc.erase(0, 4);
                continue;
            }
            if (acc.front() < '0' || acc.front() > '9') {
                acc.erase(0, 1);  // not a command byte; the firmware ignores it
                continue;
            }
            const std::size_t need = acc.front() == '1' ? 7 : 6;
            if (acc.size() < need) return;
            handle(acc.substr(0, need));
            acc.erase(0, need);
        }
    }

    // Where a timed move stands now, in milli-degrees. Worker thread only.
    long angle_now_milli() const {
        if (!moving_) return angle_milli_;
        const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - move_start_).count();
        const long travelled = std::lround(elapsed_ms / ms_per_degree_ * 1000.0);
        const long span = move_target_milli_ - angle_milli_;
        if (travelled >= std::labs(span)) return move_target_milli_;
        return angle_milli_ + (span < 0 ? -travelled : travelled);
    }

    void handle(const std::string& cmd) {
        if (cmd.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        if (cmd == "Stop") {
            if (moving_) {
                angle_milli_ = angle_now_milli();
                moving_ = false;
                reply(std::to_string(angle_milli_) + "A");
            }
            return;
        }
        const long value = std::atol(cmd.c_str());
        if (cmd == "1500001") {
            if (const int hold = hold_ms_.exchange(0); hold > 0) {
                alpacacore::test::connect_hold(std::chrono::milliseconds(hold));  // connect_hold_gate.h
            }
            reply(std::string("WandererRotatorMiniA20250101A") + std::to_string(angle_now_milli()) + "A0A0A");
        } else if (cmd == "1500002") {
            angle_milli_ = 0;
            moving_ = false;
        } else if (value < 1500000) {
            if (motor_off_.load()) return;
            const double steps = static_cast<double>(value - 1000000);
            const long delta = std::lround(steps * 1000.0 / vendor::wandererastro::kRotatorMiniStepsPerDegree);
            angle_milli_ = angle_now_milli();  // a move in flight is replaced from where it stands
            if (ms_per_degree_ <= 0.0) {
                moving_ = false;
                angle_milli_ += delta;
                reply(std::to_string(angle_milli_) + "A");
                return;
            }
            move_target_milli_ = angle_milli_ + delta;
            move_start_ = Clock::now();
            // milli-degrees times ms per degree is microseconds
            move_end_ = move_start_ +
                        std::chrono::microseconds(std::llround(std::fabs(static_cast<double>(delta)) * ms_per_degree_));
            moving_ = true;
        }  // backlash / reverse: accepted silently
    }

    void reply(const std::string& text) { pty_write_bounded(pty_.master_fd(), text, stop_); }

    PtyPair pty_;
    const double ms_per_degree_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<int> hold_ms_{0};
    std::atomic<bool> motor_off_{false};
    // Worker thread only.
    long angle_milli_ = 0;
    bool moving_ = false;
    long move_target_milli_ = 0;
    Clock::time_point move_start_{};
    Clock::time_point move_end_{};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
};

}  // namespace alpacacore::test

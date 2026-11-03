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

// Hardware-free iOptron iFR focuser-rotator behind a pseudo-terminal. The drivers open the pty slave as they
// open either of the unit's two Prolific ports; this fake answers on the master as an iFR 3" (model 05,
// firmware 260324) did on the rig, 2026-11-03 (AlpacaCore/external/iOptron/iFR-protocol.md):
//
//   :DeviceInfo#  -> "000000" "05" "0100" "#"     (the position field always reads 000000 on the iFR)
//   :FW1#         -> "260324260324#"
//   :FI#          -> "%+07d%1d%05d%1d#"           position, moving, temperature (K x 100), direction
//   :FM<7>#       -> "1" accepted / "0" refused   refused for the current position, and toward a limit the
//                                                  focuser already sits at; otherwise travel stops at a limit
//   :FQ# :FZ# :FR# -> "1"
//   :RI#          -> "%+010ld%1d%09ld%1d%02d#"    offset, moving, position (0.01 arcsec), direction, size
//   :RM<9>#       -> "1"                          target in the reported (synced, possibly mirrored) frame
//   :RY<9>#       -> "1"                          sets the offset. While reversed it stores the offset the
//                                                  firmware does: one that does not match the mirrored frame
//   :RR0# / :RR1# -> "1"                          reversed / normal; the reported frame mirrors
//   :RQ# :RS<2>#  -> "1"
//
// Acks are one byte with no '#', as on the unit; a reply is never sent for an unknown command. Motion takes
// set_*_polls() status polls to arrive (default 2): each poll of a moving axis reports it moving and part of
// the way, the last one at the target. The harsher shape on purpose: the hardware's moving flag never lagged,
// but a driver that assumed it could miss a poll that reads "stopped at the old place" would be wrong.

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeIoptronIfr {
public:
    static constexpr long kUnitsPerDegree = 360000;
    static constexpr long kUnitsPerTurn = 360L * kUnitsPerDegree;

    FakeIoptronIfr() : pty_("FakeIoptronIfr") {
        reader_ = std::thread([this] { run(); });
    }

    ~FakeIoptronIfr() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeIoptronIfr(const FakeIoptronIfr&) = delete;
    FakeIoptronIfr& operator=(const FakeIoptronIfr&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    int count(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    /// Handshakes that reached the wire: one per opened session.
    int handshakes() const { return count(":DeviceInfo#"); }

    void set_model(int model) { model_.store(model); }
    /// One shot: the reply to the next command handled is held for @p delay (makes Connecting observable).
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }
    /// While true every command is recorded and left unanswered, as a hung firmware with the port open.
    void set_silent(bool silent) { silent_.store(silent); }
    /// A USB unplug: every driver write fails with EIO from here on. Not reversible.
    void unplug() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
        pty_.sever();
    }

    // --- focuser ---
    int focuser_position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return focuser_pos_;
    }
    void set_focuser_position(int p) {
        std::lock_guard<std::mutex> lock(mutex_);
        focuser_pos_ = p;
        focuser_target_ = p;
        focuser_polls_left_ = 0;
    }
    /// Travel stops at these (the 3" measured 0..62,892 once zeroed at its inner end).
    void set_focuser_limits(int low, int high) {
        std::lock_guard<std::mutex> lock(mutex_);
        focuser_low_ = low;
        focuser_high_ = high;
    }
    void set_focuser_polls(int polls) { focuser_polls_.store(polls); }
    /// Polls after an accepted move that still report the focuser stopped at its old position, before the motor
    /// starts (default 0: the rig never showed one). What the drivers' move hold exists for.
    void set_start_delay_polls(int polls) { start_delay_polls_.store(polls); }
    bool focuser_moving() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return focuser_moving_locked();
    }

    // --- rotator ---
    /// Physical angle in degrees (the ASCOM MechanicalPosition).
    double rotator_mechanical() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<double>(phys_) / kUnitsPerDegree;
    }
    void set_rotator_mechanical(double degrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        phys_ = wrap_units(std::lround(degrees * kUnitsPerDegree));
        phys_target_ = phys_;
        rotator_polls_left_ = 0;
    }
    /// The offset U of the normal frame, in degrees (Position = mechanical + U when not reversed).
    double rotator_offset() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<double>(offset_) / kUnitsPerDegree;
    }
    void set_rotator_offset(double degrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        offset_ = wrap_units(std::lround(degrees * kUnitsPerDegree));
    }
    bool rotator_reversed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reversed_;
    }
    void set_rotator_reversed(bool reversed) {
        std::lock_guard<std::mutex> lock(mutex_);
        reversed_ = reversed;
    }
    /// The size register, tenths of an inch.
    void set_rotator_size(int tenths) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_ = tenths;
    }
    void set_rotator_polls(int polls) { rotator_polls_.store(polls); }
    bool rotator_moving() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rotator_moving_locked();
    }

private:
    static long wrap_units(long u) {
        u %= kUnitsPerTurn;
        if (u < 0) u += kUnitsPerTurn;
        return u;
    }

    // The reported (synced, possibly mirrored) angle of a physical one. Caller holds mutex_.
    long reported_locked(long phys) const {
        const long normal = wrap_units(phys + offset_);
        return reversed_ ? wrap_units(kUnitsPerTurn - normal) : normal;
    }

    long physical_for_reported_locked(long reported) const {
        const long normal = reversed_ ? wrap_units(kUnitsPerTurn - reported) : reported;
        return wrap_units(normal - offset_);
    }

    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 20) <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            for (ssize_t i = 0; i < n; ++i) {
                if (pending.empty() && buf[i] != ':') continue;
                pending += buf[i];
                if (buf[i] == '#') {
                    handle(pending);
                    pending.clear();
                }
            }
        }
    }

    void handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        if (silent_.load()) return;
        if (const int hold = hold_ms_.exchange(0); hold > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        }
        std::string reply;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reply = reply_locked(cmd);
        }
        if (!reply.empty()) pty_write_bounded(pty_.master_fd(), reply, stop_);
    }

    std::string reply_locked(const std::string& cmd) {
        char out[48] = {};
        if (cmd == ":DeviceInfo#") {
            std::snprintf(out, sizeof(out), "000000%02d0100#", model_.load());
        } else if (cmd == ":FW1#") {
            return "260324260324#";
        } else if (cmd == ":FI#") {
            step_focuser_locked();
            std::snprintf(out, sizeof(out), "%+07d%1d%05d%1d#", focuser_pos_, focuser_moving_locked() ? 1 : 0, 29858,
                          focuser_dir_);
        } else if (cmd.rfind(":FM", 0) == 0) {
            const int target = std::atoi(cmd.c_str() + 3);
            const bool at_low_going_lower = focuser_pos_ <= focuser_low_ && target < focuser_pos_;
            const bool at_high_going_higher = focuser_pos_ >= focuser_high_ && target > focuser_pos_;
            if (target == focuser_pos_ || at_low_going_lower || at_high_going_higher) return "0";
            focuser_start_ = focuser_pos_;
            focuser_target_ = target < focuser_low_ ? focuser_low_ : (target > focuser_high_ ? focuser_high_ : target);
            focuser_polls_left_ = focuser_polls_.load();
            focuser_delay_left_ = start_delay_polls_.load();
            if (focuser_polls_left_ <= 0 && focuser_delay_left_ <= 0) focuser_pos_ = focuser_target_;
            return "1";
        } else if (cmd == ":FQ#") {
            focuser_target_ = focuser_pos_;
            focuser_polls_left_ = 0;
            focuser_delay_left_ = 0;
            return "1";
        } else if (cmd == ":FZ#") {
            focuser_pos_ = focuser_target_ = 0;
            focuser_polls_left_ = 0;
            return "1";
        } else if (cmd == ":FR#") {
            focuser_dir_ = focuser_dir_ ? 0 : 1;
            return "1";
        } else if (cmd == ":RI#") {
            step_rotator_locked();
            const long field = reversed_ ? -offset_ : offset_;
            std::snprintf(out, sizeof(out), "%+010ld%1d%09ld%1d%02d#", field, rotator_moving_locked() ? 1 : 0,
                          reported_locked(phys_), reversed_ ? 0 : 1, size_);
        } else if (cmd.rfind(":RM", 0) == 0) {
            const long target = std::atol(cmd.c_str() + 3);
            rotator_start_ = phys_;
            phys_target_ = physical_for_reported_locked(target);
            rotator_polls_left_ = rotator_polls_.load();
            rotator_delay_left_ = start_delay_polls_.load();
            if (rotator_polls_left_ <= 0 && rotator_delay_left_ <= 0) phys_ = phys_target_;
            return "1";
        } else if (cmd == ":RQ#") {
            phys_target_ = phys_;
            rotator_polls_left_ = 0;
            rotator_delay_left_ = 0;
            return "1";
        } else if (cmd.rfind(":RY", 0) == 0) {
            const long target = std::atol(cmd.c_str() + 3);
            // In the normal frame the offset makes Position read the target. While reversed the firmware stores
            // the offset as if the frame were normal, so the mirrored Position does not read the target (rig).
            offset_ = wrap_units(target - phys_);
            return "1";
        } else if (cmd == ":RR0#") {
            reversed_ = true;
            return "1";
        } else if (cmd == ":RR1#") {
            reversed_ = false;
            return "1";
        } else if (cmd.rfind(":RS", 0) == 0) {
            size_ = std::atoi(cmd.c_str() + 3);
            return "1";
        } else {
            return {};
        }
        return out;
    }

    // One poll of a moving axis: part of the way, then the target on the last poll. Caller holds mutex_.
    // Moving once the start delay is over and polls are left; a delayed move reports stopped at the old place.
    bool focuser_moving_locked() const { return focuser_delay_left_ <= 0 && focuser_polls_left_ > 0; }
    bool rotator_moving_locked() const { return rotator_delay_left_ <= 0 && rotator_polls_left_ > 0; }

    void step_focuser_locked() {
        if (focuser_delay_left_ > 0) {
            if (--focuser_delay_left_ > 0 || focuser_polls_left_ > 0) return;
            focuser_pos_ = focuser_target_;  // a zero-poll move lands once the delay is over
            return;
        }
        if (focuser_polls_left_ <= 0) return;
        --focuser_polls_left_;
        const int total = focuser_polls_.load();
        if (focuser_polls_left_ == 0 || total <= 0) {
            focuser_pos_ = focuser_target_;
            focuser_polls_left_ = 0;
        } else {
            const int done = total - focuser_polls_left_;
            focuser_pos_ = focuser_start_ + (focuser_target_ - focuser_start_) * done / total;
        }
    }

    void step_rotator_locked() {
        if (rotator_delay_left_ > 0) {
            if (--rotator_delay_left_ > 0 || rotator_polls_left_ > 0) return;
            phys_ = phys_target_;
            return;
        }
        if (rotator_polls_left_ <= 0) return;
        --rotator_polls_left_;
        const int total = rotator_polls_.load();
        if (rotator_polls_left_ == 0 || total <= 0) {
            phys_ = phys_target_;
            rotator_polls_left_ = 0;
        } else {
            const int done = total - rotator_polls_left_;
            phys_ = rotator_start_ + (phys_target_ - rotator_start_) * done / total;
        }
    }

    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<int> hold_ms_{0};
    std::atomic<bool> silent_{false};
    std::atomic<int> model_{5};
    std::atomic<int> focuser_polls_{2};
    std::atomic<int> rotator_polls_{2};
    std::atomic<int> start_delay_polls_{0};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    int focuser_pos_ = 1000;
    int focuser_start_ = 1000;
    int focuser_target_ = 1000;
    int focuser_polls_left_ = 0;
    int focuser_delay_left_ = 0;
    int focuser_low_ = 0;
    int focuser_high_ = 62892;
    int focuser_dir_ = 0;
    long phys_ = 35L * kUnitsPerDegree;
    long rotator_start_ = 35L * kUnitsPerDegree;
    long phys_target_ = 35L * kUnitsPerDegree;
    int rotator_polls_left_ = 0;
    int rotator_delay_left_ = 0;
    long offset_ = 0;
    bool reversed_ = false;
    int size_ = 30;
};

}  // namespace alpacacore::test

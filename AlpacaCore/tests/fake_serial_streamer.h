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

// Pty-backed fake for devices that STREAM a status frame unprompted and take
// fire-and-forget commands (the WandererAstro cover / filter wheel / box
// family). The test supplies the frame bytes; the fake repeats them at a fixed
// interval and records every '\n'-terminated command line it receives. Two
// SFW wheel mode (opt-in, sfw_travel > 0): a move command 2001..2008 changes the
// frame's position token to that slot after the travel time, as the SFW does;
// other commands are only recorded. Off by default: the frame never changes.
// The mode is for SFW frames only: it finds the position as the third 'A'-delimited
// field, so another frame layout would have the wrong field rewritten.
// WandererCover mode (opt-in, cover_deg_per_s > 0): open 1001 and close 1000 move
// the frame's current angle (<model>A<fw>A<closePos>A<openPos>A<curPos>A...) toward
// that end at the given rate and stop there, as the cover's motor does; a newer
// command replaces the target. The cover has no halt command, so nothing else stops
// a move. Off by default: the frame never changes.
// Failure modes for the issue #237 link-health tests:
//   - set_muted(true): the stream stops, the fd stays healthy (hung MCU);
//   - sever_link(): the pty master closes, so the driver's writes fail with
//     EIO and its reads return 0 at once from then on (USB re-enumeration /
//     unplug), and
//     slave_path() is empty afterwards: the path named a pty that no longer
//     exists, so copy it before the cut if the test still needs it.

#include <alpacacore/util/serial_port_registry.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "connect_hold_gate.h"
#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeSerialStreamer {
public:
    /// @p first_frame_hold is a start-up window, not a one-shot: no frame is sent until that long after the worker
    /// started, and nothing is consumed (later frames stream as normal). A connect that waits for the first streamed
    /// frame stays open until then. It is a constructor argument because the worker starts streaming inside the
    /// constructor: a setter called afterwards races the first frames. Used by the contract sweep.
    /// @p sfw_travel > 0 turns on the SFW wheel mode (file header); a constructor argument for the same reason.
    /// @p cover_deg_per_s > 0 turns on the WandererCover mode (file header), for the same reason.
    FakeSerialStreamer(std::string frame, std::chrono::milliseconds interval,
                       std::chrono::milliseconds first_frame_hold = std::chrono::milliseconds(0),
                       std::chrono::milliseconds sfw_travel = std::chrono::milliseconds(0),
                       double cover_deg_per_s = 0.0)
        : pty_("FakeSerialStreamer"),
          sfw_travel_ms_(static_cast<int>(sfw_travel.count())),
          cover_deg_per_s_(cover_deg_per_s),
          first_frame_hold_ms_(static_cast<int>(first_frame_hold.count())),
          interval_ms_(static_cast<int>(interval.count())),
          frame_(std::move(frame)) {
        // The pty pair is owned by pty_ (fake_pty_write.h), constructed
        // before this body runs; a setup failure throws from there with
        // nothing left open (issue #387).
        worker_ = std::thread([this] { run(); });
    }

    ~FakeSerialStreamer() { sever_link(); }

    FakeSerialStreamer(const FakeSerialStreamer&) = delete;
    FakeSerialStreamer& operator=(const FakeSerialStreamer&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }
    /// The keep-alive slave fd: it shares the line settings of the driver's
    /// fd, so a test can retune the live port through it (issue #1202) or
    /// read back what the driver configured.
    int keepalive_fd() const { return pty_.keepalive_fd(); }

    void set_frame(std::string frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = std::move(frame);
    }
    void set_muted(bool muted) { muted_.store(muted); }
    /// Frames written to the pty so far.
    long long frames_sent() const { return frames_sent_.load(); }
    /// Returns once the worker has finished two passes after this call: a frame it was writing when
    /// set_muted(true) landed is then on the pty, and no other follows while muted. False past @p limit.
    bool wait_for_quiet(std::chrono::milliseconds limit) const {
        const long long target = passes_.load() + 2;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (passes_.load() < target) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }
    bool received(const std::string& exact) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& c : commands_) {
            if (c == exact) return true;
        }
        return false;
    }

    // Close the master side underneath the driver (idempotent).
    void sever_link() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        pty_.sever();
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        const auto started = std::chrono::steady_clock::now();
        auto last_stream = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 10);
            if (r > 0) {
                const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
                for (ssize_t i = 0; i < n; ++i) {
                    const char ch = buf[i];
                    if (ch == '\n' || ch == '\r') {
                        if (!pending.empty()) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            commands_.push_back(pending);
                        }
                        note_move_command(pending);
                        note_cover_command(pending);
                        pending.clear();
                    } else {
                        pending += ch;
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            // While a ConnectHoldGate is alive and unreleased the start-up hold stays shut, whatever the time
            // (connect_hold_gate.h, at most ConnectHoldGate::kCap), so the test, not the clock, ends it.
            const bool hold_shut =
                first_frame_hold_ms_ > 0 && ConnectHoldGate::holding() && now - started < ConnectHoldGate::kCap;
            // The connect is inside the hold once its driver holds the port (it then waits for the first frame).
            if (hold_shut && !hold_entered_ && util::is_serial_port_in_use(pty_.slave_path())) {
                hold_entered_ = true;
                ConnectHoldGate::note_entered();
            }
            if (!muted_.load() && !hold_shut && now - started >= std::chrono::milliseconds(first_frame_hold_ms_) &&
                now - last_stream >= std::chrono::milliseconds(interval_ms_)) {
                last_stream = now;
                std::string frame;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    frame = frame_;
                }
                pty_write_bounded(pty_.master_fd(), frame, stop_);
                frames_sent_.fetch_add(1);
            }
            apply_arrived_move(now);
            apply_cover_travel(now);
            passes_.fetch_add(1);
        }
    }

    // Worker thread only. A move command 2000+slot (slot 1-8) starts the travel; a newer one replaces it.
    void note_move_command(const std::string& command) {
        if (sfw_travel_ms_ <= 0 || command.size() != 4 || command.compare(0, 3, "200") != 0) return;
        const int slot = command[3] - '0';
        if (slot < 1 || slot > 8) return;
        move_slot_ = slot;
        move_due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(sfw_travel_ms_);
    }

    // Worker thread only: once the travel time has passed, the frame's position token (third 'A'-delimited
    // field: <model>A<firmware>A<position>A...) becomes the commanded slot.
    void apply_arrived_move(std::chrono::steady_clock::time_point now) {
        if (move_slot_ == 0 || now < move_due_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        set_field(2, std::to_string(move_slot_));
        move_slot_ = 0;
    }

    // Worker thread only. Open 1001 aims the cover at the frame's open angle (field 3), close 1000 at its close angle
    // (field 2), from the angle the frame reports now (field 4).
    void note_cover_command(const std::string& command) {
        if (cover_deg_per_s_ <= 0.0 || (command != "1001" && command != "1000")) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string target = field(command == "1001" ? 3 : 2);
        const std::string current = field(4);
        if (target.empty() || current.empty()) return;
        cover_target_ = std::stod(target);
        cover_angle_ = std::stod(current);
        cover_moving_ = true;
        cover_last_ = std::chrono::steady_clock::now();
    }

    // Worker thread only: the angle moves toward the target at the cover rate and stops on it.
    void apply_cover_travel(std::chrono::steady_clock::time_point now) {
        if (!cover_moving_) return;
        const double step = cover_deg_per_s_ * std::chrono::duration<double>(now - cover_last_).count();
        cover_last_ = now;
        const double gap = cover_target_ - cover_angle_;
        if (std::fabs(gap) <= step) {
            cover_angle_ = cover_target_;
            cover_moving_ = false;
        } else {
            cover_angle_ += gap > 0.0 ? step : -step;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", cover_angle_);
        std::lock_guard<std::mutex> lock(mutex_);
        set_field(4, text);
    }

    // Caller holds mutex_. The bounds of the 'A'-terminated field @p index of frame_ (0 = the model token); false when
    // the frame has no such field.
    bool field_bounds(int index, std::size_t& begin, std::size_t& end) const {
        begin = 0;
        for (int i = 0; i < index; ++i) {
            const auto a = frame_.find('A', begin);
            if (a == std::string::npos) return false;
            begin = a + 1;
        }
        end = frame_.find('A', begin);
        return end != std::string::npos;
    }
    std::string field(int index) const {
        std::size_t begin = 0;
        std::size_t end = 0;
        return field_bounds(index, begin, end) ? frame_.substr(begin, end - begin) : std::string();
    }
    void set_field(int index, const std::string& value) {
        std::size_t begin = 0;
        std::size_t end = 0;
        if (field_bounds(index, begin, end)) frame_.replace(begin, end - begin, value);
    }

    // First member: constructed before the worker, destroyed after it has
    // been joined.
    PtyPair pty_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> muted_{false};
    std::atomic<long long> frames_sent_{0};
    std::atomic<long long> passes_{0};
    const int sfw_travel_ms_;
    int move_slot_ = 0;                               // worker thread only; 0 = no move under way
    std::chrono::steady_clock::time_point move_due_;  // worker thread only
    const double cover_deg_per_s_;
    bool cover_moving_ = false;                         // worker thread only
    double cover_target_ = 0.0;                         // worker thread only
    double cover_angle_ = 0.0;                          // worker thread only
    std::chrono::steady_clock::time_point cover_last_;  // worker thread only
    const int first_frame_hold_ms_;
    bool hold_entered_ = false;  // worker thread only
    int interval_ms_;

    mutable std::mutex mutex_;
    std::string frame_;
    std::vector<std::string> commands_;
};

}  // namespace alpacacore::test

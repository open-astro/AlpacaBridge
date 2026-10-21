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

// Request/response pty fake of a WandererRotator Mini (V1 firmware angle format), for
// the concurrency storm: the rotator speaks only when asked, so the streaming
// fake_serial_streamer.h cannot drive its connect. The reply bytes are the ones
// wandererastro_rotator_protocol_wrapper.cpp parses:
//
//   "1500001"        -> "WandererRotatorMiniA<fw>A<angle*1000>A<backlash>A<reverse>A"
//                       (do_handshake; five 'A'-terminated fields; firmware is the literal
//                       20250101, so a change to the wrapper's firmware floor shows up here;
//                       no '.' in the angle = V1 integer format)
//   "1500002"        -> no reply; the tracked angle goes to zero (set_zero)
//   "1600000"+n*10   -> no reply (set_backlash); "1700000"/"1700001" -> no reply (set_reverse)
//   "Stop"           -> no reply (a fake move is already complete)
//   <steps+1000000>  -> "<angle*1000>A", the absolute accumulated angle in the V1
//                       integer format (monitor_move). A negative move is six digits,
//                       a positive one seven, so the first digit gives the length:
//                       every config code above is >= 1500000, which no move can reach.
//
// A move completes at once, so the wrapper's monitor thread ends after its own
// 600 ms quiet window and nothing here sleeps. The tracked angle is wrapped into
// (-180, 180] so a long storm never reaches the wrapper's 400 degree reset path.
// Every command is recorded ("Stop" and the digit strings) in wire order.
//
// Stress-only fake with no tier-2 contract-sweep roster row yet. check_docs_drift.py pins
// every fake_*.h file to a roster row or its helper list, so a roster row would also
// rename this file to fake_*.h.

#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class PtyWandererRotator {
public:
    PtyWandererRotator() : pty_("PtyWandererRotator") {
        worker_ = std::thread([this] { run(); });
    }

    ~PtyWandererRotator() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    PtyWandererRotator(const PtyWandererRotator&) = delete;
    PtyWandererRotator& operator=(const PtyWandererRotator&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

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
            if (poll(&pfd, 1, 20) <= 0) continue;
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

    void handle(const std::string& cmd) {
        if (cmd.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        if (cmd == "Stop") return;
        const long value = std::atol(cmd.c_str());
        if (cmd == "1500001") {
            reply(std::string("WandererRotatorMiniA20250101A") + std::to_string(angle_milli_) + "A0A0A");
        } else if (cmd == "1500002") {
            angle_milli_ = 0;
        } else if (value < 1500000) {
            const double steps = static_cast<double>(value - 1000000);
            angle_milli_ += std::lround(steps * 1000.0 / vendor::wandererastro::kRotatorMiniStepsPerDegree);
            while (angle_milli_ > 180000) angle_milli_ -= 360000;
            while (angle_milli_ <= -180000) angle_milli_ += 360000;
            reply(std::to_string(angle_milli_) + "A");
        }  // backlash / reverse: accepted silently
    }

    void reply(const std::string& text) { pty_write_bounded(pty_.master_fd(), text, stop_); }

    PtyPair pty_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    long angle_milli_ = 0;  // worker thread only

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
};

}  // namespace alpacacore::test

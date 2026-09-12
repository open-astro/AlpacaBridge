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

// Pty-backed fake Gemini Power & Data Hubs Advanced 3 for hardware-free
// wire-level tests. Speaks the protocol summarized in
// AlpacaCore/external/Gemini/PowerDataHubAdv3-protocol.md (as decompiled from
// the vendor's ASCOM driver): >H#/>V#/>G# queries get replies, set commands
// (>O/>C/>X/>Y/>Z/>M) update the fake's state and are deliberately NOT
// acknowledged, so the driver can never come to depend on an ack the real
// firmware might not send. Optionally streams *G status frames unprompted to
// exercise the reader thread's routing (streamed frame vs. pending request).

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeGeminiPdh {
public:
    FakeGeminiPdh() {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        // Issue #424: non-blocking, so a reply to a driver that has stopped
        // draining can never park this fake's worker inside write() and
        // hang the destructor's join. See fake_pty_write.h.
        if (master_fd_ >= 0) {
            make_pty_nonblocking(master_fd_);
        }
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0) {
            throw std::runtime_error("FakeGeminiPdh: cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            throw std::runtime_error("FakeGeminiPdh: ptsname failed");
        }
        slave_path_ = name;
        keepalive_fd_ = open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
        struct termios tty {};
        if (keepalive_fd_ >= 0 && tcgetattr(keepalive_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(keepalive_fd_, TCSANOW, &tty);
        }
        reader_ = std::thread([this] { run(); });
    }

    ~FakeGeminiPdh() {
        stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
        if (keepalive_fd_ >= 0) {
            close(keepalive_fd_);
        }
        if (master_fd_ >= 0) {
            close(master_fd_);
        }
    }

    FakeGeminiPdh(const FakeGeminiPdh&) = delete;
    FakeGeminiPdh& operator=(const FakeGeminiPdh&) = delete;

    const std::string& slave_path() const { return slave_path_; }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    int count(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.rfind(prefix, 0) == 0) {
                ++n;
            }
        }
        return n;
    }

    bool received(const std::string& exact) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& c : commands_) {
            if (c == exact) return true;
        }
        return false;
    }

    // --- Fake hardware state (readable/settable by tests) ---

    void set_firmware(int version) { firmware_.store(version); }
    void set_input_voltage(double v) {
        std::lock_guard<std::mutex> lock(mutex_);
        input_voltage_ = v;
    }
    /// 0 disables streaming (reply-only firmware model, the default).
    void set_stream_interval(std::chrono::milliseconds interval) {
        stream_ms_.store(static_cast<int>(interval.count()));
    }
    /// Muted: commands are still recorded but nothing is ever sent back
    /// (a hung MCU / broken RX line). Writes keep succeeding at the fd level.
    void set_muted(bool muted) { muted_.store(muted); }

    /// Tear the pty down underneath the driver: the master side closes, so
    /// the driver's reads and writes on the slave fail with EIO from here on
    /// (what a USB re-enumeration / unplug looks like, issue #237). Not
    /// reversible; the fake only records commands received before the cut.
    void sever_link() {
        stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
        if (keepalive_fd_ >= 0) {
            close(keepalive_fd_);
            keepalive_fd_ = -1;
        }
        if (master_fd_ >= 0) {
            close(master_fd_);
            master_fd_ = -1;
        }
    }

    bool output(int channel) const {  // 1..11 wire channel
        std::lock_guard<std::mutex> lock(mutex_);
        return outputs_[static_cast<std::size_t>(channel)];
    }
    int dew_mode(int channel) const {  // 6 or 7
        std::lock_guard<std::mutex> lock(mutex_);
        return dew_mode_[static_cast<std::size_t>(channel - 6)];
    }
    int dew_manual_pwm(int channel) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dew_pwm_[static_cast<std::size_t>(channel - 6)];
    }
    bool dew_enabled(int channel) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dew_enabled_[static_cast<std::size_t>(channel - 6)];
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        auto last_stream = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = master_fd_;
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 10);
            if (r > 0) {
                const ssize_t n = read(master_fd_, buf, sizeof(buf));
                for (ssize_t i = 0; i < n; ++i) {
                    const char ch = buf[i];
                    if (ch == '\r' || ch == '\n') {
                        continue;  // the driver terminates commands with "\n" after the '#'
                    }
                    pending += ch;
                    if (ch == '#') {
                        handle(pending);
                        pending.clear();
                    }
                }
            }
            const int stream_ms = stream_ms_.load();
            if (stream_ms > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_stream >= std::chrono::milliseconds(stream_ms)) {
                    last_stream = now;
                    send(status_frame());
                }
            }
        }
    }

    void send(const std::string& reply) {
        if (muted_.load()) return;
        pty_write_bounded(master_fd_, reply, stop_);
    }

    // Vendor layout: "*G" + DC2..5 digits + 'U' + USB A..F digits + 'A' aht +
    // 'T' ds18 + 'D' dew6 enabled + 'M' dew6 mode + 'D' dew7 enabled + 'M'
    // dew7 mode + 'B' dew6 pwm + 'C' dew7 pwm + 'S' lens + 'H' ambient + 'V'
    // humidity + 'P' dew point + 'D' volts + 'C' amps + 'B' watts + '#'.
    std::string status_frame() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string f = "*G";
        for (int ch = 2; ch <= 5; ++ch) f += outputs_[static_cast<std::size_t>(ch)] ? '1' : '0';
        f += 'U';
        for (int ch = 6; ch <= 11; ++ch) f += outputs_[static_cast<std::size_t>(ch)] ? '1' : '0';
        f += "A1T1";
        f += "D" + std::string(dew_enabled_[0] ? "1" : "0") + "M" + std::to_string(dew_mode_[0]);
        f += "D" + std::string(dew_enabled_[1] ? "1" : "0") + "M" + std::to_string(dew_mode_[1]);
        f += "B" + std::to_string(dew_pwm_[0]) + "C" + std::to_string(dew_pwm_[1]);
        f += "S12.5H21.3V45.2P8.9";
        char tail[48];
        std::snprintf(tail, sizeof(tail), "D%.2fC0.85B11.2#", input_voltage_);
        f += tail;
        return f;
    }

    void handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        if (cmd == ">H#") {
            send("*HGeminiPowerBoxPlusAdv3#");
            return;
        }
        if (cmd == ">V#") {
            send("*V" + std::to_string(firmware_.load()) + "#");
            return;
        }
        if (cmd == ">G#") {
            send(status_frame());
            return;
        }
        // Set commands: mutate state, no acknowledgement.
        std::lock_guard<std::mutex> lock(mutex_);
        if (cmd.size() >= 4 && (cmd[1] == 'O' || cmd[1] == 'C')) {
            const int ch = std::atoi(cmd.c_str() + 2);
            if (ch >= 1 && ch <= 11) outputs_[static_cast<std::size_t>(ch)] = (cmd[1] == 'O');
        } else if (cmd.size() >= 4 && (cmd[1] == 'X' || cmd[1] == 'Y')) {
            dew_pwm_[cmd[1] == 'X' ? 0 : 1] = std::atoi(cmd.c_str() + 2);
        } else if (cmd.size() == 5 && cmd[1] == 'Z') {
            const int idx = cmd[2] - '1';
            if (idx == 0 || idx == 1) dew_enabled_[static_cast<std::size_t>(idx)] = (cmd[3] == '1');
        } else if (cmd.size() == 5 && cmd[1] == 'M') {
            const int idx = cmd[2] - '1';
            if (idx == 0 || idx == 1) dew_mode_[static_cast<std::size_t>(idx)] = cmd[3] - '0';
        }
    }

    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<int> firmware_{308};
    std::atomic<int> stream_ms_{0};
    std::atomic<bool> muted_{false};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    std::array<bool, 12> outputs_{{false, true, true, true, true, true, true, true, true, true, true, true}};
    std::array<int, 2> dew_mode_{{1, 1}};  // Manual, Manual
    std::array<int, 2> dew_pwm_{{0, 0}};
    std::array<bool, 2> dew_enabled_{{false, false}};
    double input_voltage_ = 13.1;
};

}  // namespace alpacacore::test

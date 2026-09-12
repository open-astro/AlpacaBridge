// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// AlpacaCore is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with AlpacaCore. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// A Sky-Watcher motor controller behind a pseudo-terminal, so the protocol
// wrapper's SERIAL transport (exchange_serial, settle_serial, the dirty-link
// bookkeeping and the mis-paired-reply shape check) can be exercised without
// hardware. FakeSkyWatcherMount covers the driver over UDP; this one covers
// the serial path only and speaks just enough of the ":" command set for the
// wrapper's inquiries and the step-period write/readback.
//
// Test knobs model the two failure shapes seen on an EQM-35 Pro's PL2303
// link: a reply that arrives after the wrapper's timeout (delay_next_reply),
// and an OK reply of the wrong length for the command (mispair_next). Both
// are one-shot and consumed by the next matching frame.

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeSkyWatcherSerialBoard {
public:
    FakeSkyWatcherSerialBoard() {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        // Issue #424: the master goes non-blocking here, so a reply to a
        // driver that has stopped draining can never park this fake's
        // worker inside write() and hang the destructor's join. Folded
        // into the same throw as the other setup failures: a silent
        // fallback to a blocking master would look exactly like the hang
        // this exists to remove. See fake_pty_write.h.
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0 ||
            !make_pty_nonblocking(master_fd_)) {
            throw std::runtime_error("FakeSkyWatcherSerialBoard: cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            throw std::runtime_error("FakeSkyWatcherSerialBoard: ptsname failed");
        }
        slave_path_ = name;
        // Hold the slave open so the master never sees HUP between the
        // wrapper's connect/disconnect cycles, and make it raw.
        keepalive_fd_ = open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
        struct termios tty {};
        if (keepalive_fd_ >= 0 && tcgetattr(keepalive_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(keepalive_fd_, TCSANOW, &tty);
        }
        worker_ = std::thread([this] { run(); });
    }

    ~FakeSkyWatcherSerialBoard() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (keepalive_fd_ >= 0) {
            close(keepalive_fd_);
        }
        if (master_fd_ >= 0) {
            close(master_fd_);
        }
    }

    FakeSkyWatcherSerialBoard(const FakeSkyWatcherSerialBoard&) = delete;
    FakeSkyWatcherSerialBoard& operator=(const FakeSkyWatcherSerialBoard&) = delete;

    const std::string& slave_path() const { return slave_path_; }

    /// Position counts the board reports for ":j<axis>".
    void set_counts(int axis, uint32_t counts) {
        std::lock_guard<std::mutex> lock(mutex_);
        counts_[axis - 1] = counts & 0xFFFFFF;
    }

    /// Hold the reply to the next frame (any command, or only @p command if
    /// given) for @p ms before sending it, so it lands after the wrapper's
    /// timeout: the "late reply" that the next exchange must not consume.
    void delay_next_reply(int ms, char command = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        delay_ms_ = ms;
        delay_command_ = command;
    }

    /// Answer the next @p times frames with an OK reply of the wrong length
    /// ("=00"), i.e. a reply that belongs to some other command. If
    /// @p straggler is given, it is sent @p straggler_ms after the LAST
    /// mis-paired reply, as the stale frame that was still in flight behind
    /// it: a caller that gives up on the mis-pair must settle the line, or
    /// its next command is answered by this frame instead.
    void mispair_next(int times = 1, std::string straggler = "", int straggler_ms = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        mispair_left_ = times;
        straggler_ = std::move(straggler);
        straggler_ms_ = straggler_ms;
    }

    /// The ":e1" payload (default "033A44": Wave 100i, MC 3.58 / code 0x44).
    void set_version_reply(std::string payload) {
        std::lock_guard<std::mutex> lock(mutex_);
        version_reply_ = std::move(payload);
    }

    /// Answer frames only while the wrapper has the line configured at
    /// @p baud (0 = any). A pty carries no real signalling rate, but the
    /// termios speed the wrapper sets on its fd is visible on every fd of
    /// the same slave, so this models a board that only decodes at its own
    /// rate: the 9600 probe gets silence, the 115200 one an answer.
    void answer_only_at_baud(int baud) {
        std::lock_guard<std::mutex> lock(mutex_);
        answer_baud_ = baud;
    }

    /// Refuse ":i" with "!0" (Unknown command), as a board without the
    /// step-period readback does.
    void set_no_readback(bool no_readback) {
        std::lock_guard<std::mutex> lock(mutex_);
        no_readback_ = no_readback;
    }

    /// Refuse the next ":i" with "!<code>" once (e.g. "2", Motor not stopped).
    void reject_next_readback(std::string code) {
        std::lock_guard<std::mutex> lock(mutex_);
        reject_readback_code_ = std::move(code);
    }

    /// Every frame received so far, without the ':' and the trailing CR
    /// (e.g. "j1", "I1123456").
    std::vector<std::string> frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }

    int count_frames(char command) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& f : frames_) {
            if (!f.empty() && f[0] == command) ++n;
        }
        return n;
    }

private:
    static std::string u24(uint32_t v) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        for (int byte = 0; byte < 3; ++byte) {
            const uint32_t b = (v >> (8 * byte)) & 0xFF;
            out += kHex[b >> 4];
            out += kHex[b & 0xF];
        }
        return out;
    }

    static uint32_t parse_u24(const std::string& data) {
        if (data.size() < 6) return 0;
        uint32_t v = 0;
        for (int byte = 0; byte < 3; ++byte) {
            v |= static_cast<uint32_t>(std::stoul(data.substr(byte * 2, 2), nullptr, 16)) << (8 * byte);
        }
        return v;
    }

    // Returns the reply for one frame (without the trailing CR).
    // The speed currently configured on the slave (the wrapper's fd and the
    // keepalive fd share one termios).
    int line_baud() const {
        struct termios tty {};
        if (keepalive_fd_ < 0 || tcgetattr(keepalive_fd_, &tty) != 0) return 0;
        switch (cfgetispeed(&tty)) {
            case B9600:
                return 9600;
            case B115200:
                return 115200;
            default:
                return -1;
        }
    }

    // Returns the reply for one frame (without the trailing CR), or an empty
    // string for "stay silent".
    std::string handle(const std::string& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (answer_baud_ != 0 && line_baud() != answer_baud_) {
            return "";  // wrong rate for this board: nothing decodable arrives
        }
        frames_.push_back(frame);
        if (frame.size() < 2) return "!3";
        const char cmd = frame[0];
        const int axis = frame[1] == '2' ? 2 : 1;
        const std::string data = frame.substr(2);
        if (mispair_left_ > 0) {
            if (--mispair_left_ == 0 && !straggler_.empty()) {
                straggler_pending_ = true;
            }
            return "=00";  // OK reply, wrong length for anything the wrapper asks
        }
        switch (cmd) {
            case 'e':
                return "=" + version_reply_;
            case 'a':
                return "=" + u24(4147200);
            case 'b':
                return "=" + u24(14000000);
            case 'g':
                return "=01";
            case 'j':
                return "=" + u24(counts_[axis - 1]);
            case 'f':
                return "=101";
            case 'q':
                return "=0C1000";
            case 'I':
                t1_[axis - 1] = parse_u24(data);
                return "=";
            case 'i':
                if (!reject_readback_code_.empty()) {
                    std::string code;
                    code.swap(reject_readback_code_);
                    return "!" + code;
                }
                if (no_readback_) return "!0";
                return "=" + u24(t1_[axis - 1]);
            case 'G':
            case 'S':
            case 'J':
            case 'K':
            case 'L':
            case 'E':
            case 'F':
            case 'M':
                return "=";
            default:
                return "!0";
        }
    }

    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = master_fd_;
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 10);
            if (r <= 0) continue;
            const ssize_t n = read(master_fd_, buf, sizeof(buf));
            for (ssize_t i = 0; i < n; ++i) {
                const char ch = buf[i];
                if (ch == ':') {
                    pending.clear();
                    continue;
                }
                if (ch != '\r') {
                    pending += ch;
                    continue;
                }
                const std::string frame = pending;
                pending.clear();
                const std::string body = handle(frame);
                if (body.empty()) {
                    continue;  // silent: the board did not decode the frame
                }
                const std::string reply = body + "\r";
                int delay = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (delay_ms_ > 0 && (delay_command_ == 0 || delay_command_ == frame[0])) {
                        delay = delay_ms_;
                        delay_ms_ = 0;
                        delay_command_ = 0;
                    }
                }
                if (delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                pty_write_bounded(master_fd_, reply, stop_);
                std::string straggler;
                int straggler_ms = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (straggler_pending_) {
                        straggler_pending_ = false;
                        straggler = straggler_ + "\r";
                        straggler_ms = straggler_ms_;
                    }
                }
                if (!straggler.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(straggler_ms));
                    pty_write_bounded(master_fd_, straggler, stop_);
                }
            }
        }
    }

    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    uint32_t counts_[2] = {0x800000, 0x800000};
    uint32_t t1_[2] = {0, 0};
    int delay_ms_ = 0;
    char delay_command_ = 0;
    std::string version_reply_ = "033A44";
    int answer_baud_ = 0;
    int mispair_left_ = 0;
    std::string straggler_;
    int straggler_ms_ = 0;
    bool straggler_pending_ = false;
    bool no_readback_ = false;
    std::string reject_readback_code_;
    std::vector<std::string> frames_;
};

}  // namespace alpacacore::test

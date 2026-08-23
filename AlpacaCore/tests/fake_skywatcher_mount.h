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

// Loopback UDP Sky-Watcher motor controller simulator (POSIX-only, like the
// other test socket helpers). Unlike FakeMountServer's canned replies, this
// implements the MC command set with a CONTINUOUS AXIS MODEL — counts advance
// in real time per the commanded mode/rate, gotos ramp to their target and
// stop, and the home-index registers latch when an axis sweeps past the
// simulated sensor — so the SkyWatcher driver's async state machines (slew
// dispatch + landing refinement, Park/FindHome tasks, pulse-guide timers,
// MoveAxis stop tasks) run end-to-end through the REAL protocol wrapper and
// UDP transport with no hardware and no production-code seams (issue #213).
//
// Simulated geometry matches the Wave 100i values captured in AGENTS.md:
// CPR 4147200, timer 14 MHz, high-speed ratio 1, firmware reply "=033A44",
// feature register 0x100C (home indexers present on both axes).

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace alpacacore::test {

class FakeSkyWatcherMount {
public:
    static constexpr uint32_t kCpr = 4147200;
    static constexpr uint32_t kTimerFreq = 14000000;
    static constexpr uint32_t kHome = 0x800000;
    static constexpr double kSiderealDegPerSec = 360.0 / 86164.0905;
    static constexpr double kGotoDegPerSec = 800.0 * kSiderealDegPerSec;

    FakeSkyWatcherMount() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeSkyWatcherMount() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    bool ok() const { return fd_ >= 0; }
    int port() const { return port_; }

    /// PHYSICAL axis angle in degrees — survives ":E" count re-stamps (the
    /// frame shift is tracked), so tests can assert where the axis really is.
    double physical_degrees(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        return (static_cast<double>(a.counts + a.frame_shift) - static_cast<double>(kHome)) * 360.0 / kCpr;
    }

    /// Signed axis angle in degrees from COUNT home (the controller's frame).
    double axis_degrees(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        return (static_cast<double>(a.counts) - static_cast<double>(kHome)) * 360.0 / kCpr;
    }

    bool axis_running(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        return a.running;
    }

    /// Place the simulated home-index sensor (axis degrees from count home).
    void set_home_index_degrees(int axis, double deg) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.home_index_counts =
            static_cast<int64_t>(kHome) - a.frame_shift + static_cast<int64_t>(std::llround(deg * kCpr / 360.0));
    }

    /// Move the simulated axes instantly (test setup).
    void jump_axis_degrees(int axis, double deg) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        a.counts = static_cast<int64_t>(kHome) + static_cast<int64_t>(std::llround(deg * kCpr / 360.0));
    }

private:
    struct Axis {
        int64_t counts = kHome;
        double rate_counts = 0.0;  // signed counts/sec while running
        bool running = false;
        bool speed_mode = true;
        bool fast = false;
        char dir = '0';
        uint32_t t1 = 0;
        int64_t goto_target = kHome;
        int64_t frame_shift = 0;  // physical = counts + frame_shift
        bool in_goto = false;
        bool init_done = true;
        // Home indexer: 0 = armed below the index, 0xFFFFFF = armed above,
        // else the latched count of the crossing.
        uint32_t indexer = 0;
        int64_t home_index_counts = kHome;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

        void advance(std::chrono::steady_clock::time_point t) {
            double dt = std::chrono::duration<double>(t - last).count();
            last = t;
            if (!running || dt <= 0.0) {
                return;
            }
            int64_t before = counts;
            if (in_goto) {
                double dir_sign = goto_target >= counts ? 1.0 : -1.0;
                double step = kGotoDegPerSec * kCpr / 360.0 * dt;
                double remaining = std::abs(static_cast<double>(goto_target - counts));
                if (step >= remaining) {
                    counts = goto_target;
                    running = false;
                    in_goto = false;
                } else {
                    counts += static_cast<int64_t>(std::llround(dir_sign * step));
                }
            } else {
                counts += static_cast<int64_t>(std::llround(rate_counts * dt));
            }
            // Latch the home index on a crossing while armed.
            if (indexer == 0 || indexer == 0xFFFFFF) {
                bool was_below = before < home_index_counts;
                bool is_below = counts < home_index_counts;
                if (was_below != is_below) {
                    indexer = static_cast<uint32_t>(home_index_counts & 0xFFFFFF);
                }
            }
        }

        void arm_indexer() { indexer = counts < home_index_counts ? 0u : 0xFFFFFFu; }
    };

    static std::chrono::steady_clock::time_point now() { return std::chrono::steady_clock::now(); }

    Axis& ax(int axis) { return axes_[axis == 2 ? 1 : 0]; }

    static std::string u24(uint32_t v) {
        static const char* hex = "0123456789ABCDEF";
        std::string out(6, '0');
        out[0] = hex[(v >> 4) & 0xF];
        out[1] = hex[v & 0xF];
        out[2] = hex[(v >> 12) & 0xF];
        out[3] = hex[(v >> 8) & 0xF];
        out[4] = hex[(v >> 20) & 0xF];
        out[5] = hex[(v >> 16) & 0xF];
        return out;
    }

    static uint32_t parse_u24(const std::string& d) {
        auto nib = [](char c) -> uint32_t {
            if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<uint32_t>(c - 'A' + 10);
            if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
            return 0;
        };
        if (d.size() < 6) return 0;
        return (nib(d[0]) << 4 | nib(d[1])) | (nib(d[2]) << 4 | nib(d[3])) << 8 | (nib(d[4]) << 4 | nib(d[5])) << 16;
    }

    std::string handle(const std::string& frame) {
        if (frame.size() < 3 || frame[0] != ':') {
            return "!3";
        }
        char cmd = frame[1];
        int axis = frame[2] - '0';
        std::string data = frame.substr(3);
        if (axis != 1 && axis != 2) {
            return "!0";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        switch (cmd) {
            case 'e':
                return "=033A44";  // MC firmware 3.58.68 (real Wave 100i reply)
            case 'a':
                return "=" + u24(kCpr);
            case 'b':
                return "=" + u24(kTimerFreq);
            case 'g':
                return "=01";  // high-speed ratio 1 (as the real Wave reports)
            case 'j':
                return "=" + u24(static_cast<uint32_t>(a.counts & 0xFFFFFF));
            case 'f': {
                static const char* hex = "0123456789ABCDEF";
                uint32_t n0 = (a.speed_mode ? 1u : 0u) | (a.dir == '1' ? 2u : 0u) | (a.fast ? 4u : 0u);
                uint32_t n1 = a.running ? 1u : 0u;
                uint32_t n2 = a.init_done ? 1u : 0u;
                std::string out = "=";
                out += hex[n0];
                out += hex[n1];
                out += hex[n2];
                return out;
            }
            case 'E': {
                if (a.running) return "!2";
                int64_t fresh = static_cast<int64_t>(parse_u24(data));
                a.frame_shift += a.counts - fresh;  // physical position unchanged
                a.counts = fresh;
                return "=";
            }
            case 'F':
                a.init_done = true;
                return "=";
            case 'G':
                if (data.size() < 2) return "!1";
                // mode: '0' goto fast, '1' speed slow, '2' goto slow, '3' speed fast
                a.speed_mode = data[0] == '1' || data[0] == '3';
                a.fast = data[0] == '0' || data[0] == '3';
                a.in_goto = !a.speed_mode;
                a.dir = data[1];
                return "=";
            case 'S':
                a.goto_target = static_cast<int64_t>(parse_u24(data));
                return "=";
            case 'I': {
                a.t1 = parse_u24(data);
                double cps = a.t1 > 0 ? static_cast<double>(kTimerFreq) / a.t1 : 0.0;
                a.rate_counts = a.dir == '1' ? -cps : cps;
                return "=";
            }
            case 'J':
                a.running = true;
                if (a.in_goto) {
                    a.goto_target &= 0xFFFFFF;
                }
                return "=";
            case 'K':  // ramped stop — modeled as immediate for determinism
            case 'L':
                a.running = false;
                a.in_goto = false;
                return "=";
            case 'q': {
                uint32_t inquiry = parse_u24(data);
                if (inquiry == 0x000001) return "=0C1000";  // features 0x100C
                if (inquiry == 0x000000) return "=" + u24(a.indexer);
                return "!0";
            }
            case 'W': {
                uint32_t w = parse_u24(data);
                if (w == 0x000008) {
                    a.arm_indexer();
                    return "=";
                }
                return "=";
            }
            case 'O':
            case 'P':
            case 'V':
                return "=";
            default:
                return "!0";
        }
    }

    void serve() {
        char buf[64];
        while (!stop_.load()) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            ssize_t n = ::recvfrom(fd_, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (n <= 0) {
                continue;
            }
            std::string frame(buf, static_cast<size_t>(n));
            while (!frame.empty() && (frame.back() == '\r' || frame.back() == '\n')) {
                frame.pop_back();
            }
            std::string reply = handle(frame) + "\r";
            ::sendto(fd_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&peer), plen);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    Axis axes_[2];
};

}  // namespace alpacacore::test

#endif  // _WIN32

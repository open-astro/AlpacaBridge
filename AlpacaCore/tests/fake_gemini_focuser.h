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

// Hardware-free MyFocuserPro2 firmware behind a pseudo-terminal, for the
// Gemini focuser driver (open-astro#333). The driver opens the pty slave
// exactly like its CH340 USB-serial bridge; this fake answers on the master
// with the request/response pairs the protocol wrapper sends:
//
//   ":03#" -> "F<firmware>#"    ":00#" -> "P<position>#"
//   ":01#" -> "I<moving>#"      ":06#" -> "Z<temperature>#"
//   ":08#" -> "M<maxpos>#"      ":11#" -> "O<coilpower>#"
//   ":13#" -> "R<reverse>#"     ":24#" -> "1<tempcomp>#"
//   ":26#" -> "B<coefficient>#" ":29#" -> "S<stepmode>#"
//   ":43#" -> "C<speed>#"
//
// Blind commands (":05" move, ":07" max, ":12", ":14", ":15", ":16") are
// recorded and not answered, as the firmware does.
//
// The observable that matters for #333 is how many connects reached the
// wire: the fake holds the pty master, which sees nothing when the slave is
// opened, so a connect is counted by its handshake command (":1502#", see
// connects() below). A concurrent connect that gets past the driver's
// transition_mutex_ sends a second handshake and leaks the first descriptor.
// A settable handshake delay widens the window so the race is reproducible
// rather than timing-dependent.

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>  // posix_openpt/grantpt/unlockpt/ptsname: POSIX, not the <cstdlib> subset
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeGeminiFocuser {
public:
    FakeGeminiFocuser() {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        // Issue #424: non-blocking, so a reply to a driver that has stopped
        // draining can never park this fake's worker inside write() and
        // hang the destructor's join. See fake_pty_write.h.
        if (master_fd_ >= 0) {
            make_pty_nonblocking(master_fd_);
        }
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0) {
            throw std::runtime_error("FakeGeminiFocuser: cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            throw std::runtime_error("FakeGeminiFocuser: ptsname failed");
        }
        slave_path_ = name;
        // Keep a slave handle open so the master never sees EIO between the
        // driver's disconnect (close) and reconnect (open).
        keepalive_fd_ = open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
        struct termios tty {};
        if (keepalive_fd_ >= 0 && tcgetattr(keepalive_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(keepalive_fd_, TCSANOW, &tty);
        }
        reader_ = std::thread([this] { run(); });
    }

    ~FakeGeminiFocuser() {
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

    FakeGeminiFocuser(const FakeGeminiFocuser&) = delete;
    FakeGeminiFocuser& operator=(const FakeGeminiFocuser&) = delete;

    const std::string& slave_path() const { return slave_path_; }

    /// Every command received so far, in wire order (e.g. ":03#", ":05100#").
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    /// Number of received commands starting with `prefix`.
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

    /// How many connects have reached the wire. Counts ":1502#", the speed
    /// command the wrapper sends exactly once at the end of a successful
    /// connect, rather than ":03#": the handshake command is also what
    /// get_firmware_version() sends, which the driver calls right after every
    /// connect, so ":03#" arrives twice per session and cannot separate a
    /// second connect from an ordinary firmware read.
    ///
    /// One collision to know about: GeminiProtocolWrapper::set_speed(2) emits
    /// the same ":1502#" (":150%d#"). No test calls it today, so the count is
    /// exact; a test that does would inflate every connects() assertion in
    /// this file.
    int connects() const { return count(":1502#"); }

    /// Hold the reply to the handshake for `delay`, widening the window
    /// between a driver's idempotency check and its connected_ store.
    void set_handshake_delay(std::chrono::milliseconds delay) { handshake_delay_ms_.store(delay.count()); }

    int position() const { return position_.load(); }
    int max_position() const { return max_position_.load(); }

private:
    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = master_fd_;
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 20);
            if (r <= 0) {
                continue;
            }
            const ssize_t n = read(master_fd_, buf, sizeof(buf));
            if (n <= 0) {
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) {
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
        std::string reply;
        if (cmd == ":03#") {
            const auto delay = handshake_delay_ms_.load();
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            reply = "F" + std::to_string(kFirmware) + "#";
        } else if (cmd == ":00#") {
            reply = "P" + std::to_string(position_.load()) + "#";
        } else if (cmd == ":01#") {
            reply = "I0#";  // never moving: the tests here are about connect
        } else if (cmd == ":06#") {
            reply = "Z12.5#";
        } else if (cmd == ":08#") {
            reply = "M" + std::to_string(max_position_.load()) + "#";
        } else if (cmd == ":11#") {
            reply = "O1#";
        } else if (cmd == ":13#") {
            reply = "R0#";
        } else if (cmd == ":24#") {
            reply = "10#";
        } else if (cmd == ":26#") {
            reply = "B0#";
        } else if (cmd == ":29#") {
            reply = "S1#";
        } else if (cmd == ":43#") {
            reply = "C2#";
        } else if (cmd.rfind(":05", 0) == 0) {
            position_.store(std::atoi(cmd.c_str() + 3));
            return;  // blind
        } else if (cmd.rfind(":07", 0) == 0) {
            max_position_.store(std::atoi(cmd.c_str() + 3));
            return;  // blind
        } else {
            return;  // every other write is blind on this firmware
        }
        pty_write_bounded(master_fd_, reply, stop_);
    }

    static constexpr int kFirmware = 311;

    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<long long> handshake_delay_ms_{0};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;

    std::atomic<int> position_{1000};
    std::atomic<int> max_position_{50000};
};

}  // namespace alpacacore::test

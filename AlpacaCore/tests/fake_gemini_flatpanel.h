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

// Hardware-free Gemini "Pro" (Motorized Flat Panel V3) firmware behind a
// pseudo-terminal. The driver opens the pty slave exactly like a USB-serial
// port; this fake answers on the master with the replies captured from the
// real panel (firmware 107, 2026-08-25): ">H#" -> "*HGeminiFlatPanelPro#",
// ">S#" -> "*S0M<light>L<cover>C0D76C405O#", ">O#" -> "*O405#", ">C#" ->
// "*C70#". Per-command reply delays let a test hold the port the way a
// slow light command or a 10 s cover move does on hardware, which is what
// the driver's fast-path / background-path selection keys on.

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

class FakeGeminiFlatPanel {
public:
    FakeGeminiFlatPanel() {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0) {
            throw std::runtime_error("FakeGeminiFlatPanel: cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            throw std::runtime_error("FakeGeminiFlatPanel: ptsname failed");
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

    ~FakeGeminiFlatPanel() {
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

    FakeGeminiFlatPanel(const FakeGeminiFlatPanel&) = delete;
    FakeGeminiFlatPanel& operator=(const FakeGeminiFlatPanel&) = delete;

    const std::string& slave_path() const { return slave_path_; }

    /// Every command received so far, in wire order (e.g. ">L#", ">B128#").
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

    /// Index (wire order) of the first received command starting with
    /// `prefix`, or -1.
    int index_of(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < commands_.size(); ++i) {
            if (commands_[i].rfind(prefix, 0) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /// Hold the reply to any command starting with `prefix` for `delay`,
    /// simulating a slow firmware command or a mechanical cover move.
    void set_reply_delay(const std::string& prefix, std::chrono::milliseconds delay) {
        std::lock_guard<std::mutex> lock(mutex_);
        delays_.emplace_back(prefix, delay);
    }

    bool light_on() const { return light_on_.load(); }
    int brightness() const { return brightness_.load(); }
    int cover() const { return cover_.load(); }  // 1 = closed, 2 = open (FlatPanelCoverState)

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

    std::chrono::milliseconds delay_for(const std::string& cmd) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [prefix, delay] : delays_) {
            if (cmd.rfind(prefix, 0) == 0) {
                return delay;
            }
        }
        return std::chrono::milliseconds(0);
    }

    void handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        std::string reply;
        if (cmd == ">H#") {
            reply = "*HGeminiFlatPanelPro#";
        } else if (cmd == ">V#") {
            reply = "*V107#";
        } else if (cmd == ">S#") {
            reply = std::string("*S0M") + (light_on_.load() ? '1' : '0') + "L" + std::to_string(cover_.load()) +
                    "C0D76C405O#";
        } else if (cmd == ">J#") {
            reply = "*J" + std::to_string(brightness_.load()) + "#";
        } else if (cmd == ">L#") {
            light_on_.store(true);
            reply = "*L#";
        } else if (cmd == ">D#") {
            light_on_.store(false);
            reply = "*D#";
        } else if (cmd.rfind(">B", 0) == 0) {
            brightness_.store(std::atoi(cmd.c_str() + 2));
            reply = "*B" + std::to_string(brightness_.load()) + "#";
        } else if (cmd == ">O#") {
            reply = "*O405#";
        } else if (cmd == ">C#") {
            reply = "*C70#";
        } else {
            reply = "*?#";
        }
        const auto delay = delay_for(cmd);
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        // The cover reaches its end stop when the ack goes out (as on hardware,
        // where the ack arrives only once travel has finished).
        if (cmd == ">O#") {
            cover_.store(2);
        } else if (cmd == ">C#") {
            cover_.store(1);
        }
        (void)!write(master_fd_, reply.data(), reply.size());
    }

    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
    std::thread reader_;
    std::atomic<bool> stop_{false};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    std::vector<std::pair<std::string, std::chrono::milliseconds>> delays_;

    std::atomic<bool> light_on_{false};
    std::atomic<int> brightness_{0};
    std::atomic<int> cover_{1};
};

}  // namespace alpacacore::test

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

// Hardware-free QHYCFW3 firmware behind a pseudo-terminal. The driver opens
// the pty slave exactly like the wheel's CP2102 port; this fake answers on
// the master with the bare-ASCII replies a real 7-slot CFW3 gave on
// 2026-09-15 (firmware 20181114):
//
//   "VRS"        -> "20181114"
//   "MXP"        -> '7'                (slot COUNT on this firmware)
//   "NOW"        -> current slot char
//   '0'..'F'     -> after travel_ms per slot, the arrived slot char; a slot
//                   at or beyond the count is silently ignored (hardware
//                   behaviour); a same-slot goto answers at once
//   "RESET"      -> re-home to slot 0 and emit '0'
//
// A pty has no DTR, so the ~17 s post-reset boot byte the real wheel emits
// after the port is opened cannot happen on its own here: emit_boot_byte()
// writes it on demand, and set_old_firmware(true) makes the fake ignore
// VRS/MXP/NOW the way firmware before 201409 does.

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeQhyCfw3 {
public:
    explicit FakeQhyCfw3(int slot_count = 7) : slot_count_(slot_count), pty_("FakeQhyCfw3") {
        reader_ = std::thread([this] { run(); });
    }

    ~FakeQhyCfw3() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeQhyCfw3(const FakeQhyCfw3&) = delete;
    FakeQhyCfw3& operator=(const FakeQhyCfw3&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// Close both pty ends: the driver's next read or write sees the port
    /// vanish (POLLHUP / EOF / EIO), like a mid-session USB unplug.
    void sever() { pty_.sever(); }

    /// Every command received so far, in wire order ("VRS", "MXP", "NOW",
    /// "RESET", or a single goto character).
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

    /// Handshakes that reached the wire: one MXP per connect.
    int connects() const { return count("MXP"); }

    int position() const { return position_.load(); }
    void set_position(int p) { position_.store(p); }

    /// Simulated travel time per slot moved (0 = arrives at once).
    void set_travel_ms(int ms) { travel_ms_.store(ms); }

    /// Firmware before 201409: no VRS/MXP/NOW, no same-slot reply.
    void set_old_firmware(bool on) { old_firmware_.store(on); }

    /// Stop answering anything (hung MCU, healthy fd).
    void set_muted(bool on) { muted_.store(on); }
    /// One shot, then spent: the reply to the next query (VRS, MXP, NOW or RESET) is held for @p delay; a lone-hex
    /// goto does not spend it. A connect waiting on that reply stays open that long; used by the contract sweep to
    /// make Connecting observable.
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }

    /// Write the post-reset boot byte (the wheel's position) now, as the real
    /// wheel does ~17 s after the port is opened.
    void emit_boot_byte() {
        const char ch = slot_char(position_.load());
        pty_write_bounded(pty_.master_fd(), &ch, 1, stop_);
    }

private:
    static char slot_char(int slot) {
        return slot < 10 ? static_cast<char>('0' + slot) : static_cast<char>('A' + slot - 10);
    }
    static int slot_from_char(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
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
                const char ch = buf[i];
                if (pending.empty() && slot_from_char(ch) >= 0) {
                    // A lone hex digit is a goto. The query commands start
                    // with V, M, N or R, none of which is a hex digit, so a
                    // first byte never needs disambiguating.
                    handle_goto(slot_from_char(ch));
                    continue;
                }
                pending += ch;
                if (pending == "VRS" || pending == "MXP" || pending == "NOW" || pending == "RESET") {
                    handle_query(pending);
                    pending.clear();
                } else if (pending.size() >= 5 || (pending.size() >= 3 && pending != "RES" && pending != "RESE")) {
                    pending.clear();  // not a command the firmware knows; it stays silent
                }
            }
        }
    }

    void record(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(cmd);
    }

    void reply(const std::string& text) {
        if (muted_.load()) return;
        pty_write_bounded(pty_.master_fd(), text, stop_);
    }

    void handle_query(const std::string& cmd) {
        record(cmd);
        if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        if (cmd == "RESET") {
            position_.store(0);
            reply("0");
            return;
        }
        if (old_firmware_.load()) return;
        if (cmd == "VRS") reply("20181114");
        if (cmd == "MXP") reply(std::string(1, slot_char(slot_count_)));
        if (cmd == "NOW") reply(std::string(1, slot_char(position_.load())));
    }

    void handle_goto(int slot) {
        record(std::string(1, slot_char(slot)));
        if (slot < 0 || slot >= slot_count_) return;  // hardware: silently ignored
        const int from = position_.load();
        if (from == slot) {
            if (!old_firmware_.load()) reply(std::string(1, slot_char(slot)));
            return;
        }
        // Shortest path, like the real direct-drive wheel.
        int distance = std::abs(slot - from);
        distance = std::min(distance, slot_count_ - distance);
        const auto travel = std::chrono::milliseconds(travel_ms_.load() * distance);
        const auto deadline = std::chrono::steady_clock::now() + travel;
        while (std::chrono::steady_clock::now() < deadline && !stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (stop_.load()) return;
        position_.store(slot);
        reply(std::string(1, slot_char(slot)));
    }

    int slot_count_;
    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;

    std::atomic<int> position_{0};
    std::atomic<int> travel_ms_{0};
    std::atomic<bool> old_firmware_{false};
    std::atomic<bool> muted_{false};
    std::atomic<int> hold_ms_{0};
};

}  // namespace alpacacore::test

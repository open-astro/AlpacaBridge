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

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeGeminiFocuser {
public:
    FakeGeminiFocuser() : pty_("FakeGeminiFocuser") {
        // The pty pair is owned by pty_ (fake_pty_write.h), constructed
        // before this body runs; a setup failure throws from there with
        // nothing left open (issue #387).
        reader_ = std::thread([this] { run(); });
    }

    ~FakeGeminiFocuser() {
        stop_.store(true);
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);  // wake a reader parked on the gate
        }
        gate_cv_.notify_all();
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    FakeGeminiFocuser(const FakeGeminiFocuser&) = delete;
    FakeGeminiFocuser& operator=(const FakeGeminiFocuser&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }
    /// The keep-alive slave fd: shares the driver's termios, so a test can read
    /// back (or retune, as another program would) the line settings (#1202).
    int keepalive_fd() const { return pty_.keepalive_fd(); }

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

    /// What ":24#" reports for temperature compensation ("11#" on, "10#"
    /// off). Settable so a test can pin the TRUE branch: an assertion that
    /// only ever sees "off" would not show that the round trip worked.
    void set_temp_comp_reply(bool on) { temp_comp_on_.store(on); }

    /// What ":01#" and ":06#" report; the position is set by a move or here.
    /// Answer the three state queries with a parseable-looking but invalid
    /// frame at once ("Px#"), as a wedged firmware does, without the serial
    /// timeout a silent fake costs.
    void set_garbage_state_replies(bool on) { garbage_state_.store(on); }
    /// ":06#" answers garbage while the other state queries stay good (a
    /// focuser without a temperature probe).
    void set_garbage_temperature(bool on) { garbage_temperature_.store(on); }
    /// ":24#" answers a frame the temperature compensation parser rejects ("Qx#"), the other queries stay good.
    void set_garbage_temp_comp(bool on) { garbage_temp_comp_.store(on); }
    void set_moving(bool moving) { moving_.store(moving); }
    void set_temperature(double celsius) { temperature_.store(celsius); }
    void set_position(int position) { position_.store(position); }

    int position() const { return position_.load(); }
    int max_position() const { return max_position_.load(); }

    /// Put `bytes` in the driver's receive queue as an unsolicited reply, the
    /// way a late answer to a timed-out command lands.
    void push_unsolicited(const std::string& bytes) { pty_write_bounded(pty_.master_fd(), bytes, stop_); }

    /// One shot, then spent: the next reply the fake sends is held for @p delay (blind commands do not use it up).
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }

    /// One shot, then spent: the next reply is held until release_reply(), with no timer. wait_reply_held() blocks
    /// until the reader thread is holding it.
    void gate_next_reply() { gate_armed_.store(true); }
    bool wait_reply_held(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(gate_mutex_);
        return gate_cv_.wait_for(lock, bound, [this] { return gate_held_; });
    }
    void release_reply() {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        gate_open_ = true;
        gate_cv_.notify_all();
    }

    /// A USB unplug: the reader stops and both pty ends close, so the driver's reads fail with EIO from here on.
    /// Not reversible.
    void unplug() {
        stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
        pty_.sever();
    }

private:
    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 20);
            if (r <= 0) {
                continue;
            }
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
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
        } else if (garbage_state_.load() && (cmd == ":00#" || cmd == ":01#" || cmd == ":06#")) {
            reply = "Px#";
        } else if (garbage_temperature_.load() && cmd == ":06#") {
            reply = "Zx#";
        } else if (cmd == ":00#") {
            reply = "P" + std::to_string(position_.load()) + "#";
        } else if (cmd == ":01#") {
            reply = moving_.load() ? "I1#" : "I0#";
        } else if (cmd == ":06#") {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Z%.1f#", temperature_.load());
            reply = buf;
        } else if (cmd == ":08#") {
            reply = "M" + std::to_string(max_position_.load()) + "#";
        } else if (cmd == ":11#") {
            reply = "O1#";
        } else if (cmd == ":13#") {
            reply = "R0#";
        } else if (cmd == ":24#" && garbage_temp_comp_.load()) {
            reply = "Qx#";
        } else if (cmd == ":24#") {
            reply = temp_comp_on_.load() ? "11#" : "10#";
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
        if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        }
        if (gate_armed_.exchange(false)) {
            std::unique_lock<std::mutex> lock(gate_mutex_);
            gate_held_ = true;
            gate_cv_.notify_all();
            gate_cv_.wait(lock, [this] { return gate_open_ || stop_.load(); });
        }
        pty_write_bounded(pty_.master_fd(), reply, stop_);
    }

    static constexpr int kFirmware = 311;

    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<long long> handshake_delay_ms_{0};
    std::atomic<int> hold_ms_{0};
    std::atomic<bool> gate_armed_{false};
    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool gate_held_ = false;
    bool gate_open_ = false;
    std::atomic<bool> temp_comp_on_{false};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;

    std::atomic<bool> garbage_state_{false};
    std::atomic<bool> garbage_temperature_{false};
    std::atomic<bool> garbage_temp_comp_{false};
    std::atomic<bool> moving_{false};
    std::atomic<double> temperature_{12.5};
    std::atomic<int> position_{1000};
    std::atomic<int> max_position_{50000};
};

}  // namespace alpacacore::test

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

// Hardware-free QHY Q-Focuser firmware behind a pseudo-terminal. The driver
// opens the pty slave exactly like the focuser's GD32 CDC-ACM port; this
// fake answers on the master with the JSON request/response pairs the
// protocol wrapper sends (replies copied from a real unit, firmware
// 20231207):
//
//   {"cmd_id":1}            -> {"idx":1,"id":"...","version":20231207,"bv":208}
//   {"cmd_id":5}            -> {"idx":5,"pos":<position>}
//   {"cmd_id":4}            -> {"idx":4,"temp":..,"c_t":..,"c_r":..,"o_t":..,"sg":0}
//   {"cmd_id":6,"tar":N}    -> {"idx":6}   (position steps toward N on each poll)
//   {"cmd_id":3}            -> {"idx":3}   (stops at the current position)
//   7 / 12 / 13 / 16        -> {"idx":<same>}
//
// Motion is simulated as `steps_per_poll` steps per position query, so a
// test can watch IsMoving flip without real time passing.

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeQhyQFocuser {
public:
    FakeQhyQFocuser() : pty_("FakeQhyQFocuser") {
        reader_ = std::thread([this] { run(); });
    }

    ~FakeQhyQFocuser() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeQhyQFocuser(const FakeQhyQFocuser&) = delete;
    FakeQhyQFocuser& operator=(const FakeQhyQFocuser&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// Close both pty ends now, so the driver's next read sees the port vanish
    /// (POLLHUP / EOF / EIO) exactly like a mid-session USB unplug. Used to test
    /// that transact_locked() fails fast instead of busy-spinning to timeout.
    void sever() { pty_.sever(); }

    /// Every command received so far, in wire order.
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    /// Number of received commands containing `needle` (e.g. "\"cmd_id\":6").
    int count(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    /// Handshakes that reached the wire: one per connect.
    int connects() const { return count("\"cmd_id\":1}"); }

    int position() const { return position_.load(); }
    void set_position(int p) { position_.store(p); }
    /// Steps moved toward the target per position poll (0 = a stalled motor).
    void set_steps_per_poll(int n) { steps_per_poll_.store(n); }
    /// Supply voltage reported in c_r (tenths of a volt): 125 = 12.5 V.
    void set_voltage_tenths(int v) { voltage_tenths_.store(v); }
    /// One shot, then spent: the reply to the next command handled, whichever it is (even an unknown id the fake then
    /// stays silent on) is held for @p delay. A connect waiting on that reply stays open that long; used by the
    /// contract sweep to make Connecting observable.
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }

    /// Model the real GD32 firmware's one-reply-behind behaviour: each reply is
    /// held until a later inbound OUT (a command or the driver's newline kick)
    /// clocks it out. With this on, a driver that never kicks gets no reply and
    /// times out. Note a pty has no USB OUT-packet boundary, so this does NOT
    /// reproduce the coalescing that makes tcdrain matter, nor the proactive-
    /// vs-fallback kick timing — those are validated on hardware. Off by
    /// default so the other tests keep deterministic (non-lagged) reads.
    void set_one_behind(bool on) { one_behind_.store(on); }

    /// Answer every command AND every newline kick with an overlong brace-less
    /// blob (longer than the wrapper's kMaxReplyLen, no closing '}'), the shape
    /// of a babbling-but-present tty. The wrapper's reader gives up on such a
    /// reply at once instead of at its slice deadline, which is the path that
    /// used to re-kick in a tight loop (issue #527). kicks() counts the
    /// newlines received so a test can bound the re-kick rate.
    void set_garbage(bool on) { garbage_.store(on); }
    int kicks() const { return kicks_.load(); }

private:
    void run() {
        std::string pending;
        char buf[128];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 20) <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            for (ssize_t i = 0; i < n; ++i) {
                if (pending.empty() && buf[i] != '{') {
                    if (buf[i] == '\n') {
                        kicks_.fetch_add(1);
                        if (garbage_.load()) {
                            pty_write_bounded(pty_.master_fd(), std::string(300, '{'), stop_);
                            continue;
                        }
                    }
                    // In one-behind mode a newline "kick" (which the driver
                    // sends after tcdrain-ing the command out) clocks out the
                    // held reply, exactly as the real GD32 firmware does on the
                    // next OUT packet. Outside that mode it is ignored, as
                    // before.
                    if (one_behind_.load() && buf[i] == '\n' && !held_reply_.empty()) {
                        pty_write_bounded(pty_.master_fd(), held_reply_, stop_);
                    }
                    continue;
                }
                pending += buf[i];
                if (buf[i] == '}') {
                    handle(pending);
                    pending.clear();
                }
            }
        }
    }

    static int field(const std::string& cmd, const char* key) {
        const std::string k = std::string("\"") + key + "\":";
        const auto at = cmd.find(k);
        if (at == std::string::npos) return -1;
        return std::atoi(cmd.c_str() + at + k.size());
    }

    void handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        const int id = field(cmd, "cmd_id");
        std::string reply;
        if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));

        switch (id) {
            case 1:
                reply = "{\"idx\":1,\"id\":\"\\u001f@SL3KG\\u0018TH2C\",\"version\":20231207,\"bv\":208}";
                break;
            case 5: {
                int pos = position_.load();
                if (moving_.load()) {
                    const int tar = target_.load();
                    const int step = steps_per_poll_.load();
                    if (pos < tar)
                        pos = (tar - pos <= step) ? tar : pos + step;
                    else if (pos > tar)
                        pos = (pos - tar <= step) ? tar : pos - step;
                    position_.store(pos);
                    if (pos == tar) moving_.store(false);
                }
                reply = "{\"idx\":5,\"pos\":" + std::to_string(pos) + "}";
                break;
            }
            case 4:
                reply = "{\"idx\":4,\"temp\":120683,\"c_t\":18727,\"c_r\":" + std::to_string(voltage_tenths_.load()) +
                        ",\"o_t\":23881,\"sg\":0}";
                break;
            case 6:
                target_.store(field(cmd, "tar"));
                moving_.store(true);
                reply = "{\"idx\":6}";
                break;
            case 3:
                moving_.store(false);
                reply = "{\"idx\":3}";
                break;
            case 7:
            case 12:
            case 13:
            case 16:
                reply = "{\"idx\":" + std::to_string(id) + "}";
                break;
            default:
                return;  // unknown command: the firmware stays silent
        }
        if (garbage_.load()) {
            pty_write_bounded(pty_.master_fd(), std::string(300, '{'), stop_);
            return;
        }
        if (one_behind_.load()) {
            // Model the real firmware: this command's OUT transmits the
            // PREVIOUS reply, and the freshly computed one is held until the
            // next OUT (a later command, or the driver's newline kick above).
            // A driver that writes a command and reads without kicking sees
            // only the stale reply and never this one -- which is the failure
            // the driver's tcdrain+kick sequence exists to fix, so a test in
            // this mode fails if that sequence is removed.
            if (!held_reply_.empty()) pty_write_bounded(pty_.master_fd(), held_reply_, stop_);
            held_reply_ = reply;
            return;
        }
        pty_write_bounded(pty_.master_fd(), reply, stop_);
    }

    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<int> hold_ms_{0};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;

    std::atomic<int> position_{1000};
    std::atomic<int> target_{1000};
    std::atomic<bool> moving_{false};
    std::atomic<int> steps_per_poll_{100000};  // default: moves complete on the first poll
    std::atomic<int> voltage_tenths_{125};

    // one_behind_ is set before the driver connects and only read on the
    // reader thread thereafter; held_reply_ is touched solely on that thread
    // (run() and the handle() it calls), so it needs no lock.
    std::atomic<bool> one_behind_{false};
    std::string held_reply_;
    std::atomic<bool> garbage_{false};
    std::atomic<int> kicks_{0};
};

}  // namespace alpacacore::test

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

// Hardware-free iOptron iEAF / iAFS2/3 focuser behind a pseudo-terminal. The
// driver opens the pty slave exactly like the focuser's PL2303 port; this fake
// answers on the master with the fixed-width replies the protocol wrapper
// parses (formats from INDI ieaffocus.cpp, confirmed on an iEAF, model 2,
// firmware 100):
//
//   :DeviceInfo#   -> "%+06d%02d%04d#"   position, model code, firmware
//   :FI#           -> "%+07d%1d%05d%1d#" position, moving, temp (K x 100), dir
//   (sign + zero padding, as the hardware sends; the wrapper's fixed-width
//   sscanf over-reads space padding)
//   :FM<7u>#       -> "1"               move (position jumps to target)
//   :FQ# / :FZ#    -> "1"               abort / zero
//
// Motion completes on the poll after the move, which is enough for the
// lifecycle tests this fake exists for (issue #528); set_focuser_moving(true)
// keeps :FI# reporting the motor running and set_silent(true) stops every
// reply, for a failure in mid-move.
//
// The same fake answers the iEFW filter wheel (model 99 or 98) with its own commands, so the wheel
// driver can run over it: :FW1# -> "<12 chars>#" firmware, :WP# -> "n#" slot or "-1#" while moving,
// :WM<2d># -> "1" move, :WF<2d># -> "+00000#" stored offset. A move reads -1 for wheel_moving_polls()
// :WP# polls, then the target. fail_wheel_position(true) makes :WP# answer "ERR#" (a reply the
// wrapper cannot parse, so the read throws at once rather than after the serial timeout).

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeIoptronIeaf {
public:
    FakeIoptronIeaf() : pty_("FakeIoptronIeaf") {
        reader_ = std::thread([this] { run(); });
    }

    ~FakeIoptronIeaf() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeIoptronIeaf(const FakeIoptronIeaf&) = delete;
    FakeIoptronIeaf& operator=(const FakeIoptronIeaf&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// Every command received so far, in wire order.
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    int count(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    /// Handshakes that reached the wire: one per connect.
    int connects() const { return count(":DeviceInfo#"); }

    int position() const { return position_.load(); }
    void set_position(int p) { position_.store(p); }
    /// One shot, then spent: the reply to the next command handled, whichever it is (even one the fake then stays
    /// silent on) is held for @p delay. A connect waiting on that reply stays open that long; used by the contract
    /// sweep to make Connecting observable.
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }
    /// The slot :WP# reports once the wheel has settled.
    int wheel_slot() const { return wheel_slot_.load(); }
    void set_wheel_slot(int slot) { wheel_slot_.store(slot); }
    /// :WP# polls that answer -1 after a :WM move to a different slot (default 0: the move settles at once).
    void set_wheel_moving_polls(int polls) { wheel_moving_polls_.store(polls); }
    void fail_wheel_position(bool fail) { fail_wp_.store(fail); }
    /// While true, :FI# reports the focuser moving whatever its position: a motor still running after the move
    /// (the default reports it stopped on the first poll).
    void set_focuser_moving(bool moving) { focuser_moving_.store(moving); }
    /// While true, every command is recorded and left unanswered, as a firmware that hangs with the port open does.
    void set_silent(bool silent) { silent_.store(silent); }
    /// A USB unplug: every driver write fails with EIO from here on, so a move throws. Not reversible.
    void unplug() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
        pty_.sever();
    }
    /// Model code answered in the handshake: 2 = iEAF (default), 3 = iAFS2/3.
    void set_model(int m) { model_.store(m); }

private:
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
                if (pending.empty() && buf[i] != ':') continue;  // skip stray bytes
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
        if (silent_.load()) return;
        char reply[40] = {};
        if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        if (cmd == ":DeviceInfo#") {
            std::snprintf(reply, sizeof(reply), "%+06d%02d%04d#", position_.load(), model_.load(), 100);
        } else if (cmd == ":FI#") {
            // Temperature 20.00 C = 29315 (Kelvin x 100); dir 1 = not reversed.
            // Sign plus zero-padded digits, as the real unit sends ("+013211..."): the
            // fixed-width sscanf in the wrapper over-reads space padding.
            std::snprintf(reply, sizeof(reply), "%+07d%1d%05d%1d#", position_.load(), focuser_moving_.load() ? 1 : 0,
                          29315, 1);
        } else if (cmd.rfind(":FM", 0) == 0) {
            position_.store(std::atoi(cmd.c_str() + 3));
            std::snprintf(reply, sizeof(reply), "1");
        } else if (cmd == ":FW1#") {
            std::snprintf(reply, sizeof(reply), "FAKEFW000001#");
        } else if (cmd == ":WP#") {
            if (fail_wp_.load()) {
                std::snprintf(reply, sizeof(reply), "ERR#");
            } else if (moving_left_.load() > 0) {
                if (moving_left_.fetch_sub(1) == 1) wheel_slot_.store(wheel_target_.load());
                std::snprintf(reply, sizeof(reply), "-1#");
            } else {
                std::snprintf(reply, sizeof(reply), "%d#", wheel_slot_.load());
            }
        } else if (cmd.rfind(":WM", 0) == 0) {
            const int target = std::atoi(cmd.c_str() + 3);
            wheel_target_.store(target);
            if (wheel_moving_polls_.load() > 0 && target != wheel_slot_.load()) {
                moving_left_.store(wheel_moving_polls_.load());
            } else {
                wheel_slot_.store(target);
            }
            std::snprintf(reply, sizeof(reply), "1");
        } else if (cmd.rfind(":WF", 0) == 0) {
            std::snprintf(reply, sizeof(reply), "+00000#");
        } else if (cmd == ":FQ#") {
            std::snprintf(reply, sizeof(reply), "1");
        } else if (cmd == ":FZ#") {
            position_.store(0);
            std::snprintf(reply, sizeof(reply), "1");
        } else {
            return;  // unknown command: the firmware stays silent
        }
        pty_write_bounded(pty_.master_fd(), std::string(reply), stop_);
    }

    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<int> hold_ms_{0};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;

    std::atomic<int> position_{1000};
    std::atomic<int> model_{2};
    std::atomic<int> wheel_slot_{0};
    std::atomic<int> wheel_target_{0};
    std::atomic<int> wheel_moving_polls_{0};
    std::atomic<int> moving_left_{0};
    std::atomic<bool> fail_wp_{false};
    std::atomic<bool> focuser_moving_{false};
    std::atomic<bool> silent_{false};
};

}  // namespace alpacacore::test

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

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeSkyWatcherSerialBoard {
public:
    FakeSkyWatcherSerialBoard() : pty_("FakeSkyWatcherSerialBoard") {
        // The pty pair is owned by pty_ (fake_pty_write.h), constructed
        // before this body runs; a setup failure throws from there with
        // nothing left open (issue #387). Its keep-alive slave is what holds
        // the master clear of HUP between the wrapper's connect/disconnect
        // cycles.
        worker_ = std::thread([this] { run(); });
    }

    ~FakeSkyWatcherSerialBoard() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    FakeSkyWatcherSerialBoard(const FakeSkyWatcherSerialBoard&) = delete;
    FakeSkyWatcherSerialBoard& operator=(const FakeSkyWatcherSerialBoard&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    // open-astro#445: pull the cable. Closing the master is what a USB-serial
    // adapter leaving the bus does to the driver's side of the link: the node
    // is removed (the held fd's link count drops to 0), writes fail with EIO
    // and reads return 0. Idempotent (PtyPair::sever); slave_path() is empty after it.
    void sever_link() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        pty_.sever();
    }

    /// Position counts the board reports for ":j<axis>".
    void set_counts(int axis, uint32_t counts) {
        std::lock_guard<std::mutex> lock(mutex_);
        counts_[axis - 1] = counts & 0xFFFFFF;
    }

    /// open-astro#505: a hung MCU behind a perfectly healthy fd — the mount
    /// powered off with the USB adapter left plugged in, or the EQDIR cable
    /// pulled at the mount end. Distinct from sever_link(), which removes the
    /// node: here every frame still reaches the board and the board simply
    /// never answers, so link_alive() stays true and only the exchange
    /// timeouts can reveal it. Same knob name as the #237 fakes.
    void set_muted(bool muted) {
        std::lock_guard<std::mutex> lock(mutex_);
        muted_ = muted;
    }

    /// A muted board on an adapter whose driver does not honour VMIN/VTIME
    /// (#836): a read with no data parks instead of timing out. After each
    /// silent frame the fake rewrites the line to VMIN=1 / VTIME=0 (every
    /// slave fd shares one termios), so any blocking read on it waits for a
    /// byte. Only a poll()-bounded read on a non-blocking fd keeps its budget.
    void set_reads_ignore_vtime(bool ignore) {
        std::lock_guard<std::mutex> lock(mutex_);
        reads_ignore_vtime_ = ignore;
    }

    /// Release a reader parked by set_reads_ignore_vtime(): one CR ends the
    /// probe's read loop, so a test that saw the hang can still join.
    void release_blocked_reader() { pty_write_bounded(pty_.master_fd(), std::string("\r"), stop_); }

    /// open-astro#521: start an axis in the running state, as a board whose
    /// motion outlived the driver's link does. @p speed_mode true models a
    /// MoveAxis or tracking drive — the case the driver classifies as NOT
    /// slewing — and false a GOTO. ":K"/":L" clear it, so a driver-side
    /// stop-and-confirm loop terminates against this fake instead of spinning.
    void set_axis_running(int axis, bool running, bool speed_mode = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        running_[axis - 1] = running;
        speed_mode_[axis - 1] = speed_mode;
    }

    bool axis_running(int axis) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_[axis - 1];
    }

    /// The mode the last ":G" selected for @p axis: true = speed mode (a
    /// MoveAxis or tracking drive), false = GOTO. Review of #553: pins the
    /// bit-0 decode of the mode digit against what the driver sends.
    bool axis_speed_mode(int axis) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return speed_mode_[axis - 1];
    }

    /// open-astro#505: the board's ":F" initialization bit, per axis. A board
    /// that power-cycles mid-session comes back with this false and its
    /// position registers reset, while answering every frame normally.
    /// Confirmed on an EQM-35 Pro 2026-09-17 (":f1" read "=100").
    void set_init_done(bool init_done) {
        std::lock_guard<std::mutex> lock(mutex_);
        init_done_[0] = init_done;
        init_done_[1] = init_done;
    }

    /// Hold the reply to the next frame (any command, or only @p command if
    /// given) for @p ms before sending it, so it lands after the wrapper's
    /// timeout: the "late reply" that the next exchange must not consume.
    void delay_next_reply(int ms, char command = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        delay_ms_ = ms;
        delay_command_ = command;
    }

    /// open-astro#559: LOSE the next @p times frames of @p command on the
    /// wire -- the board neither applies them nor answers, as a frame that
    /// arrived corrupted (or not at all) over a noisy EQDIR link. The frame
    /// is still recorded, so count_frames() shows the wrapper's retransmits.
    /// Distinct from delay_next_reply(): here a ":K" that is dropped leaves
    /// the axis RUNNING, which is what turns a lost stop into pulse overshoot.
    void drop_next_frames(char command, int times = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        drop_command_ = command;
        drop_left_ = times;
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

    /// Answer the next @p times frames with a MALFORMED reply ("25278" -- no
    /// leading "=" or "!"), i.e. a reply with a byte dropped on a noisy serial
    /// link. The wrapper must settle and resend rather than fail the command
    /// outright.
    void malform_next(int times = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        malform_left_ = times;
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
        if (pty_.keepalive_fd() < 0 || tcgetattr(pty_.keepalive_fd(), &tty) != 0) return 0;
        switch (cfgetispeed(&tty)) {
            case B9600:
                return 9600;
            case B115200:
                return 115200;
            default:
                return -1;
        }
    }

    void force_blocking_reads() const {
        struct termios tty {};
        if (pty_.keepalive_fd() < 0 || tcgetattr(pty_.keepalive_fd(), &tty) != 0) return;
        tty.c_cc[VMIN] = 1;
        tty.c_cc[VTIME] = 0;
        tcsetattr(pty_.keepalive_fd(), TCSANOW, &tty);
    }

    // Returns the reply for one frame (without the trailing CR), or an empty
    // string for "stay silent".
    std::string handle(const std::string& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (answer_baud_ != 0 && line_baud() != answer_baud_) {
            return "";  // wrong rate for this board: nothing decodable arrives
        }
        if (muted_) {
            // The frame arrived (the fd is healthy and the node is there); the
            // board is simply not answering. Recorded so a test can assert the
            // driver kept talking while faulted — which is what lets the next
            // good reply clear the latch without a reconnect.
            frames_.push_back(frame);
            if (reads_ignore_vtime_) {
                force_blocking_reads();
            }
            return "";
        }
        frames_.push_back(frame);
        if (frame.size() < 2) return "!3";
        const char cmd = frame[0];
        if (drop_left_ > 0 && cmd == drop_command_) {
            --drop_left_;
            return "";  // lost on the wire: not applied, not answered (#559)
        }
        const int axis = frame[1] == '2' ? 2 : 1;
        const std::string data = frame.substr(2);
        if (mispair_left_ > 0) {
            if (--mispair_left_ == 0 && !straggler_.empty()) {
                straggler_pending_ = true;
            }
            return "=00";  // OK reply, wrong length for anything the wrapper asks
        }
        if (malform_left_ > 0) {
            --malform_left_;
            return "25278";  // no leading "=" / "!": a byte dropped on a noisy link
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
            case 'f': {
                // char0 bit0 speed-mode, char1 bit0 running, char2 bit0 init-done.
                const int c0 = speed_mode_[axis - 1] ? 1 : 0;
                const int c1 = running_[axis - 1] ? 1 : 0;
                const int c2 = init_done_[axis - 1] ? 1 : 0;
                return std::string("=") + static_cast<char>('0' + c0) + static_cast<char>('0' + c1) +
                       static_cast<char>('0' + c2);
            }
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
            case 'K':
            case 'L':
                // Stop: the axis actually comes to rest, so a driver-side
                // stop-and-confirm loop terminates against this fake instead
                // of spinning (open-astro#521).
                running_[axis - 1] = false;
                return "=";
            case 'J':
                running_[axis - 1] = true;
                return "=";
            case 'F':
                init_done_[axis - 1] = true;
                return "=";
            case 'G':
                // ":G<mode><dir>": the mode digit is a bit field. Bit 0 set
                // (1 or 3) selects SPEED mode, clear (0 or 2) selects GOTO;
                // bit 1 picks the fast/slow rate. The driver sends '3' for a
                // fast speed move and '1' for a slow one, so decode bit 0 the
                // way the board does rather than listing digits (review of
                // #553: the old "1 or 2" list recorded a fast MoveAxis as a
                // GOTO, which get_hardware_slewing_locked() reports as Slewing).
                if (!data.empty()) {
                    speed_mode_[axis - 1] = ((data[0] - '0') & 1) != 0;
                }
                return "=";
            case 'S':
            case 'E':
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
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            const int r = poll(&pfd, 1, 10);
            if (r <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
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
                pty_write_bounded(pty_.master_fd(), reply, stop_);
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
                    pty_write_bounded(pty_.master_fd(), straggler, stop_);
                }
            }
        }
    }

    // First member: constructed before the worker, destroyed after it has
    // been joined.
    PtyPair pty_;
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
    int malform_left_ = 0;   // frames to answer with a byte-dropped malformed reply
    char drop_command_ = 0;  // frames of this command to lose on the wire (#559)
    int drop_left_ = 0;
    std::string straggler_;
    int straggler_ms_ = 0;
    bool straggler_pending_ = false;
    bool no_readback_ = false;
    std::string reject_readback_code_;
    // Defaults reproduce the fixed "=101" this fake used to answer for ":f":
    // speed mode, not running, initialized.
    bool muted_ = false;
    bool reads_ignore_vtime_ = false;
    bool running_[2] = {false, false};
    bool speed_mode_[2] = {true, true};
    bool init_done_[2] = {true, true};
    std::vector<std::string> frames_;
};

}  // namespace alpacacore::test

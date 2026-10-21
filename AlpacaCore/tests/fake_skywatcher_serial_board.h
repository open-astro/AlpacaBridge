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
// hardware. FakeSkyWatcherMount serves the Wave and the other UDP profiles over
// UDP; this one serves the serial wrapper path and the EQM-35 Pro cases of the
// driver suites (the `[eqm35]` cases in test_skywatcher_async.cpp and
// test_skywatcher_pointing.cpp, open-astro#604), with the full ":" command set
// and axis model of the shared core.
//
// The axis model, the profile and the ":" command handler are the shared
// FakeSkyWatcherBoard core (fake_skywatcher_board.h, open-astro#604), the same
// one FakeSkyWatcherMount drives over UDP; this class adds the pty transport
// and the serial-only knobs. Where the two transports differed, the serial
// answer is kept by a transport setting here, or the core's answer is taken
// (open-astro/AlpacaBridge#1330 lists each one):
//   - set_muted() (frame reaches the board, is recorded, is NOT applied, no
//     reply) is not the UDP set_silent() (frame applied, no reply).
//   - frames() is the raw frame log including unapplied frames; the core's
//     frames_seen() counts frames the core handled.
//   - set_reply_latency() takes int milliseconds (UDP: chrono::milliseconds).
//   - ":M" answers "=" here (the core and UDP answer "!0").
//   - ":E" on a running axis answers "!2" (core); this fake used to answer "=".
//   - ":f" char 0 carries the direction and fast bits too (core).
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
#include <utility>
#include <vector>

#include "fake_pty_write.h"
#include "fake_skywatcher_board.h"

namespace alpacacore::test {

class FakeSkyWatcherSerialBoard : public FakeSkyWatcherBoard {
public:
    // The board answers the geometry and feature inquiries (":a", ":b", ":g",
    // ":q", ":s") and the version of @p profile, the same FakeMountProfile the
    // UDP fake takes, so a serial test of an EQM-35 Pro is not served the
    // Wave 100i's counts or its HOME_INDEXER bit.
    /// @param clock the clock the axis model integrates motion against (see
    /// fake_skywatcher_board.h); a FakeTaskClock makes motion virtual. Must
    /// outlive the board.
    explicit FakeSkyWatcherSerialBoard(FakeMountProfile profile = FakeMountProfile::wave_100i(),
                                       alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock())
        : FakeSkyWatcherBoard(std::move(profile), clock), pty_("FakeSkyWatcherSerialBoard") {
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
    /// The pty's keep-alive slave fd: lets a test read or retune the line the
    /// driver holds without opening the slave path, which TIOCEXCL refuses
    /// while the driver is connected (#1202).
    int keepalive_fd() const { return pty_.keepalive_fd(); }

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

    /// open-astro#505: a hung MCU behind a perfectly healthy fd — the mount
    /// powered off with the USB adapter left plugged in, or the EQDIR cable
    /// pulled at the mount end. Distinct from sever_link(), which removes the
    /// node: here every frame still reaches the board and the board simply
    /// never answers, so link_alive() stays true and only the exchange
    /// timeouts can reveal it. Same knob name as the #237 fakes.
    void set_muted(bool muted) {
        std::lock_guard<std::mutex> lock(smutex_);
        muted_ = muted;
    }

    /// A muted board on an adapter whose driver does not honour VMIN/VTIME
    /// (#836): a read with no data parks instead of timing out. Whenever
    /// bytes arrive (a `:` frame or the SynScan echo guard's bare "KB") the
    /// fake rewrites the line to VMIN=1 / VTIME=0 (every slave fd shares one
    /// termios), after the reader's own tcsetattr, so any blocking read on
    /// it waits for a byte. Only a poll()-bounded read keeps its budget.
    void set_reads_ignore_vtime(bool ignore) {
        std::lock_guard<std::mutex> lock(smutex_);
        reads_ignore_vtime_ = ignore;
    }

    /// Release a reader parked by set_reads_ignore_vtime(): one CR ends the
    /// probe's read loop, so a test that saw the hang can still join.
    void release_blocked_reader() { pty_write_bounded(pty_.master_fd(), std::string("\r"), stop_); }

    /// Hold the reply to the next frame (any command, or only @p command if
    /// given) for @p ms before sending it, so it lands after the wrapper's
    /// timeout: the "late reply" that the next exchange must not consume.
    void delay_next_reply(int ms, char command = 0) {
        std::lock_guard<std::mutex> lock(smutex_);
        delay_ms_ = ms;
        delay_command_ = command;
    }

    /// Steady state, until set back to zero: every reply is held for @p ms
    /// before it is sent (delay_next_reply() is the one-shot form).
    /// transactions_served() counts the replies sent, readable from a test.
    void set_reply_latency(int ms) { latency_ms_.store(ms); }
    int transactions_served() const { return served_.load(); }

    /// open-astro#559: LOSE the next @p times frames of @p command on the
    /// wire -- the board neither applies them nor answers, as a frame that
    /// arrived corrupted (or not at all) over a noisy EQDIR link. The frame
    /// is still recorded, so count_frames() shows the wrapper's retransmits.
    /// Distinct from delay_next_reply(): here a ":K" that is dropped leaves
    /// the axis RUNNING, which is what turns a lost stop into pulse overshoot.
    void drop_next_frames(char command, int times = 1) {
        std::lock_guard<std::mutex> lock(smutex_);
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
        std::lock_guard<std::mutex> lock(smutex_);
        mispair_left_ = times;
        straggler_ = std::move(straggler);
        straggler_ms_ = straggler_ms;
    }

    /// Answer the next @p times frames with a MALFORMED reply ("25278" -- no
    /// leading "=" or "!"), i.e. a reply with a byte dropped on a noisy serial
    /// link. The wrapper must settle and resend rather than fail the command
    /// outright.
    void malform_next(int times = 1) {
        std::lock_guard<std::mutex> lock(smutex_);
        malform_left_ = times;
    }

    /// Answer ":J" on an axis that is already running with "!2" (motor not
    /// stopped), as a board that refuses to start a running axis does.
    void reject_j_on_running_axis(bool reject = true) {
        std::lock_guard<std::mutex> lock(smutex_);
        reject_j_on_running_ = reject;
    }

    /// The ":e1" payload (default: the profile's, "033A44" for the Wave 100i); overrides it.
    void set_version_reply(std::string payload) {
        std::lock_guard<std::mutex> lock(smutex_);
        version_reply_ = std::move(payload);
    }

    /// Answer frames only while the wrapper has the line configured at
    /// @p baud (0 = any). A pty carries no real signalling rate, but the
    /// termios speed the wrapper sets on its fd is visible on every fd of
    /// the same slave, so this models a board that only decodes at its own
    /// rate: the 9600 probe gets silence, the 115200 one an answer.
    void answer_only_at_baud(int baud) {
        std::lock_guard<std::mutex> lock(smutex_);
        answer_baud_ = baud;
    }

    /// open-astro#912: another process reprograms the shared tty. Sets the
    /// speed on the keep-alive slave fd, which every fd of the slave sees.
    void set_line_baud(int baud) {
        struct termios tty {};
        if (pty_.keepalive_fd() < 0 || tcgetattr(pty_.keepalive_fd(), &tty) != 0) return;
        cfsetispeed(&tty, baud == 115200 ? B115200 : B9600);
        cfsetospeed(&tty, baud == 115200 ? B115200 : B9600);
        tcsetattr(pty_.keepalive_fd(), TCSANOW, &tty);
    }

    /// Refuse ":i" with "!0" (Unknown command), as a board without the
    /// step-period readback does.
    void set_no_readback(bool no_readback) {
        std::lock_guard<std::mutex> lock(smutex_);
        no_readback_ = no_readback;
    }

    /// Answer ":i" the way an EQM-35 Pro (fw 3.47) does: the stored period
    /// divided by 16 (truncated), as a signed 24-bit value negative when
    /// `negative` (the direction). `wrong_value` >= 0 answers that raw value
    /// instead, to model a readback that fits neither encoding.
    void set_readback_div16(bool negative, int64_t wrong_value = -1) {
        std::lock_guard<std::mutex> lock(smutex_);
        div16_readback_ = true;
        div16_negative_ = negative;
        div16_wrong_ = wrong_value;
    }

    /// Refuse the next ":i" with "!<code>" once (e.g. "2", Motor not stopped).
    void reject_next_readback(std::string code) {
        std::lock_guard<std::mutex> lock(smutex_);
        reject_readback_code_ = std::move(code);
    }

    /// Every frame received so far, without the ':' and the trailing CR
    /// (e.g. "j1", "I1123456").
    std::vector<std::string> frames() const {
        std::lock_guard<std::mutex> lock(smutex_);
        return frames_;
    }

    int count_frames(char command) const {
        std::lock_guard<std::mutex> lock(smutex_);
        int n = 0;
        for (const auto& f : frames_) {
            if (!f.empty() && f[0] == command) ++n;
        }
        return n;
    }

private:
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

    // The reply for one frame (without the trailing CR), or an empty string for
    // "stay silent". The transport and failure knobs decide first; every other
    // frame goes to the shared core as ":" + frame.
    std::string handle_frame(const std::string& frame) {
        bool reject_j = false;
        bool apply_j_then_garble = false;
        bool div16 = false;
        bool div16_negative = false;
        int64_t div16_wrong = -1;
        {
            std::lock_guard<std::mutex> lock(smutex_);
            if (answer_baud_ != 0 && line_baud() != answer_baud_) {
                return "";  // wrong rate for this board: nothing decodable arrives
            }
            // A muted board: the frame arrived (the fd is healthy and the node is
            // there); the board is simply not answering. Recorded so a test can
            // assert the driver kept talking while faulted, which is what lets the
            // next good reply clear the latch without a reconnect.
            frames_.push_back(frame);
            if (muted_) return "";
            if (frame.size() < 2) return "!3";
            const char cmd = frame[0];
            if (drop_left_ > 0 && cmd == drop_command_) {
                --drop_left_;
                return "";  // lost on the wire: not applied, not answered (#559)
            }
            if (mispair_left_ > 0) {
                if (--mispair_left_ == 0 && !straggler_.empty()) {
                    straggler_pending_ = true;
                }
                return "=00";  // OK reply, wrong length for anything the wrapper asks
            }
            if (malform_left_ > 0) {
                --malform_left_;
                // The board decoded and APPLIED a start before its ack was garbled.
                if (cmd != 'J') return "25278";  // no leading "=" / "!": a byte dropped on a noisy link
                apply_j_then_garble = true;
            }
            if (cmd == 'e' && !version_reply_.empty()) return "=" + version_reply_;
            if (cmd == 'M') return "=";
            if (cmd == 'i') {
                if (!reject_readback_code_.empty()) {
                    std::string code;
                    code.swap(reject_readback_code_);
                    return "!" + code;
                }
                if (no_readback_) return "!0";
                div16 = div16_readback_;
                div16_negative = div16_negative_;
                div16_wrong = div16_wrong_;
            }
            reject_j = cmd == 'J' && reject_j_on_running_;
        }
        const int axis = frame[1] == '2' ? 2 : 1;
        if (reject_j && axis_running(axis)) return "!2";
        if (div16) {
            if (div16_wrong >= 0) return "=" + u24(static_cast<uint32_t>(div16_wrong));
            const uint32_t q = step_period(axis) / 16;
            return "=" + u24(div16_negative ? (0x1000000u - q) & 0xFFFFFFu : q);
        }
        std::string reply = FakeSkyWatcherBoard::handle(":" + frame);
        return apply_j_then_garble ? "25278" : reply;
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
            if (n > 0) {
                std::lock_guard<std::mutex> lock(smutex_);
                if (muted_ && reads_ignore_vtime_) force_blocking_reads();
            }
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
                const std::string body = handle_frame(frame);
                if (body.empty()) {
                    continue;  // silent: the board did not decode the frame
                }
                const std::string reply = body + "\r";
                int delay = 0;
                {
                    std::lock_guard<std::mutex> lock(smutex_);
                    if (delay_ms_ > 0 && (delay_command_ == 0 || delay_command_ == frame[0])) {
                        delay = delay_ms_;
                        delay_ms_ = 0;
                        delay_command_ = 0;
                    }
                }
                if (delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                if (const int latency_ms = latency_ms_.load(); latency_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
                }
                pty_write_bounded(pty_.master_fd(), reply, stop_);
                served_.fetch_add(1);
                std::string straggler;
                int straggler_ms = 0;
                {
                    std::lock_guard<std::mutex> lock(smutex_);
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
    std::atomic<int> latency_ms_{0};
    std::atomic<int> served_{0};
    mutable std::mutex smutex_;  // the transport and failure knobs; the core's mutex_ guards the axes
    int delay_ms_ = 0;
    char delay_command_ = 0;
    std::string version_reply_;  // empty: the profile's
    int answer_baud_ = 0;
    int mispair_left_ = 0;
    bool reject_j_on_running_ = false;
    int malform_left_ = 0;   // frames to answer with a byte-dropped malformed reply
    char drop_command_ = 0;  // frames of this command to lose on the wire (#559)
    int drop_left_ = 0;
    std::string straggler_;
    int straggler_ms_ = 0;
    bool straggler_pending_ = false;
    bool no_readback_ = false;
    bool div16_readback_ = false;
    bool div16_negative_ = false;
    int64_t div16_wrong_ = -1;
    std::string reject_readback_code_;
    bool muted_ = false;
    bool reads_ignore_vtime_ = false;
    std::vector<std::string> frames_;
};

}  // namespace alpacacore::test

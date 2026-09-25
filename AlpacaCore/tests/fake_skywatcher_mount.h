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
// Simulated geometry matches the Wave 100i values captured in
// .github/instructions/skywatcher.instructions.md:
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
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace alpacacore::test {

// A simulated board's identity and geometry. The presets are REAL hardware
// captures, so the loopback tests exercise the same numbers the driver sees on
// the bench rather than idealised ones.
struct FakeMountProfile {
    uint32_t cpr = 4147200;
    // Dec-axis counts per revolution when the board reports a different ":a2"
    // from its ":a1"; 0 means "same as cpr", which is every board here but the
    // EQ-AL55i Pro. Kept opt-in so the existing profiles are untouched.
    uint32_t cpr_dec = 0;
    uint32_t timer_freq = 14000000;
    std::string version_reply = "033A44";  // ":e" payload: fw 3.58, mount code 0x44
    std::string high_speed_ratio_reply = "01";
    uint32_t features = 0x100C;  // ":q" 0x000001: POLAR_LED | IS_AZEQ | HOME_INDEXER
    // ":s" (steps per worm). nullopt: the board rejects ":s" with "!0".
    // A value, zero included, is answered as "=" + that value.
    std::optional<uint32_t> steps_per_worm;

    // Wave 100i, MC firmware 3.58, mount code 0x44 (.github/instructions/skywatcher.instructions.md capture).
    static FakeMountProfile wave_100i() { return FakeMountProfile{}; }

    // Sky-Watcher EQM-35 Pro, MC firmware 3.39, mount code 0x32. Captured over
    // the mount's built-in USB port (onboard PL2303 @ 115200) on 2026-09-06:
    //   :e -> =032732   :a -> 9216000   :b -> 16000000   :g -> 01
    //   :s -> 68266 (9216000/68266 = 135 worm teeth)
    //   :q 0x000001 -> 0x7000  POLAR_LED | COMMON_SLEW_START | HALF_CURRENT_TRACKING
    // No HOME_INDEXER bit (0x04) -> find_home() takes the count-frame fallback
    // instead of AutoHome; get_can_find_home() stays true unconditionally.
    static FakeMountProfile eqm35_pro() {
        FakeMountProfile p;
        p.cpr = 9216000;
        p.timer_freq = 16000000;
        p.version_reply = "032732";
        p.high_speed_ratio_reply = "01";
        p.features = 0x7000;
        p.steps_per_worm = 68266;
        return p;
    }

    // Sky-Watcher EQ-AL55i Pro, MC firmware 3.46, mount code 0x09. Read from
    // the board by its owner on 2026-09-20 (open-astro#306) over the mount's
    // own USB port, an STM32 CDC-ACM port like the Wave's, not a PL2303:
    //   :e -> =032E09   :a1 -> 4032000   :a2 -> 3600000   :b -> 16000000
    //   :g -> 01        :q 0x000001 -> 0x9000
    // The two axes report DIFFERENT counts per revolution, the only board here
    // that does.
    //   :s1 -> =000000  :s2 -> =000000   (2026-09-22, open-astro#306, read twice
    //   per axis through CommandString Raw=true, mount stationary at home)
    // That is a real zero reply, not the "!0" the Wave gives. Whether this
    // firmware leaves the register unpopulated or 0x09 does not count worm
    // steps the same way is not known from one reading.
    // ":g" is recorded as read: the reporter could not confirm that 0x01 is
    // what this firmware is expected to return. It is inert either way -- the
    // driver already reads a high-speed ratio of 0 as 1.
    // No HOME_INDEXER bit (0x04) -> find_home() takes the count-frame fallback
    // instead of AutoHome; get_can_find_home() stays true unconditionally.
    static FakeMountProfile eq_al55i() {
        FakeMountProfile p;
        p.cpr = 4032000;
        p.cpr_dec = 3600000;
        p.timer_freq = 16000000;
        p.version_reply = "032E09";
        p.high_speed_ratio_reply = "01";
        p.features = 0x9000;
        p.steps_per_worm = 0;
        return p;
    }
};

class FakeSkyWatcherMount {
public:
    static constexpr uint32_t kHome = 0x800000;
    static constexpr double kSiderealDegPerSec = 360.0 / 86164.0905;
    static constexpr double kGotoDegPerSec = 800.0 * kSiderealDegPerSec;

    const uint32_t kCpr;
    const uint32_t kCprDec;
    const uint32_t kTimerFreq;

    explicit FakeSkyWatcherMount(FakeMountProfile profile = FakeMountProfile::wave_100i())
        : kCpr(profile.cpr),
          kCprDec(profile.cpr_dec != 0 ? profile.cpr_dec : profile.cpr),
          kTimerFreq(profile.timer_freq),
          profile_(std::move(profile)) {
        axes_[0].cpr = kCpr;
        axes_[1].cpr = kCprDec;
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

    const FakeMountProfile& profile() const { return profile_; }

    bool ok() const { return fd_ >= 0; }
    int port() const { return port_; }

    /// PHYSICAL axis angle in degrees — survives ":E" count re-stamps (the
    /// frame shift is tracked), so tests can assert where the axis really is.
    double physical_degrees(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        return (static_cast<double>(a.counts + a.frame_shift) - static_cast<double>(kHome)) * 360.0 / a.cpr;
    }

    /// Signed axis angle in degrees from COUNT home (the controller's frame).
    double axis_degrees(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        return (static_cast<double>(a.counts) - static_cast<double>(kHome)) * 360.0 / a.cpr;
    }

    /// Number of ":J" start commands received for an axis (regression: no
    /// motor start may reach the controller after an AbortSlew returns).
    int start_count(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ax(axis).start_count;
    }

    /// Number of ":K"/":L" stop commands received for an axis (regression:
    /// a superseded goto dispatch must still stop BOTH axes).
    int stop_count(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        return ax(axis).stop_count;
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
            static_cast<int64_t>(kHome) - a.frame_shift + static_cast<int64_t>(std::llround(deg * a.cpr / 360.0));
    }

    /// Simulate deceleration: ":K" keeps the axis running (at its current
    /// rate) for this long before it reports stopped — the window in which
    /// the driver's stop-waits poll (issue #212 coverage).
    void set_stop_ramp_ms(int ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ramp_ms_ = ms;
    }

    /// Hold the reply to the NEXT command for @p delay (one shot), so a connect that is waiting on it stays
    /// open that long. Used by the contract sweep to make Connecting observable.
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }

    /// While @p on, answer every ":e" identity request with a reply of the
    /// right length that is not hex, so the driver's identify fails
    /// (open-astro#458 review: an unidentified board loses its measured
    /// dec-axis sense). Every one, not a count: the UDP link check sends its
    /// own ":e1" first and accepts any OK-shaped reply.
    void set_garbled_version_replies(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        garbled_version_replies_ = on;
    }

    /// Acknowledge but silently DROP the next @p n ":I" step-period writes on
    /// an axis (regression: the ":i" readback logs the mismatch but the write
    /// is NOT resent and the call does NOT throw -- the INDI/EQMod contract).
    void drop_step_period_writes(int axis, int n) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax(axis).drop_step_period_writes = n;
    }

    /// Acknowledge and STORE (":i" will read it back correctly) the next @p n
    /// ":I" writes to a RUNNING axis, but do NOT apply them to the spinning
    /// motor until a fresh ":J" arrives. Models the EQM-35 Pro ConformU
    /// failure (2026-09-06): a live step-period change during PulseGuide
    /// East whose ":i" readback matched what was written, yet the axis held
    /// its old rate for the whole pulse. Root cause on the real board is
    /// unconfirmed (~22% of live East/West pulses, never in isolation); this
    /// models the failure shape closely enough to prove the ":J" re-kick works.
    void stall_live_rate_writes(int axis, int n) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax(axis).stall_live_rate_writes = n;
    }

    /// Swallow the re-latch of the next @p n ":J" on a RUNNING axis: the kick
    /// is acknowledged but the stored preset still is not applied. Combined
    /// with stall_live_rate_writes this is a stall that survives the driver's
    /// unconditional ":I"+":J" and can only be recovered by the sampled
    /// rate-applied check re-kicking (verify_live_rate_or_rekick).
    void ignore_start_relatches(int axis, int n) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax(axis).ignore_start_relatches = n;
    }

    /// Latch the next @p n ":J" starts that arrive on a STOPPED, non-goto axis
    /// -- the tracking restart after a slew landing -- at @p factor times the
    /// commanded rate. Models the #432 symptom directly: the board accepts the
    /// restart, ":i" reads back exactly what the driver wrote, and the axis
    /// nonetheless runs at the wrong rate, so only a sampled position check
    /// can see it.
    ///
    /// A dropped ":I" cannot produce this: dispatch_goto_locked() writes only
    /// ":G"/":S"/":J", never ":I", so the axis still holds the tracking period
    /// from before the slew and the restart's ":J" re-latches it CORRECTLY.
    void restart_tracking_at_wrong_rate(int axis, int n, double factor = 2.0) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax(axis).wrong_restart_latches = n;
        ax(axis).wrong_restart_factor = factor;
    }

    /// Report the next @p n goto landings as STOPPED while the axis is still
    /// @p deg short of its goto target, then creep that remainder in over
    /// @p coast_ms. Models the real landing the #432 session measured: ":f"
    /// clears its running bit when the controller ends its braking ramp, but
    /// the last counts keep arriving, so a position read taken the instant
    /// the slew "completes" is short of where the axis actually settles.
    ///
    /// This is the seam wait_axis_stationary_locked() exists for, and the one
    /// stop_ramp_ms_ cannot provide: a ramped ":K" keeps ":f" RUNNING for the
    /// whole ramp, so the driver's ordinary stop-wait already covers it and
    /// the stationary check has no window left to close.
    void land_short_by(int axis, int n, double deg, int coast_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.short_landings = n;
        a.short_counts = static_cast<int64_t>(std::llround(deg * a.cpr / 360.0));
        a.coast_ms = coast_ms;
    }

    /// Refuse the next @p n ":J" on an axis with "!2" (Motor not stopped):
    /// the start/re-latch throws in the wrapper. Models a transport-level
    /// failure of the ":J" kick that follows a live ":I" (#249 review).
    void reject_start_motion(int axis, int n) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax(axis).reject_starts = n;
    }

    /// The T1 step period the axis is actually running with (last APPLIED ":I").
    uint32_t step_period(int axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(ax(axis).t1);
    }

    /// Move the simulated axes instantly (test setup).
    void jump_axis_degrees(int axis, double deg) {
        std::lock_guard<std::mutex> lock(mutex_);
        Axis& a = ax(axis);
        a.advance(now());
        a.counts = static_cast<int64_t>(kHome) + static_cast<int64_t>(std::llround(deg * a.cpr / 360.0));
    }

private:
    struct Axis {
        // Copy of the profile CPR: the goto ramp below needs it, and a nested
        // type cannot reach the enclosing object's non-static members.
        uint32_t cpr = 4147200;
        int64_t counts = kHome;
        double rate_counts = 0.0;  // signed counts/sec while running
        double count_frac = 0.0;   // sub-count remainder carried between advance() calls
        bool running = false;
        bool speed_mode = true;
        bool fast = false;
        char dir = '0';
        uint32_t t1 = 0;
        int64_t goto_target = kHome;
        int64_t frame_shift = 0;                          // physical = counts + frame_shift
        std::chrono::steady_clock::time_point stop_at{};  // ramped ":K" deadline
        bool stopping = false;
        bool in_goto = false;
        bool init_done = true;
        // Home indexer: 0 = armed below the index, 0xFFFFFF = armed above,
        // else the latched count of the crossing.
        uint32_t indexer = 0;
        int start_count = 0;
        int stop_count = 0;
        int short_landings = 0;    // goto landings to report stopped early (test knob)
        int64_t short_counts = 0;  // how far short of the target to report it (test knob)
        int coast_ms = 0;          // how long the remainder takes to arrive (test knob)
        bool coasting = false;     // counts still creeping with ":f" reading stopped
        int64_t coast_target = 0;
        double coast_cps = 0.0;
        int drop_step_period_writes = 0;  // ":I" writes to ack-but-ignore (test knob)
        int wrong_restart_latches = 0;    // ":J" restarts to latch at the wrong rate (test knob)

        double wrong_restart_factor = 2.0;  // how wrong (test knob)
        int stall_live_rate_writes = 0;     // ":I" writes on a running axis to store but not apply (test knob)
        int ignore_start_relatches = 0;     // ":J" kicks on a running axis that must NOT re-latch T1 (test knob)
        int reject_starts = 0;              // ":J" to refuse with "!2" (test knob)
        int64_t home_index_counts = kHome;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

        void advance(std::chrono::steady_clock::time_point t) {
            double dt = std::chrono::duration<double>(t - last).count();
            last = t;
            if (stopping && t >= stop_at) {
                running = false;
                stopping = false;
                in_goto = false;
            }
            if (dt <= 0.0) {
                return;
            }
            if (!running) {
                // A landing reported stopped early: the last counts are still
                // arriving even though ":f" says the axis is idle.
                if (coasting) {
                    int64_t before_coast = counts;
                    double dir_sign = coast_target >= counts ? 1.0 : -1.0;
                    double step = coast_cps * dt;
                    double remaining = std::abs(static_cast<double>(coast_target - counts));
                    if (step >= remaining) {
                        counts = coast_target;
                        coasting = false;
                    } else {
                        counts += static_cast<int64_t>(std::llround(dir_sign * step));
                    }
                    latch_indexer(before_coast);
                }
                return;
            }
            int64_t before = counts;
            if (in_goto) {
                double dir_sign = goto_target >= counts ? 1.0 : -1.0;
                double step = kGotoDegPerSec * cpr / 360.0 * dt;
                double remaining = std::abs(static_cast<double>(goto_target - counts));
                if (step >= remaining) {
                    running = false;
                    in_goto = false;
                    if (short_landings > 0 && short_counts > 0 && coast_ms > 0) {
                        --short_landings;
                        // Stop short and creep the remainder in while ":f"
                        // already reads stopped.
                        counts = goto_target - static_cast<int64_t>(std::llround(dir_sign * short_counts));
                        coasting = true;
                        coast_target = goto_target;
                        coast_cps = static_cast<double>(short_counts) / (static_cast<double>(coast_ms) / 1000.0);
                    } else {
                        counts = goto_target;
                    }
                } else {
                    counts += static_cast<int64_t>(std::llround(dir_sign * step));
                }
            } else {
                // Carry the sub-count remainder across calls. Rounding each
                // increment on its own and still advancing `last` by the whole
                // dt threw the fraction away every time advance() ran, so the
                // modelled rate depended on how often a test polled: at a guide
                // rate of ~24 counts/s a 50 ms poll adds llround(1.2) = 1, and a
                // pulse delivered ~79% of its counts (open-astro#306). Slew rates
                // were unaffected (~19,000 counts/s), which is why only the guide
                // and tracking regime showed it.
                const double exact = rate_counts * dt + count_frac;
                const double whole = std::trunc(exact);
                count_frac = exact - whole;
                counts += static_cast<int64_t>(whole);
            }
            latch_indexer(before);
        }

        // Latch the home index on a crossing while armed.
        void latch_indexer(int64_t before) {
            if (indexer != 0 && indexer != 0xFFFFFF) {
                return;
            }
            bool was_below = before < home_index_counts;
            bool is_below = counts < home_index_counts;
            if (was_below != is_below) {
                indexer = static_cast<uint32_t>(home_index_counts & 0xFFFFFF);
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
        bool was_running_on_start = false;  // ":J" only; declared here to not cross case labels
        switch (cmd) {
            case 'e':
                if (garbled_version_replies_) {
                    return "=ZZZZZZ";  // right length (no mis-pair resend), not hex
                }
                return "=" + profile_.version_reply;
            case 'a':
                return "=" + u24(a.cpr);
            case 'b':
                return "=" + u24(kTimerFreq);
            case 'g':
                return "=" + profile_.high_speed_ratio_reply;
            case 's':
                if (!profile_.steps_per_worm) return "!0";
                return "=" + u24(*profile_.steps_per_worm);
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
                if (a.drop_step_period_writes > 0) {
                    --a.drop_step_period_writes;
                    return "=";  // acked, not stored -- ":i" will disagree
                }
                a.t1 = parse_u24(data);
                if (a.running && a.stall_live_rate_writes > 0) {
                    --a.stall_live_rate_writes;
                    // Stored (":i" readback matches) but NOT applied to the
                    // spinning motor -- only a fresh ":J" latches it.
                    return "=";
                }
                double cps = a.t1 > 0 ? static_cast<double>(kTimerFreq) / a.t1 : 0.0;
                a.rate_counts = a.dir == '1' ? -cps : cps;
                return "=";
            }
            case 'i':  // inquire the T1 step period last written with ":I"
                return "=" + u24(static_cast<uint32_t>(a.t1 & 0xFFFFFF));
            case 'J':
                if (a.reject_starts > 0) {
                    --a.reject_starts;
                    return "!2";  // refused: nothing applied
                }
                ++a.start_count;
                was_running_on_start = a.running;
                if (!was_running_on_start) {
                    // Only a genuinely fresh start (from stopped) has no
                    // remainder to carry. An in-place re-kick on an already-
                    // running axis -- the RA pulse path re-sends ":I"/":J" at
                    // dispatch AND at restore -- must keep the fraction, or it
                    // silently discards up to one count each time (open-astro#603
                    // review, mirroring the same bug this branch fixed for the
                    // cadence-independent Dec/speed-mode path).
                    a.count_frac = 0.0;
                }
                a.coasting = false;  // a fresh command supersedes any coast
                a.running = true;
                a.stopping = false;
                if (a.in_goto) {
                    a.goto_target &= 0xFFFFFF;
                } else if (a.ignore_start_relatches > 0) {
                    --a.ignore_start_relatches;  // acked, preset still not applied
                } else {
                    // Re-latch the current T1 preset into the running rate --
                    // this is what makes a ":J" kick after a stalled live
                    // ":I" actually take effect.
                    double cps = a.t1 > 0 ? static_cast<double>(kTimerFreq) / a.t1 : 0.0;
                    if (!was_running_on_start && a.wrong_restart_latches > 0) {
                        --a.wrong_restart_latches;
                        cps *= a.wrong_restart_factor;  // latched wrong: #432
                    }
                    a.rate_counts = a.dir == '1' ? -cps : cps;
                }
                return "=";
            case 'K':  // ramped stop: keeps running for stop_ramp_ms_ first
                ++a.stop_count;
                a.coasting = false;  // an explicit stop ends the landing coast
                if (a.running && stop_ramp_ms_ > 0) {
                    a.stopping = true;
                    a.stop_at = now() + std::chrono::milliseconds(stop_ramp_ms_);
                } else {
                    a.running = false;
                    a.in_goto = false;
                }
                return "=";
            case 'L':  // instant stop
                ++a.stop_count;
                a.coasting = false;  // an explicit stop ends the landing coast
                a.running = false;
                a.stopping = false;
                a.in_goto = false;
                return "=";
            case 'q': {
                uint32_t inquiry = parse_u24(data);
                if (inquiry == 0x000001) return "=" + u24(profile_.features);
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
            if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            ::sendto(fd_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&peer), plen);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    FakeMountProfile profile_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::atomic<int> hold_ms_{0};
    std::mutex mutex_;
    int stop_ramp_ms_ = 0;
    bool garbled_version_replies_ = false;  // ":e" replies made unparseable (test knob)
    Axis axes_[2];
};

}  // namespace alpacacore::test

#endif  // _WIN32

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

// Stateful ZWO AM-mount fake for the telescope driver tests (decision 0005,
// decision 0015). It answers the ZWO ":"-command dialect over the loopback
// FakeMountServer, because the driver has no code-level transport seam, and
// derives the mount's motion from the shared FakeTaskClock: a GOTO ends when
// virtual time reaches slew_end, so a test advances the clock instead of
// sleeping. Replies are the ones the driver's wrapper parses
// (zwo_mount_protocol_wrapper.cpp), taken from the wrapper, not from memory.
// A command the wrapper sends blind (send_command_blind: ":Q", ":TQ"/":TL"/":TS",
// ":Rv", ":Rg", ":Mg", jogs, ":hP", ":hC") gets no reply, as on the wire; a
// stray reply would be read as the next exchange's answer. A command the fake
// does not know gets no reply either, so a test that needs one times out
// visibly instead of reading a guessed value.
//
// Real-time knobs (hold_reply, set_reply_delay, delay_next) model transport latency,
// not a driver timer; they never read the TaskClock.

#ifndef _WIN32

#include <alpacacore/util/task_clock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_mount_server.h"

namespace alpacacore::test {

class FakeZwoMount {
public:
    using virtual_duration = std::chrono::nanoseconds;

    /// @param clock the TaskClock the driver under test also reads (a FakeTaskClock in every test)
    /// @param identity the ":GVP#" reply, an AM-family model
    explicit FakeZwoMount(util::TaskClock& clock, std::string identity = "AM5N")
        : clock_(clock),
          identity_(std::move(identity)),
          server_([this](const std::string& chunk) { return receive(chunk); }) {}

    ~FakeZwoMount() { release(); }

    FakeZwoMount(const FakeZwoMount&) = delete;
    FakeZwoMount& operator=(const FakeZwoMount&) = delete;

    bool ok() const { return server_.ok(); }
    int port() const { return server_.port(); }

    // ---- Knobs ----------------------------------------------------------

    /// Virtual time a ":MS#" GOTO, a ":hP#" park and a ":hC#" home take. Default 5 s.
    void set_slew_duration(virtual_duration duration) {
        std::lock_guard<std::mutex> lock(mutex_);
        slew_duration_ = duration;
    }

    /// The GOTO lands short of Dec by @p arcsec (RA exact).
    void set_land_short_by(double arcsec) {
        std::lock_guard<std::mutex> lock(mutex_);
        land_short_by_arcsec_ = arcsec;
    }

    /// Answer the next @p times ":MS#" with @p reply ("2", "e3", "e5", "e7"), then accept ("0"). A negative count
    /// rejects every GOTO.
    void reject_goto(std::string reply, int times = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        goto_reply_ = std::move(reply);
        goto_rejections_ = times;
    }

    /// Answer the next @p times commands that start with @p prefix with @p reply (a negative count: every one),
    /// whatever the mount's state, e.g. "e4" to ":SMMC" for a mount still settling. One rule per prefix: a second
    /// call for the same prefix replaces it, and a count of 0 removes it. Rules for different prefixes stack.
    void override_reply(std::string prefix, std::string reply, int times = -1) {
        set_rule(std::move(prefix), std::move(reply), times, false);
    }

    /// The next @p times commands that start with @p prefix are received and counted but get no reply, as when the
    /// mount stops answering (a negative count: every one; 0 removes the rule). Shares the rule slot of
    /// override_reply() for the same prefix.
    void go_silent(std::string prefix, int times = -1) { set_rule(std::move(prefix), {}, times, true); }

    /// The reply to the next command that starts with @p prefix (and every later one until release()) blocks in
    /// the mount, so a test holds a transaction in flight. Transport latency in real time.
    void hold_reply(std::string prefix) {
        std::lock_guard<std::mutex> lock(hold_mutex_);
        hold_prefix_ = std::move(prefix);
        held_ = false;
        released_ = false;
    }
    /// True once a held reply is waiting (the command is already counted).
    bool wait_until_held(std::chrono::milliseconds real_timeout) {
        std::unique_lock<std::mutex> lock(hold_mutex_);
        return hold_cv_.wait_for(lock, real_timeout, [&] { return held_; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(hold_mutex_);
            released_ = true;
            hold_prefix_.clear();
        }
        hold_cv_.notify_all();
    }

    /// Every command waits this long (real time) before it is answered.
    void set_reply_delay(std::chrono::milliseconds delay) { reply_delay_ms_.store(static_cast<int>(delay.count())); }

    /// The mount keeps reporting "slewing" past slew_end until cleared (a GOTO that never settles).
    void hold_running(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_running_ = on;
    }

    /// ":GU#" flags with no motion behind them: 'L' low power, 'S' / 's' RA / Dec stall, 'Z' instead of 'G'.
    void set_low_power(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        low_power_ = on;
    }
    void set_stall(bool ra, bool dec) {
        std::lock_guard<std::mutex> lock(mutex_);
        ra_stall_ = ra;
        dec_stall_ = dec;
    }
    void set_alt_az(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        alt_az_ = on;
    }
    /// The mount moves with no GOTO, jog or park from the driver (a slew from the hand controller): ":GU" lacks 'N'
    /// until cleared.
    void set_moving(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        moving_ = on;
    }

    /// The next command that starts with @p prefix waits @p delay in real time before it is answered (one shot).
    void delay_next(std::string prefix, std::chrono::milliseconds delay) {
        std::lock_guard<std::mutex> lock(delay_mutex_);
        delay_prefix_ = std::move(prefix);
        delay_ms_ = static_cast<int>(delay.count());
        delay_done_ = false;
    }
    /// True once the reply held by delay_next() has been written to the socket (not merely computed), so a test
    /// can order a later exchange behind a late reply.
    bool wait_for_delayed_reply(std::chrono::milliseconds real_timeout) {
        const auto deadline = std::chrono::steady_clock::now() + real_timeout;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(delay_mutex_);
                if (delay_done_ && server_.replies_sent() >= replies_owed_.load()) return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
    }

    /// RST every accepted connection; the listener stays up, so a reconnect is served.
    bool drop_link() { return server_.drop_connections(); }
    void reset_connections() { server_.reset_connections(); }

    void set_position(double ra_hours, double dec_degrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        settle_locked();
        ra_hours_ = ra_hours;
        dec_degrees_ = dec_degrees;
        at_home_ = false;
    }
    void set_tracking(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracking_ = on;
    }
    /// ":Gps#" digit: 0 not parked, 1 in progress, 2 parked, 3 error.
    void set_park_state(int state) {
        std::lock_guard<std::mutex> lock(mutex_);
        park_state_ = state;
        park_pending_ = false;
    }
    /// The ":GMTI#" payload, "MM/DD/YYYY&HH:MM:SS&+HH:MM"; a ":SMTI" write replaces it.
    void set_time_reply(std::string reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        time_reply_ = std::move(reply);
    }

    // ---- State ----------------------------------------------------------

    bool slewing() {
        std::lock_guard<std::mutex> lock(mutex_);
        return slewing_locked();
    }
    double ra_hours() {
        std::lock_guard<std::mutex> lock(mutex_);
        settle_locked();
        return ra_hours_;
    }
    double dec_degrees() {
        std::lock_guard<std::mutex> lock(mutex_);
        settle_locked();
        return dec_degrees_;
    }
    bool tracking() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracking_;
    }
    int park_state() {
        std::lock_guard<std::mutex> lock(mutex_);
        settle_locked();
        return park_state_;
    }
    bool jogging() {
        std::lock_guard<std::mutex> lock(mutex_);
        return jog_[0] || jog_[1] || jog_[2] || jog_[3];
    }
    /// At the home position: a ":hC" slew has landed and nothing has moved the mount since.
    bool at_home() {
        std::lock_guard<std::mutex> lock(mutex_);
        settle_locked();
        return at_home_;
    }
    /// ":TQ" 0 sidereal, ":TL" 1 lunar, ":TS" 2 solar; ":GT#" reports it.
    int tracking_rate() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracking_rate_;
    }
    /// The last ":Rv" move rate, a sidereal multiple (the wire sends "NNNN.NN", not an index).
    double move_rate() {
        std::lock_guard<std::mutex> lock(mutex_);
        return move_rate_;
    }
    /// The last ":Rg" payload; ":Ggr#" answers it.
    std::string guide_rate() {
        std::lock_guard<std::mutex> lock(mutex_);
        return guide_rate_;
    }

    // ---- Command record ---------------------------------------------------

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(record_mutex_);
        return commands_;
    }
    /// Commands received that start with @p prefix (the trailing '#' is not part of the match).
    int count(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(record_mutex_);
        return static_cast<int>(std::count_if(commands_.begin(), commands_.end(), [&](const std::string& command) {
            return command.rfind(prefix, 0) == 0;
        }));
    }
    bool wait_for_count(const std::string& prefix, int n, std::chrono::milliseconds real_timeout) {
        const auto deadline = std::chrono::steady_clock::now() + real_timeout;
        while (count(prefix) < n) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

private:
    // Slewing only while a GOTO / park is within its virtual window (or held running), a jog is on or the mount is
    // set moving.
    bool slewing_locked() {
        settle_locked();
        return moving_ || slew_active_ || jog_[0] || jog_[1] || jog_[2] || jog_[3];
    }

    // Lands a finished GOTO / park / home once virtual time reaches slew_end.
    void settle_locked() {
        if (!slew_active_ || hold_running_ || clock_.now() < slew_end_) return;
        slew_active_ = false;
        ra_hours_ = target_ra_;
        dec_degrees_ = std::clamp(target_dec_ - land_short_by_arcsec_ / 3600.0, -90.0, 90.0);
        if (park_pending_) {
            park_pending_ = false;
            park_state_ = 2;
        }
        if (home_pending_) {
            home_pending_ = false;
            at_home_ = true;
            homed_ = true;
        }
    }

    // Any new motion leaves home and ends a pending park or home.
    void start_slew_locked() {
        settle_locked();
        slew_active_ = true;
        slew_end_ = clock_.now() + slew_duration_;
        at_home_ = false;
        park_pending_ = false;
        home_pending_ = false;
    }

    // ":Mg" + direction (n, s, e, w) + four-digit milliseconds: n/s guide Dec ('t'), e/w guide RA ('T').
    void start_guide_locked(const std::string& command) {
        if (command.size() < 5) return;
        const auto end = clock_.now() + std::chrono::milliseconds(std::atoi(command.c_str() + 4));
        if (command[3] == 'e' || command[3] == 'w') {
            ra_guide_end_ = end;
        } else {
            dec_guide_end_ = end;
        }
    }

    void maybe_hold(const std::string& command) {
        std::unique_lock<std::mutex> lock(hold_mutex_);
        if (hold_prefix_.empty() || released_ || command.rfind(hold_prefix_, 0) != 0) return;
        held_ = true;
        hold_cv_.notify_all();
        hold_cv_.wait(lock, [&] { return released_; });
    }

    static std::string hms(double hours) {
        const int total = static_cast<int>(std::lround(std::fmod(hours + 24.0, 24.0) * 3600.0)) % 86400;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
        return buf;
    }
    static std::string dms(double degrees) {
        const int total = static_cast<int>(std::lround(std::abs(degrees) * 3600.0));
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%c%02d*%02d:%02d", degrees < 0.0 ? '-' : '+', total / 3600,
                      (total % 3600) / 60, total % 60);
        return buf;
    }
    // "HH:MM:SS" or "+DD*MM:SS" / "+DD:MM:SS": sign, then up to three numbers split on any non-digit.
    static double sexagesimal(const std::string& text) {
        std::size_t pos = 0;
        const bool negative = !text.empty() && text[0] == '-';
        if (!text.empty() && (text[0] == '+' || text[0] == '-')) ++pos;
        double value = 0.0, scale = 1.0;
        while (pos < text.size() && scale >= 1.0 / 3600.0 * 0.5) {
            std::size_t used = 0;
            const int part = std::atoi(text.c_str() + pos);
            while (pos + used < text.size() && std::isdigit(static_cast<unsigned char>(text[pos + used]))) ++used;
            value += part * scale;
            scale /= 60.0;
            pos += used + 1;
        }
        return negative ? -value : value;
    }

    std::string receive(const std::string& chunk) {
        std::string response;
        std::string owed;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            pending_ += chunk;
            for (auto end = pending_.find('#'); end != std::string::npos; end = pending_.find('#')) {
                owed = pending_.substr(0, end + 1);
                pending_.erase(0, end + 1);
                const std::string command = owed.substr(0, owed.size() - 1);
                {
                    std::lock_guard<std::mutex> record(record_mutex_);
                    commands_.push_back(command);
                }
                maybe_hold(command);
                if (const int delay = reply_delay_ms_.load(); delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                delay_once(command);
                response += answer(command);
            }
        }
        if (!response.empty()) replies_owed_.fetch_add(1);  // one send per chunk, in the server
        finish_delay();
        return response;
    }

    // delay_next(): sleep once, in real time, for the first command with the armed prefix.
    void delay_once(const std::string& command) {
        int delay = 0;
        {
            std::lock_guard<std::mutex> lock(delay_mutex_);
            if (delay_ms_ <= 0 || delay_prefix_.empty() || command.rfind(delay_prefix_, 0) != 0) return;
            delay = delay_ms_;
            delay_ms_ = 0;
            delayed_command_ = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    void finish_delay() {
        std::lock_guard<std::mutex> lock(delay_mutex_);
        if (delayed_command_) {
            delayed_command_ = false;
            delay_done_ = true;
        }
    }

    void set_rule(std::string prefix, std::string reply, int times, bool silent) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(rules_, [&](const Rule& rule) { return rule.prefix == prefix; });
        if (times != 0) rules_.push_back({std::move(prefix), std::move(reply), times, silent});
    }

    // One reply per command the wrapper reads a reply to; blind and unknown commands get none (header comment).
    std::string answer(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto starts = [&](const char* prefix) { return command.rfind(prefix, 0) == 0; };
        for (auto rule = rules_.begin(); rule != rules_.end(); ++rule) {
            if (!starts(rule->prefix.c_str())) continue;
            const bool silent = rule->silent;
            std::string reply = rule->reply + "#";
            if (rule->times > 0 && --rule->times == 0) rules_.erase(rule);
            return silent ? std::string{} : reply;
        }
        if (command == ":GVP") return identity_ + "#";
        if (command == ":GMA") return "012345ABCDEF#";
        if (command == ":GU") {  // the wrapper's table: n N L H S s T t, then G or Z
            const bool moving = slewing_locked();
            const auto now = clock_.now();
            std::string flags;
            if (!tracking_) flags += 'n';
            if (!moving) flags += 'N';
            if (low_power_) flags += 'L';
            if (at_home_ && !moving) flags += 'H';
            if (ra_stall_) flags += 'S';
            if (dec_stall_) flags += 's';
            if (now < ra_guide_end_) flags += 'T';
            if (now < dec_guide_end_) flags += 't';
            return flags + (alt_az_ ? "Z#" : "G#");
        }
        if (command == ":GR") {
            settle_locked();
            return hms(ra_hours_) + "#";
        }
        if (command == ":GD") {
            settle_locked();
            return dms(dec_degrees_) + "#";
        }
        if (command == ":Gr") return hms(target_ra_) + "#";
        if (command == ":Gd") return dms(target_dec_) + "#";
        if (command == ":GAT") return tracking_ ? "1#" : "0#";
        if (command == ":GT") return std::to_string(tracking_rate_) + "#";
        if (command == ":Ggr") return guide_rate_ + "#";
        if (command == ":GS") return "00:00:00#";
        if (command == ":Gm") return "W#";
        if (command == ":Gh") return homed_ ? "1#" : "0#";
        if (command == ":GA") return "+45*00:00#";
        if (command == ":GZ") return "180*00:00#";
        if (command == ":GMGE") return site_reply_ + "#";
        if (command == ":GMTI") return time_reply_ + "#";
        if (command == ":Gps") {
            settle_locked();
            return std::to_string(park_state_) + "#";
        }
        if (starts(":Sr") && command.size() > 3) {
            target_ra_ = sexagesimal(command.substr(3));
            return "1#";
        }
        if (starts(":Sd") && command.size() > 3) {
            target_dec_ = sexagesimal(command.substr(3));
            return "1#";
        }
        if (starts(":SMGE")) {
            site_reply_ = command.substr(5);
            return "1#";
        }
        if (starts(":SMTI")) {
            time_reply_ = command.substr(5);
            return "1#";
        }
        if (command == ":MS") {
            if (goto_rejections_ != 0 && !goto_reply_.empty()) {
                if (goto_rejections_ > 0) --goto_rejections_;
                return goto_reply_ + "#";
            }
            park_state_ = park_state_ == 2 ? park_state_ : 0;
            start_slew_locked();
            return "0#";
        }
        if (command == ":Q") {
            // TODO: no rig evidence confirms that ":Q" stops jogs; the fake clears them so an abort leaves nothing
            // moving. Confirm on an AM mount before a test relies on it.
            for (bool& jog : jog_) jog = false;
            settle_locked();
            if (slew_active_) {  // frozen where it is: the start position, as the fake reports it while moving
                slew_active_ = false;
                park_pending_ = false;
            }
            return {};
        }
        if (command == ":Qe" || command == ":Qw" || command == ":Qn" || command == ":Qs") {
            jog_[jog_index(command[2])] = false;
            return {};
        }
        if (command == ":Me" || command == ":Mw" || command == ":Mn" || command == ":Ms") {
            jog_[jog_index(command[2])] = true;
            at_home_ = false;
            return {};
        }
        if (starts(":Mg")) {
            start_guide_locked(command);
            return {};
        }
        if (command == ":TQ" || command == ":TL" || command == ":TS") {
            tracking_rate_ = command == ":TQ" ? 0 : command == ":TL" ? 1 : 2;
            return {};
        }
        if (starts(":Rv")) {
            move_rate_ = std::atof(command.c_str() + 3);
            return {};
        }
        if (starts(":Rg")) {
            guide_rate_ = command.substr(3);
            return {};
        }
        if (command == ":Te") {
            tracking_ = true;
            return "1#";
        }
        if (command == ":Td") {
            tracking_ = false;
            return "1#";
        }
        if (command == ":hP") {
            target_ra_ = ra_hours_;
            target_dec_ = dec_degrees_;
            start_slew_locked();
            park_state_ = 1;
            park_pending_ = true;
            return {};
        }
        if (command == ":hC") {
            target_ra_ = 0.0;
            target_dec_ = 0.0;
            start_slew_locked();
            home_pending_ = true;
            return {};
        }
        if (command == ":Spu") {
            park_state_ = 0;
            return "1#";
        }
        if (starts(":SMMC")) return "N/A#";  // the wrapper accepts an empty or N/A answer to a sync
        if (command == ":Sp01" || command == ":CM") return "1#";
        return {};
    }

    static std::size_t jog_index(char axis) {
        switch (axis) {
            case 'e':
                return 0;
            case 'w':
                return 1;
            case 'n':
                return 2;
            default:
                return 3;
        }
    }

    util::TaskClock& clock_;
    const std::string identity_;

    std::mutex mutex_;  // mount state (leaf: nothing is called while it is held but the clock's now())
    virtual_duration slew_duration_ = std::chrono::seconds(5);
    double land_short_by_arcsec_ = 0.0;
    std::string goto_reply_;
    int goto_rejections_ = 0;
    struct Rule {
        std::string prefix, reply;
        int times;
        bool silent;
    };
    std::vector<Rule> rules_;
    bool moving_ = false;
    bool hold_running_ = false;
    bool slew_active_ = false;
    util::TaskClock::clock::time_point slew_end_{};
    double ra_hours_ = 12.0, dec_degrees_ = 45.0;
    double target_ra_ = 12.0, target_dec_ = 45.0;
    bool tracking_ = true;
    int tracking_rate_ = 0;
    double move_rate_ = 0.0;
    std::string guide_rate_ = "0.50";
    int park_state_ = 0;
    bool park_pending_ = false;
    bool home_pending_ = false;
    bool at_home_ = false;
    bool homed_ = false;  // ":Gh#": a home search has completed since power-on
    bool low_power_ = false, ra_stall_ = false, dec_stall_ = false, alt_az_ = false;
    util::TaskClock::clock::time_point ra_guide_end_{}, dec_guide_end_{};
    bool jog_[4] = {false, false, false, false};
    std::string site_reply_ = "-33*51:24&-151*12:55";  // ":GMGE" lat&lon, west-positive: -33.856784, 151.215297 E
    std::string time_reply_ = "01/02/2026&03:04:05&+00:00";

    std::mutex frame_mutex_;  // one connection's partial command
    std::string pending_;

    mutable std::mutex record_mutex_;
    std::vector<std::string> commands_;

    std::mutex hold_mutex_;
    std::condition_variable hold_cv_;
    std::string hold_prefix_;
    bool held_ = false;
    bool released_ = true;
    std::atomic<int> reply_delay_ms_{0};

    std::mutex delay_mutex_;
    std::string delay_prefix_;
    int delay_ms_ = 0;
    bool delayed_command_ = false;  // a delay_next() command is being answered
    bool delay_done_ = false;
    std::atomic<std::uint64_t> replies_owed_{0};  // non-empty replies handed to the server

    FakeMountServer server_;  // last: its threads call receive() until this is destroyed first
};

}  // namespace alpacacore::test

#endif  // !_WIN32

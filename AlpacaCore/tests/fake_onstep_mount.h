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

// An OnStepX German mount for the fake ConformU row (#2692, slice 6 of #2078): the mount commands of
// onstep_telescope_driver.cpp answered from one FakeMountMotionModel, so a GOTO moves the reported position over its
// travel time, a Sync re-labels it, tracking follows the sky (off, RA drifts east), a guide pulse and a MoveAxis move
// their axis, and Park and FindHome reach their positions. The focuser, rotator and auxiliary-feature commands of the
// same firmware stay in onstep_controller_model.h (OnStepXController), which the other OnStep rows serve; this fake
// answers the unknown-command "0#" to them, so the telescope row hosts the mount alone.
//
// The wire follows .github/instructions/onstep.instructions.md ("Commands this driver sends", the :GU# table):
//   - a numeric reply (:S.. setters, :MS#, :Te#, :ST.., :hP#, :hR#, :hQ#, :SXT..) is one bare digit with no '#';
//   - a command the firmware does not answer (:Q#, :Qn#.., :Mn#.., :RA.#, :RE.#, :RS#, :Mg..#, :TQ#.., :hC#) gets no
//     reply at all, as a stray reply would be read as the next exchange's answer;
//   - a refused command keeps its code for :GE# (the :MS# digits are the codes 15 to 23 less 14).
//
// What the firmware does that this fake keeps (the fake stays the HARSHER of the two, decision 0015):
//   - :MS# is refused with the firmware's digit: 1 below the horizon limit (0 degrees here), 4 parked, 5 a GOTO already
//     running (which it also stops), 8 an axis already in motion;
//   - on a parked mount :Te# and :ST<rate># (tracking on), :hP# and :hQ# are refused with code 9 (already parked),
//     and :CM# ("E4#") and :hC# with code 18 (mount parked, no home starts); :ST0# (tracking off) is accepted, as
//     ASCOM requires;
//   - :hP# and :hC# while moving, :hQ# during a GOTO or a guide move (code 22), and :hR# on an unparked mount (code
//     11) are refused;
//   - a MoveAxis (:Mn#, :Ms#, :Me#, :Mw#) ends on its own kGuideTimeLimit after its last command, so a driver that
//     does not re-send it loses it (GUIDE_TIME_LIMIT, OnStepX Guide.cpp), and a move or a pulse is ignored while the
//     mount is parked or slewing.
//
// KNOWN PARITY GAPS (what the firmware does and this fake does not):
//   - A stopped axis does not brake: the firmware keeps `g` set for about 0.4 s after a stop.
//   - Pier flips are off: :GX96# and :GX94# give the unknown-command "0#", so CanSetPierSide is false and the pier
//     side reported in :GU# is the hour-angle rule (T east, W west, o at home or parked).
//   - Only a refused command sets a :GE# code; the limit-driven general error digit in :GU# stays 0.
//   - A GOTO leaves the tracking state as it was (the firmware's behaviour at its end is not on rig evidence here).
//
// Every command is logged. The model owns time: a ConformU row runs it on the real clock, a unit case injects a clock
// it advances by hand. The responder is called from the FakeMountServer's handler threads; the mount's own state is
// under one mutex that is a leaf (only the model's own mutex is taken under it).

#include <alpacacore/util/sexagesimal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "fake_mount_motion_model.h"
#include "fake_mount_server.h"

namespace alpacacore::test {

class FakeOnStepMount {
public:
    using Axis = FakeMountMotionModel::Axis;
    /// OnStepX ends a MoveAxis this long after its last :Mn#/:Ms#/:Me#/:Mw# (GUIDE_TIME_LIMIT, default 10 s).
    static constexpr std::chrono::seconds kGuideTimeLimit{10};

    explicit FakeOnStepMount(FakeMountMotionModel::Config motion = conformu_motion_config(),
                             std::string version = "10.23a")
        : model_(std::move(motion)), version_(std::move(version)) {}

    FakeOnStepMount(const FakeOnStepMount&) = delete;
    FakeOnStepMount& operator=(const FakeOnStepMount&) = delete;

    /// The model setup of the fake ConformU row: the made-up Sydney Opera House site (never a real observing site),
    /// the mount a 4 degree/s GOTO and the real clock.
    static FakeMountMotionModel::Config conformu_motion_config() {
        FakeMountMotionModel::Config config;
        config.latitude_degrees = -33.856784;
        config.longitude_degrees = 151.215297;
        config.slew_deg_per_s = 4.0;
        return config;
    }

    FakeMountMotionModel& motion() { return model_; }

    /// The function a FakeMountServer hands each received chunk to.
    FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) { return receive(chunk); };
    }

    /// One '#'-terminated command in, the reply (empty for a command the firmware does not answer) out.
    std::string respond_to(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        return answer_locked(command);
    }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_;
    }
    int count(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(
            std::count_if(log_.begin(), log_.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }

private:
    using TimePoint = FakeMountMotionModel::TimePoint;

    // The server may deliver two commands in one chunk (a blind :RA# and the :Mn# behind it) or one in two.
    std::string receive(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ += chunk;
        std::string out;
        for (auto end = pending_.find('#'); end != std::string::npos; end = pending_.find('#')) {
            const std::string command = pending_.substr(0, end + 1);
            pending_.erase(0, end + 1);
            out += answer_locked(command);
        }
        return out;
    }

    // ---- formats (OnStepX, high-precision position reads) ----

    static std::string hms_precise(double hours) {
        long long ten_thousandths = std::llround(std::fmod(hours + 24.0, 24.0) * 3600.0 * 10000.0);
        ten_thousandths %= 24LL * 3600 * 10000;
        const long long seconds = ten_thousandths / 10000;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%04lld", seconds / 3600, (seconds % 3600) / 60,
                      seconds % 60, ten_thousandths % 10000);
        return buf;
    }
    static std::string dms_precise(double degrees) {
        const long long milli = std::llround(std::fabs(degrees) * 3600.0 * 1000.0);
        const long long seconds = milli / 1000;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%c%02lld*%02lld:%02lld.%03lld", degrees < 0.0 ? '-' : '+', seconds / 3600,
                      (seconds % 3600) / 60, seconds % 60, milli % 1000);
        return buf;
    }
    static std::string azimuth_text(double degrees) {
        const long long seconds = std::llround(std::fmod(degrees + 360.0, 360.0) * 3600.0) % (360LL * 3600);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%03lld*%02lld:%02lld", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
        return buf;
    }
    static std::string two_digits(int code) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d", code);
        return buf;
    }

    // OnStepX Errors.h command errors the replies below set (the :MS# digit is the code less 14).
    enum Error {
        kNone = 0,
        kParked = 9,  // "already parked"
        kNotParked = 11,
        kBelowHorizon = 15,
        kSlewInPark = 18,  // "mount parked"
        kGotoInProgress = 19,
        kInMotion = 22
    };

    // ---- time: the model's clock, plus what a :SL#/:SC# write moved it by ----

    std::time_t mount_utc_locked() const {
        return std::chrono::system_clock::to_time_t(model_.now()) + clock_offset_s_;
    }
    std::tm mount_tm_locked() const {
        const std::time_t t = mount_utc_locked();
        std::tm tm{};
        gmtime_r(&t, &tm);
        return tm;
    }
    void write_time_locked(const std::tm& wanted) {
        std::tm copy = wanted;
        clock_offset_s_ = static_cast<std::int64_t>(timegm(&copy)) -
                          static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(model_.now()));
    }

    // ---- :GU# ----

    std::string status_locked() {
        const auto s = model_.state();
        const bool parking = park_pending_ && s.slewing;
        const bool homing = home_pending_ && s.slewing;
        if (!s.slewing) park_pending_ = home_pending_ = false;
        std::string flags;
        if (!s.tracking) flags += 'n';
        if (!s.slewing) flags += 'N';
        flags += s.parked ? 'P' : parking ? 'I' : 'p';
        if (s.at_home) flags += 'H';
        if (homing) flags += 'h';
        if (moving_locked()) flags += 'g';
        if (pulsing_locked()) flags += 'G';
        if (tracking_rate_ == 1) flags += '(';
        if (tracking_rate_ == 2) flags += 'O';
        if (tracking_rate_ == 3) flags += 'k';
        flags += 'E';  // a German mount
        flags += (s.parked || s.at_home) ? 'o' : (s.pier_side == 0 ? 'T' : 'W');
        flags += "180";  // pulse-guide rate, guide rate, general error: '0' + the value
        return flags + "#";
    }

    // A MoveAxis is a model pulse of kGuideTimeLimit, so the model ends it at the limit exactly, and a re-sent command
    // restarts the pulse. The `g` flag and the refusals read these deadlines, not the model's MoveAxis state.
    bool moving_locked() const {
        const TimePoint now = model_.now();
        return now < move_until_[0] || now < move_until_[1];
    }
    bool pulsing_locked() const {
        const TimePoint now = model_.now();
        return now < pulse_until_[0] || now < pulse_until_[1];
    }

    bool stationary_for_guide_locked() {
        const auto s = model_.state();
        return !s.parked && !s.slewing;
    }

    // ---- the commands ----

    std::string numeric(bool ok, Error failure) {
        last_error_ = ok ? kNone : failure;
        return ok ? "1" : "0";
    }

    std::string answer_locked(const std::string& command) {
        log_.push_back(command);
        auto starts = [&](const char* prefix) { return command.rfind(prefix, 0) == 0; };
        const auto payload = [&](std::size_t skip) { return command.substr(skip, command.size() - skip - 1); };

        if (command == ":GVP#") return "On-Step#";
        if (command == ":GVN#") return version_ + "#";
        if (command == ":GXEE#") return "0";  // coordinates are not refracted
        if (command == ":GU#") return status_locked();
        if (command == ":GE#") return two_digits(last_error_) + "#";
        if (command == ":GRa#") return hms_precise(model_.ra_hours()) + "#";
        if (command == ":GDe#") return dms_precise(model_.dec_degrees()) + "#";
        if (command == ":GA#") return alpacacore::util::format_dms(model_.altitude_degrees(), 2, '*') + "#";
        if (command == ":GZ#") return azimuth_text(model_.azimuth_degrees()) + "#";
        if (command == ":Gh#") return "+00*#";  // horizon limit
        if (command == ":Go#") return "90*#";   // overhead limit
        if (command == ":GS#") return alpacacore::util::format_hms(model_.lst_hours()) + "#";
        if (command == ":Gt#") return alpacacore::util::format_dm(model_.site_latitude_degrees(), 2, '*') + "#";
        if (command == ":Gg#") {  // west-positive on the wire
            return alpacacore::util::format_dm(-model_.site_longitude_degrees(), 3, '*') + "#";
        }
        if (command == ":GL#") {
            const std::tm tm = mount_tm_locked();
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d#", tm.tm_hour, tm.tm_min, tm.tm_sec);
            return buf;
        }
        if (command == ":GC#") {
            const std::tm tm = mount_tm_locked();
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%02d/%02d/%02d#", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
            return buf;
        }
        if (command == ":GG#") return "+00:00#";  // the driver writes UTC as local time
        if (command == ":GT#") {
            static const char* const kHz[] = {"60.16427", "57.90000", "60.00000", "60.13600"};
            return (model_.tracking() ? std::string(kHz[tracking_rate_]) : std::string("0")) + "#";
        }
        if (command == ":GX90#") return "0.50#";  // pulse-guide rate, a multiple of sidereal
        if (command == ":GXTR#" || command == ":GXTD#") {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.8f#", command == ":GXTR#" ? rate_offset_[0] : rate_offset_[1]);
            return buf;
        }

        // Targets and sync
        if (starts(":Sr")) return set_target_ra_locked(payload(3));
        if (starts(":Sd")) return set_target_dec_locked(payload(3));
        if (command == ":CM#") return sync_locked();
        if (command == ":MS#") return goto_target_locked();

        // Site and time
        if (starts(":St")) return set_site_locked(payload(3), true);
        if (starts(":Sg")) return set_site_locked(payload(3), false);
        if (starts(":SL")) return set_clock_locked(payload(3), true);
        if (starts(":SC")) return set_clock_locked(payload(3), false);
        if (starts(":SG")) return "1";

        // Tracking
        if (command == ":Te#" || starts(":ST")) return set_tracking_locked(command != ":ST0#");
        if (command == ":TQ#" || command == ":TL#" || command == ":TS#" || command == ":TK#") {
            tracking_rate_ = command == ":TQ#" ? 0 : command == ":TL#" ? 1 : command == ":TS#" ? 2 : 3;
            return {};
        }
        if (starts(":SXTR,") || starts(":SXTD,")) {
            // Arcseconds per sidereal second on the wire; the model takes degrees per second.
            rate_offset_[command[4] == 'R' ? 0 : 1] = std::strtod(command.c_str() + 6, nullptr);
            constexpr double kSiderealSecond = 86164.0905 / 86400.0;
            model_.set_rate_offset(rate_offset_[0] / 3600.0 / kSiderealSecond,
                                   rate_offset_[1] / 3600.0 / kSiderealSecond);
            return numeric(true, kNone);
        }

        // Stops, moves and pulses: none of them is answered
        if (command == ":Q#") {
            stop_all_locked();
            return {};
        }
        if (command.size() == 4 && command[1] == 'Q' && std::string("nsew").find(command[2]) != std::string::npos) {
            stop_axis_locked(command[2]);
            return {};
        }
        if (starts(":RA")) {
            move_rate_[0] = std::strtod(command.c_str() + 3, nullptr);
            return {};
        }
        if (starts(":RE")) {
            move_rate_[1] = std::strtod(command.c_str() + 3, nullptr);
            return {};
        }
        if (command == ":RS#") return {};
        if (command.size() == 4 && command[1] == 'M' && std::string("nsew").find(command[2]) != std::string::npos) {
            start_move_locked(command[2]);
            return {};
        }
        if (starts(":Mg") && command.size() >= 9) {
            pulse_locked(command[3], std::atoi(command.c_str() + 4));
            return {};
        }

        // Park and home
        if (command == ":hP#") return park_locked();
        if (command == ":hR#") return unpark_locked();
        if (command == ":hQ#") return set_park_locked();
        if (command == ":hC#") {
            home_locked();
            return {};
        }
        return "0#";  // the unknown command of a controller without that module
    }

    std::string set_target_ra_locked(const std::string& text) {
        const auto hours = alpacacore::util::parse_sexagesimal(text);
        if (!hours || *hours < 0.0 || *hours >= 24.0) return numeric(false, kNone);
        target_ra_ = *hours;
        return numeric(true, kNone);
    }
    std::string set_target_dec_locked(const std::string& text) {
        const auto degrees = alpacacore::util::parse_sexagesimal(text);
        if (!degrees || std::fabs(*degrees) > 90.0) return numeric(false, kNone);
        target_dec_ = *degrees;
        return numeric(true, kNone);
    }

    std::string sync_locked() {
        if (model_.parked()) {
            last_error_ = kSlewInPark;
            return "E4#";
        }
        model_.set_position(target_ra_, target_dec_);
        last_error_ = kNone;
        return "N/A#";
    }

    std::string goto_target_locked() {
        const auto s = model_.state();
        if (s.parked) return refuse_goto(kSlewInPark, '4');
        if (s.slewing) {
            model_.stop();
            return refuse_goto(kGotoInProgress, '5');
        }
        if (moving_locked()) return refuse_goto(kInMotion, '8');
        if (model_.altitude_degrees_of(target_ra_, target_dec_) < 0.0) return refuse_goto(kBelowHorizon, '1');
        last_error_ = kNone;
        (void)model_.goto_radec(target_ra_, target_dec_);
        park_pending_ = home_pending_ = false;
        return "0";
    }
    std::string refuse_goto(Error code, char digit) {
        last_error_ = code;
        return std::string(1, digit);
    }

    // :Sg# is west-positive: east = -value. The firmware stores both to the arcminute.
    std::string set_site_locked(const std::string& text, bool latitude) {
        const auto degrees = alpacacore::util::parse_sexagesimal(text);
        if (!degrees) return numeric(false, kNone);
        if (latitude) {
            model_.set_site(*degrees, model_.site_longitude_degrees());
        } else {
            model_.set_site(model_.site_latitude_degrees(), -*degrees);
        }
        return numeric(true, kNone);
    }

    // :SLHH:MM:SS# then :SCMM/DD/YY#: each moves the mount's clock to the written part.
    std::string set_clock_locked(const std::string& text, bool time_of_day) {
        std::tm tm = mount_tm_locked();
        int a = 0, b = 0, c = 0;
        if (std::sscanf(text.c_str(), time_of_day ? "%d:%d:%d" : "%d/%d/%d", &a, &b, &c) != 3) return "0";
        if (time_of_day) {
            tm.tm_hour = a;
            tm.tm_min = b;
            tm.tm_sec = c;
        } else {
            tm.tm_mon = a - 1;
            tm.tm_mday = b;
            tm.tm_year = 100 + c;
        }
        write_time_locked(tm);
        return "1";
    }

    std::string set_tracking_locked(bool on) {
        if (on && model_.parked()) return numeric(false, kParked);  // only enabling is refused; off is a no-op
        model_.set_tracking(on);
        return numeric(true, kNone);
    }

    void stop_all_locked() {
        model_.stop();
        park_pending_ = home_pending_ = false;
        move_until_[0] = move_until_[1] = pulse_until_[0] = pulse_until_[1] = TimePoint{};
    }

    void stop_axis_locked(char direction) {
        const int axis = (direction == 'e' || direction == 'w') ? 0 : 1;
        model_.pulse(axis == 0 ? Axis::Ra : Axis::Dec, 0.0, std::chrono::milliseconds(0));
        move_until_[axis] = pulse_until_[axis] = TimePoint{};
    }

    // east and north raise RA and Dec.
    void start_move_locked(char direction) {
        if (!stationary_for_guide_locked()) return;
        const int axis = (direction == 'e' || direction == 'w') ? 0 : 1;
        const double sign = (direction == 'e' || direction == 'n') ? 1.0 : -1.0;
        model_.pulse(axis == 0 ? Axis::Ra : Axis::Dec, sign * std::fabs(move_rate_[axis]), kGuideTimeLimit);
        move_until_[axis] = model_.now() + kGuideTimeLimit;
        pulse_until_[axis] = TimePoint{};
    }

    void pulse_locked(char direction, int milliseconds) {
        if (!stationary_for_guide_locked() || std::string("nsew").find(direction) == std::string::npos) return;
        const int axis = (direction == 'e' || direction == 'w') ? 0 : 1;
        const double sign = (direction == 'e' || direction == 'n') ? 1.0 : -1.0;
        model_.pulse(axis == 0 ? Axis::Ra : Axis::Dec, sign * 0.5 * FakeMountMotionModel::kSiderealDegPerSec,
                     std::chrono::milliseconds(milliseconds));
        pulse_until_[axis] = model_.now() + std::chrono::milliseconds(milliseconds);
        move_until_[axis] = TimePoint{};
    }

    std::string park_locked() {
        if (model_.parked()) return numeric(false, kParked);
        if (model_.slewing() || moving_locked()) return numeric(false, kInMotion);
        model_.park();
        park_pending_ = true;
        home_pending_ = false;
        return numeric(true, kNone);
    }

    // Park.cpp set(): no new park position while parked, during a GOTO or during a guide move (a pulse included).
    std::string set_park_locked() {
        if (model_.parked()) return numeric(false, kParked);
        return numeric(!model_.slewing() && !moving_locked() && !pulsing_locked(), kInMotion);
    }

    std::string unpark_locked() {
        if (!model_.parked()) return numeric(false, kNotParked);
        model_.unpark();
        return numeric(true, kNone);
    }

    void home_locked() {
        if (model_.parked()) {
            last_error_ = kSlewInPark;
            return;
        }
        if (model_.slewing() || moving_locked()) {
            last_error_ = kInMotion;
            return;
        }
        last_error_ = kNone;
        model_.go_home();
        home_pending_ = true;
        park_pending_ = false;
    }

    FakeMountMotionModel model_;
    const std::string version_;
    mutable std::mutex mutex_;  // everything below; takes only the model's own mutex
    std::string pending_;
    std::vector<std::string> log_;
    double target_ra_ = 0.0, target_dec_ = 0.0;
    int tracking_rate_ = 0;  // 0 sidereal, 1 lunar, 2 solar, 3 King
    int last_error_ = kNone;
    bool park_pending_ = false;
    bool home_pending_ = false;
    double move_rate_[2] = {0.5, 0.5};    // :RA#, :RE#: degrees per second
    double rate_offset_[2] = {0.0, 0.0};  // :SXTR#, :SXTD#
    TimePoint move_until_[2]{};           // a MoveAxis per axis ends here (RA, Dec)
    TimePoint pulse_until_[2]{};          // a pulse guide per axis ends here
    std::int64_t clock_offset_s_ = 0;     // the mount's clock less the model's, set by :SL#/:SC#
};

}  // namespace alpacacore::test

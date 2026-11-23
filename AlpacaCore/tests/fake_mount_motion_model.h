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

// Protocol-neutral equatorial mount motion model for the fake ConformU rows (#2078, #2614). It knows how a German
// equatorial mount MOVES, not how any vendor talks: a protocol fake (FakeIoptronMount today; ZWO AM, CGX,
// NexStar/SynScan and OnStep next) keeps one FakeMountMotionModel, forwards each wire command to a method below and
// answers each read from a getter. It stays out of the wire format, so a second protocol adds no motion code.
//
// What it models, all in degrees (RA hours only at the edge, as the Alpaca units are):
//   - GOTO: a straight run at slew_deg_per_s; slewing() is true until the travel time has passed, then the position
//     is exactly the target. goto_radec() returns the travel time.
//   - Tracking: on, the sky position holds; off, it drifts east at the sidereal rate (the mount stands still while
//     the sky turns).
//   - MoveAxis: move_axis() runs one axis at a signed rate until stopped (rate 0, or stop()).
//   - Pulse guide: pulse() runs one axis at a signed rate for a duration, then stops by itself.
//   - Park and home: park() / go_home() slew to the configured mechanical position (hour angle, Dec); on arrival
//     parked() / at_home() is true and tracking is off. unpark() clears parked(); go_home() on arrival is at_home()
//     until the mount moves or tracks away.
//   - Side of pier from the hour angle (util::pier_side_for_hour_angle), Alt/Az, and local sidereal time from the
//     site and the clock.
//
// API shape (the open choice of #2614): QUERY-TIME computation, not a step(now) the owner must call. Every method
// first advances the model to the clock's "now" under one mutex, so a fake needs no worker thread and a getter is
// always current. A fake with a worker thread of its own can still call any getter on a schedule.
//
// Clock: a std::function<system_clock::time_point()>, defaulting to the real clock because sidereal time needs a
// real UTC instant. A unit test injects its own and advances it by hand.
//
// A protocol fake owns what is the protocol's: refusals (a target below the horizon, a command while parked), reply
// formats, the status codes the model's booleans map to, and command units. The model refuses nothing.

#include <alpacacore/util/pointing.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <utility>

namespace alpacacore::test {

class FakeMountMotionModel {
public:
    using TimePoint = std::chrono::system_clock::time_point;
    using Clock = std::function<TimePoint()>;

    /// Mechanical position of the two axes: hour angle (hours, east negative) and declination (degrees).
    struct AxisPosition {
        double hour_angle_hours = 0.0;
        double dec_degrees = 0.0;
    };

    enum class Axis { Ra = 0, Dec = 1 };

    struct Config {
        double latitude_degrees = -33.856784;  // made-up site (Sydney Opera House), never a real observing site
        double longitude_degrees = 151.215297;
        double slew_deg_per_s = 6.0;
        /// Where park() and go_home() go. The defaults put the OTA on the celestial pole, counterweight down.
        AxisPosition park{0.0, -90.0};
        AxisPosition home{0.0, -90.0};
        Clock clock = [] { return std::chrono::system_clock::now(); };
    };

    static constexpr double kSiderealDegPerSec = 360.98564736629 / 86400.0;

    FakeMountMotionModel() : FakeMountMotionModel(Config{}) {}
    explicit FakeMountMotionModel(Config config) : config_(std::move(config)) {
        stamp_ = config_.clock();
        // Start parked-at-home-like but tracking off, on the meridian: the state a mount powers up in.
        dec_ = config_.home.dec_degrees;
        ra_deg_ = util::wrap_degrees(lst_degrees(stamp_) - config_.home.hour_angle_hours * 15.0);
    }

    // ---- commands ----

    /// Starts a GOTO to the sky position and returns the travel time. Replaces a slew, a MoveAxis and any pulse in
    /// flight. A zero-distance GOTO is done at once.
    std::chrono::duration<double> goto_radec(double ra_hours, double dec_degrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return begin_slew_locked(Kind::Goto, util::wrap_degrees(ra_hours * 15.0), dec_degrees);
    }

    void park() {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        begin_slew_to_axes_locked(Kind::Park, config_.park);
    }

    /// Clears parked(); the mount stays where it is, tracking off, until the next command.
    void unpark() {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        parked_ = false;
    }

    void go_home() {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        begin_slew_to_axes_locked(Kind::Home, config_.home);
    }

    /// Stops a slew (park and home included), every MoveAxis and every pulse where they are. Tracking is unchanged.
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        slew_.active = false;
        axis_rate_[0] = axis_rate_[1] = 0.0;
        pulse_[0].until = pulse_[1].until = stamp_;
    }

    void set_tracking(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        tracking_ = on;
        if (on) parked_ = false;
    }

    /// Runs @p axis at @p deg_per_s (signed: positive raises RA or Dec) until stopped; 0 stops that axis.
    void move_axis(Axis axis, double deg_per_s) {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        axis_rate_[index(axis)] = deg_per_s;
    }

    /// Runs @p axis at @p deg_per_s (signed) for @p duration, then stops it.
    void pulse(Axis axis, double deg_per_s, std::chrono::milliseconds duration) {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        pulse_[index(axis)] = {deg_per_s, stamp_ + duration};
    }

    /// The mount's pointing is now (@p ra_hours, @p dec_degrees): a sync, or a test setting the start.
    void set_position(double ra_hours, double dec_degrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        ra_deg_ = util::wrap_degrees(ra_hours * 15.0);
        dec_ = dec_degrees;
    }

    void set_site(double latitude_degrees, double longitude_degrees) {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        // Hold the hour angle across the change so the pointing does not jump with the sidereal time.
        const double ha = ha_degrees_locked();
        config_.latitude_degrees = latitude_degrees;
        config_.longitude_degrees = longitude_degrees;
        ra_deg_ = util::wrap_degrees(lst_degrees(stamp_) - ha);
    }

    // ---- reads ----

    /// Everything the getters below report, read at one instant: a protocol fake answering a status read from
    /// several getters could straddle the end of a slew and report a state the mount never had (for example neither
    /// slewing nor parked at the end of a Park).
    struct State {
        double ra_hours = 0.0;
        double dec_degrees = 0.0;
        double hour_angle_hours = 0.0;  // east negative, in [-12, 12)
        int pier_side = 0;              // 0 pier east, 1 pier west
        bool slewing = false;           // a GOTO, park or home slew is in flight
        bool tracking = false;
        bool parked = false;
        bool at_home = false;
        bool axis_moving[2] = {false, false};  // indexed by Axis
        bool pulse_active = false;
    };
    State state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        State s;
        s.ra_hours = ra_deg_ / 15.0;
        s.dec_degrees = dec_;
        s.hour_angle_hours = ha_degrees_locked() / 15.0;
        s.pier_side = util::pier_side_for_hour_angle(s.hour_angle_hours);
        s.slewing = slew_.active;
        s.tracking = tracking_;
        s.parked = parked_;
        s.at_home = at_home_locked();
        s.axis_moving[0] = axis_rate_[0] != 0.0;
        s.axis_moving[1] = axis_rate_[1] != 0.0;
        s.pulse_active = pulse_[0].until > stamp_ || pulse_[1].until > stamp_;
        return s;
    }

    double ra_hours() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return ra_deg_ / 15.0;
    }
    double dec_degrees() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return dec_;
    }
    /// East negative, in [-12, 12).
    double hour_angle_hours() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return ha_degrees_locked() / 15.0;
    }
    double lst_hours() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return lst_degrees(stamp_) / 15.0;
    }
    double altitude_degrees() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return altaz_locked(true);
    }
    /// North 0, east 90.
    double azimuth_degrees() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return altaz_locked(false);
    }
    /// 0 pier east, 1 pier west: the hour-angle rule every driver shares.
    int pier_side() const { return util::pier_side_for_hour_angle(hour_angle_hours()); }

    /// A GOTO, park or home slew is in flight.
    bool slewing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return slew_.active;
    }
    bool tracking() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return tracking_;
    }
    bool parked() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return parked_;
    }
    /// Standing on the home position: not slewing, tracking off, within 0.01 degrees of it.
    bool at_home() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return at_home_locked();
    }
    /// A MoveAxis is running on @p axis.
    bool axis_moving(Axis axis) const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return axis_rate_[index(axis)] != 0.0;
    }
    /// A pulse is running on either axis.
    bool pulse_active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return pulse_[0].until > stamp_ || pulse_[1].until > stamp_;
    }
    double slew_deg_per_s() const { return config_.slew_deg_per_s; }
    double site_latitude_degrees() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.latitude_degrees;
    }
    double site_longitude_degrees() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.longitude_degrees;
    }
    /// The model's clock, which a protocol fake's own time keeping (a mount's UTC read) is relative to.
    TimePoint now() const { return config_.clock(); }

    /// Altitude of a sky position from this site at the current sidereal time, for a protocol's horizon refusal.
    double altitude_degrees_of(double ra_hours, double dec_degrees) const {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_locked();
        return altitude_of_locked(util::wrap_degrees(lst_degrees(stamp_) - ra_hours * 15.0), dec_degrees);
    }

private:
    enum class Kind { Goto, Park, Home };
    struct Slew {
        bool active = false;
        Kind kind = Kind::Goto;
        double start_ra = 0.0, start_dec = 0.0;
        double target_ra = 0.0, target_dec = 0.0;
        TimePoint t0{}, t1{};
    };
    struct Pulse {
        double rate = 0.0;
        TimePoint until{};
    };

    static constexpr double kAtPositionDeg = 0.01;

    static std::size_t index(Axis axis) { return static_cast<std::size_t>(axis); }

    double lst_degrees(TimePoint at) const {
        return util::wrap_degrees(util::gmst_degrees(at) + config_.longitude_degrees);
    }
    double ha_degrees_locked() const {
        double ha = std::remainder(lst_degrees(stamp_) - ra_deg_, 360.0);
        if (ha >= 180.0) ha -= 360.0;
        return ha;
    }
    bool at_home_locked() const {
        if (slew_.active || tracking_ || moving_locked()) return false;
        const double dha = std::remainder(ha_degrees_locked() - config_.home.hour_angle_hours * 15.0, 360.0);
        return std::fabs(dha) < kAtPositionDeg && std::fabs(dec_ - config_.home.dec_degrees) < kAtPositionDeg;
    }
    bool moving_locked() const {
        return axis_rate_[0] != 0.0 || axis_rate_[1] != 0.0 || pulse_[0].until > stamp_ || pulse_[1].until > stamp_;
    }

    static constexpr double kRad = 3.14159265358979323846 / 180.0;

    double altitude_of_locked(double ha_deg, double dec_deg) const {
        const double ha = ha_deg * kRad;
        const double dec = dec_deg * kRad;
        const double lat = config_.latitude_degrees * kRad;
        const double sin_alt = std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(lat) * std::cos(ha);
        return std::asin(std::clamp(sin_alt, -1.0, 1.0)) / kRad;
    }

    double altaz_locked(bool altitude) const {
        if (altitude) return altitude_of_locked(ha_degrees_locked(), dec_);
        const double ha = ha_degrees_locked() * kRad;
        const double dec = dec_ * kRad;
        const double lat = config_.latitude_degrees * kRad;
        const double az = std::atan2(-std::cos(dec) * std::sin(ha),
                                     std::sin(dec) * std::cos(lat) - std::cos(dec) * std::sin(lat) * std::cos(ha));
        return util::wrap_degrees(az / kRad);
    }

    std::chrono::duration<double> begin_slew_locked(Kind kind, double target_ra_deg, double target_dec) {
        slew_.active = false;
        axis_rate_[0] = axis_rate_[1] = 0.0;
        pulse_[0].until = pulse_[1].until = stamp_;
        parked_ = false;
        const double d_ra = std::remainder(target_ra_deg - ra_deg_, 360.0);
        const double d_dec = target_dec - dec_;
        const std::chrono::duration<double> travel(std::max(std::fabs(d_ra), std::fabs(d_dec)) /
                                                   config_.slew_deg_per_s);
        if (travel.count() <= 0.0) {
            ra_deg_ = target_ra_deg;
            dec_ = target_dec;
            finish_slew_kind_locked(kind);
            return travel;
        }
        slew_.active = true;
        slew_.kind = kind;
        slew_.start_ra = ra_deg_;
        slew_.start_dec = dec_;
        slew_.target_ra = target_ra_deg;
        slew_.target_dec = target_dec;
        slew_.t0 = stamp_;
        slew_.t1 = stamp_ + std::chrono::duration_cast<TimePoint::duration>(travel);
        return travel;
    }

    // Park and home name mechanical axes, so the sky target depends on when the slew ends: aim at the sidereal
    // time of the arrival, found with one refinement of the travel time.
    void begin_slew_to_axes_locked(Kind kind, const AxisPosition& at) {
        const auto sky_target = [&](TimePoint arrival) {
            return util::wrap_degrees(lst_degrees(arrival) - at.hour_angle_hours * 15.0);
        };
        const double first = sky_target(stamp_);
        const double d_ra = std::remainder(first - ra_deg_, 360.0);
        const double d_dec = at.dec_degrees - dec_;
        const auto travel = std::chrono::duration_cast<TimePoint::duration>(
            std::chrono::duration<double>(std::max(std::fabs(d_ra), std::fabs(d_dec)) / config_.slew_deg_per_s));
        begin_slew_locked(kind, sky_target(stamp_ + travel), at.dec_degrees);
    }

    void finish_slew_kind_locked(Kind kind) const {
        if (kind == Kind::Goto) return;
        tracking_ = false;
        parked_ = (kind == Kind::Park);
    }

    // Integrate the position from stamp_ to the clock's now, split at every event (slew end, pulse end) so each
    // stretch has constant rates. const: a read advances the model, whose state is a cache of "where is it now".
    void advance_locked() const {
        const TimePoint now = config_.clock();
        while (stamp_ < now) {
            TimePoint end = now;
            if (slew_.active) end = std::min(end, slew_.t1);
            for (const auto& p : pulse_) {
                if (p.until > stamp_) end = std::min(end, p.until);
            }
            const double dt = std::chrono::duration<double>(end - stamp_).count();
            if (slew_.active) {
                if (end >= slew_.t1) {
                    ra_deg_ = slew_.target_ra;
                    dec_ = slew_.target_dec;
                    slew_.active = false;
                    finish_slew_kind_locked(slew_.kind);
                } else {
                    const double span = std::chrono::duration<double>(slew_.t1 - slew_.t0).count();
                    const double frac = std::chrono::duration<double>(end - slew_.t0).count() / span;
                    ra_deg_ = util::wrap_degrees(slew_.start_ra +
                                                 frac * std::remainder(slew_.target_ra - slew_.start_ra, 360.0));
                    dec_ = slew_.start_dec + frac * (slew_.target_dec - slew_.start_dec);
                }
            } else {
                double ra_rate = axis_rate_[0] + (tracking_ ? 0.0 : kSiderealDegPerSec);
                double dec_rate = axis_rate_[1];
                if (pulse_[0].until > stamp_) ra_rate += pulse_[0].rate;
                if (pulse_[1].until > stamp_) dec_rate += pulse_[1].rate;
                ra_deg_ = util::wrap_degrees(ra_deg_ + ra_rate * dt);
                dec_ = std::clamp(dec_ + dec_rate * dt, -90.0, 90.0);
            }
            stamp_ = end;
        }
    }

    Config config_;
    mutable std::mutex mutex_;
    mutable TimePoint stamp_{};
    mutable double ra_deg_ = 0.0;
    mutable double dec_ = 0.0;
    mutable bool tracking_ = false;
    mutable bool parked_ = false;
    mutable Slew slew_;
    double axis_rate_[2] = {0.0, 0.0};
    Pulse pulse_[2];
};

}  // namespace alpacacore::test

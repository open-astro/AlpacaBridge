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

// The remotealpaca Telescope proxied device (decision 0023, issue #1753 slice 5, #2710) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the other proxied devices. What is the Telescope's own
// stays here:
//
// - The frame (decision 0010): RightAscension, Declination, Altitude, Azimuth, SiderealTime, UTCDate, Slewing,
//   Tracking, AtPark, AtHome, SideOfPier and IsPulseGuiding, twelve GETs per poll. SiderealTime and UTCDate are served
//   advanced by the frame's age: they are clocks, and the age is known.
// - What the Connect reads once (Settings): the capabilities, CanMoveAxis and AxisRates per axis, the alignment, the
//   optics, the site, the rates, the targets, DoesRefraction and SlewSettleTime. They change only by a write, so this
//   server's writes write them through; a change another client makes upstream shows at the next Connect.
// - The commands, forwarded in the command slot in caller order. A command refuses in the gate's order: argument
//   (InvalidValue, judged here for the ranges every telescope shares), connected, link fault; the upstream judges the
//   rest (parked, Tracking, its own ranges), and its error passes through. AbortSlew and MoveAxis(axis, 0) are
//   forwarded even while the link is faulted.
// - The synchronous slews (SlewToCoordinates, SlewToTarget, SlewToAltAz) are deprecated in ITelescopeV4, and an
//   Alpaca mount should not implement them: they answer NotImplemented and CanSlew and CanSlewAltAz read false. A
//   synchronous slew forwarded would also outlast the 5 s per-call timeout.
// - The motion guards that fit a proxy (util/motion_guards.h): a MoveAxis this server sent is capped at
//   util::kMaxMoveAxisDuration by a verified stop in that axis's own slot (util::run_motion_deadline), because the
//   upstream may cap nothing (the simulators, a COM driver behind ASCOM Remote); and a Connect after a link lost while
//   this server's GoTo, Park, FindHome or MoveAxis ran stops a mount that still slews (util::RelinkMotionStop). The
//   slew deadline, the landing gate and the sync bound are the upstream driver's: the upstream runs the slew and
//   keeps the pointing model.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_guards.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/telescope_command_gate.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_telescope_driver.h>
#include <alpacacore/version.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "upstream_proxy.h"

namespace alpacacore::vendor::remotealpaca {

namespace {

using detail::Answer;
using detail::answer_of;
using detail::number_text;
using TimePoint = std::chrono::system_clock::time_point;
using RateRanges = std::vector<std::pair<double, double>>;

// Every Alpaca telescope is at least an ITelescopeV3 (Alpaca began with Platform 6). Its members are the ones this
// driver forwards; V4 added only Connect, Disconnect, Connecting and DeviceState, which this server answers itself.
// An older upstream has no Alpaca member set to map, so the Connect refuses it.
constexpr int kFirstSupportedVersion = 3;
constexpr int kAxes = 3;
constexpr double kSiderealPerSolar = 1.00273790935;

// The argument checks every telescope shares (the gate's universal ranges): no bound of the upstream's own is known
// here, so a PulseGuide takes the default bound and the upstream judges a MoveAxis rate.
util::TelescopeCommandLimits command_limits() {
    util::TelescopeCommandLimits limits;
    limits.pulse_guide_max_ms = kDefaultMaxPulseGuideMs;
    limits.guide_rate_ra_min = 0.0;
    limits.guide_rate_dec_min = 0.0;
    limits.tracking_rate_max = 3;      // DriveRates: sidereal, lunar, solar, King
    limits.stop_checks_fault = false;  // MoveAxis(axis, 0) is forwarded while the link is faulted
    return limits;
}

const util::TelescopeCommandLimits kLimits = command_limits();

// The polled frame (decision 0010): what the upstream answered for each member that changes while connected.
struct Frame {
    Answer<double> right_ascension;
    Answer<double> declination;
    Answer<double> altitude;
    Answer<double> azimuth;
    Answer<double> sidereal_time;
    Answer<TimePoint> utc_date;
    Answer<bool> slewing;
    Answer<bool> tracking;
    Answer<bool> at_park;
    Answer<bool> at_home;
    Answer<int> side_of_pier;
    Answer<bool> is_pulse_guiding;
};

using Proxy = detail::UpstreamProxy<Frame>;

// What the Connect reads once, each a value or the upstream's error; this server's writes write them through.
struct Settings {
    Answer<bool> can_find_home;
    Answer<bool> can_park;
    Answer<bool> can_pulse_guide;
    Answer<bool> can_set_declination_rate;
    Answer<bool> can_set_guide_rates;
    Answer<bool> can_set_park;
    Answer<bool> can_set_pier_side;
    Answer<bool> can_set_right_ascension_rate;
    Answer<bool> can_set_tracking;
    Answer<bool> can_slew_alt_az_async;
    Answer<bool> can_slew_async;
    Answer<bool> can_sync;
    Answer<bool> can_sync_alt_az;
    Answer<bool> can_unpark;
    std::array<Answer<bool>, kAxes> can_move_axis;
    std::array<Answer<RateRanges>, kAxes> axis_rates;
    Answer<int> alignment_mode;
    Answer<int> equatorial_system;
    Answer<double> aperture_area;
    Answer<double> aperture_diameter;
    Answer<double> focal_length;
    Answer<bool> does_refraction;
    Answer<int> slew_settle_time;
    Answer<double> site_latitude;
    Answer<double> site_longitude;
    Answer<double> site_elevation;
    Answer<int> tracking_rate;
    Answer<std::vector<int>> tracking_rates;
    Answer<double> right_ascension_rate;
    Answer<double> declination_rate;
    Answer<double> guide_rate_right_ascension;
    Answer<double> guide_rate_declination;
    Answer<double> target_right_ascension;
    Answer<double> target_declination;
};

template <class T>
Answer<T> set_to(T value) {
    return {std::move(value), 0, {}};
}

// Days from 1970-01-01 to a proleptic Gregorian date (H. Hinnant's days_from_civil).
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The upstream's UTCDate, ISO 8601 "YYYY-MM-DDTHH:MM:SS[.fraction][Z]" (the Alpaca form), or nullopt.
std::optional<TimePoint> parse_utc(const std::string& text) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second,
                    &consumed) != 6 ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    std::size_t at = static_cast<std::size_t>(consumed);
    std::chrono::nanoseconds fraction{0};
    if (at < text.size() && text[at] == '.') {
        long long scale = 100000000;
        for (++at; at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0; ++at) {
            fraction += std::chrono::nanoseconds(static_cast<long long>(text[at] - '0') * scale);
            scale /= 10;
        }
    }
    if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) ++at;
    if (at != text.size()) return std::nullopt;
    const auto seconds = std::chrono::seconds(days_from_civil(year, month, day) * 86400 + std::int64_t{hour} * 3600 +
                                              std::int64_t{minute} * 60 + second);
    return TimePoint(std::chrono::duration_cast<TimePoint::duration>(seconds + fraction));
}

const char* on_off(bool value) { return value ? "True" : "False"; }

class RemoteAlpacaTelescopeDriver final : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaTelescopeDriver(int device_number, const RemoteDeviceConfig& config,
                                std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // ITelescopeV4 is the first Telescope with Connect; an ITelescopeV3 upstream gets Connected=true.
          proxy_({"telescope", "Telescope", 4, [this] { return sample(); }}, config, std::move(http), std::move(self),
                 clock) {
        for (int axis = 0; axis < kAxes; ++axis) {
            move_axis_cap_[axis] =
                std::make_unique<util::AsyncOperation>("remotealpaca MoveAxis cap " + std::to_string(axis),
                                                       cap_generation_[static_cast<std::size_t>(axis)], clock);
        }
    }

    ~RemoteAlpacaTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        stop_move_axis_caps();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }
    std::string get_description() const override { return proxy_.description(); }
    DeviceType get_device_type() const override { return DeviceType::Telescope; }
    std::string fallback_unique_id() const override { return "REMOTEALPACA_TEL_" + std::to_string(device_number_); }
    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca Telescope Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }  // ITelescopeV4 (Platform 7)

    // A loss the module latched (a frame or a fault past the bound) reads false at once, before on_lost() runs.
    bool get_connected() const override { return proxy_.connected_flag().load() && !proxy_.link_lost(); }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    // Decision 0009 rule 5: the latched fault, or after a loss the reason it was lost, kept until the next Connect or
    // Disconnect.
    std::string get_link_fault() const override { return proxy_.link_fault(); }

    void set_connected(bool connected) override {
        // Issue #528: the connect makes HTTP calls, so a sync disconnect must not slip in between its checks and its
        // store. Guards set_connected() against set_connected() only; no getter takes it.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(proxy_.connected_flag().load())) {
            return;
        }
        if (connected && consume_pending_disconnect(proxy_.connected_flag().load())) {
            return;
        }
        if (connected && get_connected()) {
            return;  // idempotent: connected, and the poll is running
        }
        // The caps of the session that ends: a cap's stop runs as a command, which no Connect or Disconnect waits for
        // under a lock the cap needs.
        stop_move_axis_caps();
        end_motion_session();
        if (connected) {
            proxy_.connect([this] { return read_settings(); });
        } else {
            proxy_.disconnect();
        }
    }

    // ── Capabilities: read at Connect; while disconnected the last Connect's answer, false before any ───────────

    bool get_can_find_home() const override { return can(&Settings::can_find_home); }
    bool get_can_park() const override { return can(&Settings::can_park); }
    bool get_can_pulse_guide() const override { return can(&Settings::can_pulse_guide); }
    bool get_can_set_declination_rate() const override { return can(&Settings::can_set_declination_rate); }
    bool get_can_set_guide_rates() const override { return can(&Settings::can_set_guide_rates); }
    bool get_can_set_park() const override { return can(&Settings::can_set_park); }
    bool get_can_set_pier_side() const override { return can(&Settings::can_set_pier_side); }
    bool get_can_set_right_ascension_rate() const override { return can(&Settings::can_set_right_ascension_rate); }
    bool get_can_set_tracking() const override { return can(&Settings::can_set_tracking); }
    bool get_can_slew_alt_az_async() const override { return can(&Settings::can_slew_alt_az_async); }
    bool get_can_slew_async() const override { return can(&Settings::can_slew_async); }
    bool get_can_sync() const override { return can(&Settings::can_sync); }
    bool get_can_sync_alt_az() const override { return can(&Settings::can_sync_alt_az); }
    bool get_can_unpark() const override { return can(&Settings::can_unpark); }
    // The synchronous slews are not proxied (ITelescopeV4: an Alpaca mount returns false).
    bool get_can_slew() const override { return false; }
    bool get_can_slew_alt_az() const override { return false; }

    bool get_can_move_axis(int axis) const override {
        validate_axis(axis, "CanMoveAxis");
        return proxy_.with_state([this, axis](const Proxy::State&) {
            const Answer<bool>& answer = settings_.can_move_axis[static_cast<std::size_t>(axis)];
            if (!proxy_.connected()) return answer.value.value_or(false);
            return answer.get();
        });
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        const RateRanges ranges = get_axis_rate_ranges(axis);
        return ranges.empty() ? std::pair<double, double>{0.0, 0.0} : ranges.front();
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        validate_axis(axis, "AxisRates");
        return setting([axis](const Settings& s) { return s.axis_rates[static_cast<std::size_t>(axis)].get(); });
    }

    // ── Read at Connect, written through by this server's writes; static while faulted ────────────────────────

    AlignmentMode get_alignment_mode() const override {
        const int mode = setting([](const Settings& s) { return s.alignment_mode.get(); });
        if (mode < 0 || mode > 2) {
            throw AlpacaException("Upstream AlignmentMode " + std::to_string(mode) + " is not an ASCOM AlignmentModes",
                                  AlpacaError::DriverException);
        }
        return static_cast<AlignmentMode>(mode);
    }

    EquatorialSystem get_equatorial_system() const override {
        const int system = setting([](const Settings& s) { return s.equatorial_system.get(); });
        if (system < 0 || system > 4) {
            throw AlpacaException(
                "Upstream EquatorialSystem " + std::to_string(system) + " is not an ASCOM EquatorialCoordinateType",
                AlpacaError::DriverException);
        }
        return static_cast<EquatorialSystem>(system);
    }

    double get_aperture_area() const override {
        return setting([](const Settings& s) { return s.aperture_area.get(); });
    }
    double get_aperture_diameter() const override {
        return setting([](const Settings& s) { return s.aperture_diameter.get(); });
    }
    double get_focal_length() const override {
        return setting([](const Settings& s) { return s.focal_length.get(); });
    }

    // Config injection, not ASCOM members: the optics are the upstream's, and this vendor's config has none.
    void set_aperture_diameter(double) override {
        throw AlpacaException("The optics of a Remote Alpaca telescope are the upstream's",
                              AlpacaError::NotImplemented);
    }
    void set_focal_length(double) override {
        throw AlpacaException("The optics of a Remote Alpaca telescope are the upstream's",
                              AlpacaError::NotImplemented);
    }

    bool get_does_refraction() const override {
        return setting([](const Settings& s) { return s.does_refraction.get(); });
    }
    void set_does_refraction(bool value) override {
        write_setting("doesrefraction", {{"DoesRefraction", on_off(value)}},
                      [value](Settings& s) { s.does_refraction = set_to(value); });
    }

    int get_slew_settle_time() const override {
        return setting([](const Settings& s) { return s.slew_settle_time.get(); });
    }
    // The client's value, kept by the upstream: not refused while the link is faulted (the call itself may fail).
    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("SlewSettleTime must be 0 or more seconds", AlpacaError::InvalidValue);
        }
        proxy_.require_connected();
        run([this, seconds](util::OperationContext& ctx) {
            proxy_.client().put("slewsettletime", {{"SlewSettleTime", std::to_string(seconds)}});
            store(ctx, [seconds](Settings& s) { s.slew_settle_time = set_to(seconds); });
        });
    }

    double get_site_latitude() const override {
        return setting([](const Settings& s) { return s.site_latitude.get(); });
    }
    void set_site_latitude(double degrees) override {
        util::validate_latitude(degrees);
        write_setting("sitelatitude", {{"SiteLatitude", number_text(degrees)}},
                      [degrees](Settings& s) { s.site_latitude = set_to(degrees); });
    }

    double get_site_longitude() const override {
        return setting([](const Settings& s) { return s.site_longitude.get(); });
    }
    void set_site_longitude(double degrees) override {
        util::validate_longitude(degrees);
        write_setting("sitelongitude", {{"SiteLongitude", number_text(degrees)}},
                      [degrees](Settings& s) { s.site_longitude = set_to(degrees); });
    }

    double get_site_elevation() const override {
        return setting([](const Settings& s) { return s.site_elevation.get(); });
    }
    void set_site_elevation(double meters) override {
        if (!std::isfinite(meters) || meters < -300.0 || meters > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters", AlpacaError::InvalidValue);
        }
        write_setting("siteelevation", {{"SiteElevation", number_text(meters)}},
                      [meters](Settings& s) { s.site_elevation = set_to(meters); });
    }

    int get_tracking_rate() const override {
        return setting([](const Settings& s) { return s.tracking_rate.get(); });
    }
    void set_tracking_rate(int rate) override {
        util::check_tracking_rate_arguments(kLimits, rate);
        write_setting("trackingrate", {{"TrackingRate", std::to_string(rate)}},
                      [rate](Settings& s) { s.tracking_rate = set_to(rate); });
    }

    std::vector<int> get_tracking_rates() const override {
        return setting([](const Settings& s) { return s.tracking_rates.get(); });
    }

    double get_right_ascension_rate() const override {
        return setting([](const Settings& s) { return s.right_ascension_rate.get(); });
    }
    void set_right_ascension_rate(double rate) override {
        util::check_rate_offset_arguments(kLimits, util::RateOffsetAxis::RightAscension, rate);
        write_setting("rightascensionrate", {{"RightAscensionRate", number_text(rate)}},
                      [rate](Settings& s) { s.right_ascension_rate = set_to(rate); });
    }

    double get_declination_rate() const override {
        return setting([](const Settings& s) { return s.declination_rate.get(); });
    }
    void set_declination_rate(double rate) override {
        util::check_rate_offset_arguments(kLimits, util::RateOffsetAxis::Declination, rate);
        write_setting("declinationrate", {{"DeclinationRate", number_text(rate)}},
                      [rate](Settings& s) { s.declination_rate = set_to(rate); });
    }

    GuideRate get_guide_rate() const override {
        return setting([](const Settings& s) {
            return GuideRate{s.guide_rate_right_ascension.get(), s.guide_rate_declination.get()};
        });
    }
    // Each axis is its own upstream member, so a write of one never writes the other back (#2465).
    void set_guide_rate(const GuideRate& rate) override {
        util::check_guide_rate_arguments(kLimits, util::GuideRateAxis::RightAscension, rate.ra);
        util::check_guide_rate_arguments(kLimits, util::GuideRateAxis::Declination, rate.dec);
        require_frame();
        run([this, rate](util::OperationContext& ctx) {
            RemoteAlpacaClient& client = proxy_.client();
            client.put("guideraterightascension", {{"GuideRateRightAscension", number_text(rate.ra)}});
            store(ctx, [rate](Settings& s) { s.guide_rate_right_ascension = set_to(rate.ra); });
            client.put("guideratedeclination", {{"GuideRateDeclination", number_text(rate.dec)}});
            store(ctx, [rate](Settings& s) { s.guide_rate_declination = set_to(rate.dec); });
        });
    }
    void set_guide_rate_right_ascension(double value) override {
        util::check_guide_rate_arguments(kLimits, util::GuideRateAxis::RightAscension, value);
        write_setting("guideraterightascension", {{"GuideRateRightAscension", number_text(value)}},
                      [value](Settings& s) { s.guide_rate_right_ascension = set_to(value); });
    }
    void set_guide_rate_declination(double value) override {
        util::check_guide_rate_arguments(kLimits, util::GuideRateAxis::Declination, value);
        write_setting("guideratedeclination", {{"GuideRateDeclination", number_text(value)}},
                      [value](Settings& s) { s.guide_rate_declination = set_to(value); });
    }

    // The upstream's targets: an unset one answers the upstream's own error, unchanged. Each setter writes its own
    // only, so the two stay independent.
    double get_target_right_ascension() const override {
        return setting([](const Settings& s) { return s.target_right_ascension.get(); });
    }
    void set_target_right_ascension(double hours) override {
        util::check_target_right_ascension_arguments(hours);
        write_setting("targetrightascension", {{"TargetRightAscension", number_text(hours)}},
                      [hours](Settings& s) { s.target_right_ascension = set_to(hours); });
    }
    double get_target_declination() const override {
        return setting([](const Settings& s) { return s.target_declination.get(); });
    }
    void set_target_declination(double degrees) override {
        util::check_target_declination_arguments(degrees);
        write_setting("targetdeclination", {{"TargetDeclination", number_text(degrees)}},
                      [degrees](Settings& s) { s.target_declination = set_to(degrees); });
    }

    // ── The frame: a stale frame is served, a latched fault refuses it (decision 0009 rule 1) ────────────────

    double get_right_ascension() const override { return served(&Frame::right_ascension); }
    double get_declination() const override { return served(&Frame::declination); }
    double get_altitude() const override { return served(&Frame::altitude); }
    double get_azimuth() const override { return served(&Frame::azimuth); }
    bool get_tracking() const override { return served(&Frame::tracking); }
    bool get_at_park() const override { return served(&Frame::at_park); }
    bool get_at_home() const override { return served(&Frame::at_home); }
    int get_side_of_pier() const override { return served(&Frame::side_of_pier); }
    bool get_is_pulse_guiding() const override { return served(&Frame::is_pulse_guiding); }

    // A MoveAxis cap that ended the move keeps its outcome here until the next motion command or Connect (a failed
    // async operation body keeps its error). Otherwise the frame: a lost link never writes it false, and a fault
    // refuses it, so Slewing stays unknown, never false, until a poll measures the mount.
    bool get_slewing() const override {
        return proxy_.with_connected([this](const Proxy::State& state) {
            if (!motion_error_.empty()) throw AlpacaException(motion_error_, AlpacaError::DriverException);
            return state.served().value.slewing.get();
        });
    }

    // Clocks: the upstream's value advanced by the frame's age.
    double get_sidereal_time() const override {
        return proxy_.with_connected([this](const Proxy::State& state) {
            const auto reading = state.served();
            const double hours = reading.value.sidereal_time.get();
            const double age_s = std::chrono::duration<double>(proxy_.clock().now() - reading.measured_at).count();
            return util::wrap_hours(hours + age_s * kSiderealPerSolar / 3600.0);
        });
    }

    TimePoint get_utc_date() const override {
        return proxy_.with_connected([this](const Proxy::State& state) {
            const auto reading = state.served();
            return reading.value.utc_date.get() +
                   std::chrono::duration_cast<TimePoint::duration>(proxy_.clock().now() - reading.measured_at);
        });
    }

    void set_utc_date(TimePoint utc) override {
        require_frame();
        run([this, utc](util::OperationContext& ctx) {
            proxy_.client().put("utcdate", {{"UTCDate", device_state_timestamp(utc)}});
            write_through(ctx, [this, utc](Proxy::State& state) {
                // Served as the stored value plus the frame's age, so the stored value is the write less that age.
                const auto age = state.age().value_or(std::chrono::nanoseconds{0});
                state.write([&](Frame& frame) {
                    frame.utc_date = set_to(utc - std::chrono::duration_cast<TimePoint::duration>(age));
                });
            });
        });
    }

    void set_tracking(bool tracking) override {
        require_frame();
        run([this, tracking](util::OperationContext& ctx) {
            proxy_.client().put("tracking", {{"Tracking", on_off(tracking)}});
            write_frame(ctx, [tracking](Frame& frame) { frame.tracking = set_to(tracking); });
        });
    }

    // A SideOfPier write flips the mount: a slew this server started.
    void set_side_of_pier(int side) override {
        if (side != 0 && side != 1) {
            throw AlpacaException("SideOfPier must be pierEast (0) or pierWest (1)", AlpacaError::InvalidValue);
        }
        require_frame();
        run([this, side](util::OperationContext& ctx) {
            proxy_.client().put("sideofpier", {{"SideOfPier", std::to_string(side)}});
            started_motion(ctx);
        });
    }

    // A parameterized query: no frame can hold it, so it is forwarded in the command slot (the per-call timeout
    // bounds it), after the arguments, the connection and the link fault.
    int get_destination_side_of_pier(double ra, double dec) const override {
        util::check_destination_side_of_pier_arguments(ra, dec);
        require_frame();
        int side = -1;
        auto* self = const_cast<RemoteAlpacaTelescopeDriver*>(this);
        self->run([self, ra, dec, &side](util::OperationContext&) {
            side = self->proxy_.client().get_int(
                "destinationsideofpier", {{"RightAscension", number_text(ra)}, {"Declination", number_text(dec)}});
        });
        return side;
    }

    // ── Motion ──────────────────────────────────────────────────────────────────────────────────────────────────

    // Forwarded even while the link is faulted: the safety stop is refused only for a lost link (NotConnected).
    // Slewing is left to the poll (ITelescopeV4: it stays true until the motion has stopped).
    void abort_slew() override {
        proxy_.require_connected();
        for (auto& cap : move_axis_cap_) cap->cancel();
        run([this](util::OperationContext&) {
            proxy_.client().put("abortslew");
            proxy_.with_state([this](Proxy::State&) {
                driver_motion_ = false;
                axis_moving_ = {false, false, false};
                motion_error_.clear();
            });
        });
    }

    // MoveAxis(axis, 0) is forwarded even while the link is faulted, as AbortSlew. A non-zero rate is refused while
    // faulted; the upstream judges it against its AxisRates. Slewing is written true at once, and the move is capped.
    void move_axis(int axis, double rate) override {
        const bool stop = util::check_move_axis(kLimits, axis, rate, StateChecks{*this});
        const auto index = static_cast<std::size_t>(axis);
        if (stop) {
            move_axis_cap_[index]->cancel();
            run([this, axis, index](util::OperationContext&) {
                proxy_.client().put("moveaxis", {{"Axis", std::to_string(axis)}, {"Rate", "0"}});
                proxy_.with_state([this, index](Proxy::State&) {
                    axis_moving_[index] = false;
                    motion_error_.clear();
                });
            });
            return;
        }
        run([this, axis, index, rate](util::OperationContext& ctx) {
            proxy_.client().put("moveaxis", {{"Axis", std::to_string(axis)}, {"Rate", number_text(rate)}});
            proxy_.with_state([this, index](Proxy::State&) { axis_moving_[index] = true; });
            started_motion(ctx);
        });
        start_move_axis_cap(axis);
    }

    void pulse_guide(int direction, int duration) override {
        util::check_pulse_guide_arguments(kLimits, direction, duration);
        require_frame();
        run([this, direction, duration](util::OperationContext& ctx) {
            proxy_.client().put("pulseguide",
                                {{"Direction", std::to_string(direction)}, {"Duration", std::to_string(duration)}});
            write_frame(ctx, [](Frame& frame) { frame.is_pulse_guiding = set_to(true); });
        });
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinatesAsync");
        require_frame();
        run([this, ra, dec](util::OperationContext& ctx) {
            proxy_.client().put("slewtocoordinatesasync",
                                {{"RightAscension", number_text(ra)}, {"Declination", number_text(dec)}});
            started_motion(ctx);
            read_targets(ctx);
        });
    }

    void slew_to_target_async() override {
        require_frame();
        run([this](util::OperationContext& ctx) {
            proxy_.client().put("slewtotargetasync");
            started_motion(ctx);
        });
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        util::check_alt_az_arguments(kLimits, altitude, azimuth, "SlewToAltAzAsync");
        require_frame();
        run([this, altitude, azimuth](util::OperationContext& ctx) {
            proxy_.client().put("slewtoaltazasync",
                                {{"Altitude", number_text(altitude)}, {"Azimuth", number_text(azimuth)}});
            started_motion(ctx);
        });
    }

    // ITelescopeV4 deprecates the synchronous slews and an Alpaca mount should not implement them (CanSlew and
    // CanSlewAltAz are false). Each still judges its arguments and the connection first.
    void slew_to_coordinates(double ra, double dec) override {
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinates");
        refuse_synchronous_slew("SlewToCoordinates", "SlewToCoordinatesAsync");
    }
    void slew_to_target() override { refuse_synchronous_slew("SlewToTarget", "SlewToTargetAsync"); }
    void slew_to_alt_az(double altitude, double azimuth) override {
        util::check_alt_az_arguments(kLimits, altitude, azimuth, "SlewToAltAz");
        refuse_synchronous_slew("SlewToAltAz", "SlewToAltAzAsync");
    }

    // An asynchronous initiator on the upstream too: Slewing is written true at once; AtPark is the poll's.
    void park() override {
        require_frame();
        run([this](util::OperationContext& ctx) {
            proxy_.client().put("park");
            started_motion(ctx);
        });
    }

    void find_home() override {
        require_frame();
        run([this](util::OperationContext& ctx) {
            proxy_.client().put("findhome");
            started_motion(ctx);
        });
    }

    void unpark() override {
        require_frame();
        run([this](util::OperationContext& ctx) {
            proxy_.client().put("unpark");
            write_frame(ctx, [](Frame& frame) { frame.at_park = set_to(false); });
        });
    }

    void set_park() override {
        require_frame();
        run([this](util::OperationContext&) { proxy_.client().put("setpark"); });
    }

    // The upstream keeps the pointing model: no sync bound here. The synced position is written through.
    void sync_to_coordinates(double ra, double dec) override {
        util::check_sync_arguments(ra, dec, "SyncToCoordinates");
        require_frame();
        run([this, ra, dec](util::OperationContext& ctx) {
            proxy_.client().put("synctocoordinates",
                                {{"RightAscension", number_text(ra)}, {"Declination", number_text(dec)}});
            write_frame(ctx, [ra, dec](Frame& frame) {
                frame.right_ascension = set_to(ra);
                frame.declination = set_to(dec);
            });
            read_targets(ctx);
        });
    }

    void sync_to_target() override {
        require_frame();
        run([this](util::OperationContext&) { proxy_.client().put("synctotarget"); });
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        util::check_alt_az_arguments(kLimits, altitude, azimuth, "SyncToAltAz");
        require_frame();
        run([this, altitude, azimuth](util::OperationContext& ctx) {
            proxy_.client().put("synctoaltaz",
                                {{"Altitude", number_text(altitude)}, {"Azimuth", number_text(azimuth)}});
            write_frame(ctx, [altitude, azimuth](Frame& frame) {
                frame.altitude = set_to(altitude);
                frame.azimuth = set_to(azimuth);
            });
        });
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

    // Set while no command runs; a command body calls it first, before its first upstream call.
    void set_command_hook(std::function<void()> hook) { command_hook_ = std::move(hook); }

    // Set while no command runs; a caller that waits for an earlier command calls it first.
    void set_command_waiting_hook(std::function<void()> hook) { proxy_.set_command_waiting_hook(std::move(hook)); }

protected:
    // DeviceState's TimeStamp is when the served frame was measured, not the request time (decision 0010).
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        return proxy_.with_state([](const util::PolledState<Frame>& state) { return state.age(); });
    }

private:
    // The gate's state checks for MoveAxis: connected and the link fault. The upstream judges parked.
    struct StateChecks {
        const RemoteAlpacaTelescopeDriver& driver;
        void check_connected() const { driver.proxy_.require_connected(); }
        void check_not_parked(bool) const {}
        void check_no_fault() const { driver.require_frame(); }
    };

    static void validate_axis(int axis, const char* member) {
        if (axis < 0 || axis >= kAxes) {
            throw AlpacaException(std::string(member) + " axis must be 0, 1 or 2", AlpacaError::InvalidValue);
        }
    }

    void refuse_synchronous_slew(const char* member, const char* async_member) const {
        proxy_.require_connected();
        throw AlpacaException(std::string(member) +
                                  " is deprecated in ITelescopeV4 and not proxied (CanSlew is false); "
                                  "use " +
                                  async_member,
                              AlpacaError::NotImplemented);
    }

    // A capability: while disconnected the last Connect's answer, false before any; connected, the upstream's answer
    // or its error. One state-lock hold picks the answer, so a racing Disconnect never makes the read throw.
    bool can(Answer<bool> Settings::*member) const {
        return proxy_.with_state([this, member](const Proxy::State&) {
            const Answer<bool>& answer = settings_.*member;
            if (!proxy_.connected()) return answer.value.value_or(false);
            return answer.get();
        });
    }

    // A Connect read: NotConnected while disconnected, answered while faulted (static metadata).
    template <class Fn>
    std::invoke_result_t<Fn&, const Settings&> setting(Fn&& fn) const {
        return proxy_.with_connected([this, &fn](const Proxy::State&) { return fn(settings_); });
    }

    // A frame member (a stale frame served, a fault refused), or the upstream's error unchanged.
    template <class T>
    T served(Answer<T> Frame::*member) const {
        return proxy_.with_connected(
            [member](const Proxy::State& state) { return (state.served().value.*member).get(); });
    }

    // A command that needs the link: NotConnected, or refused while the link is faulted (decision 0009 rule 1).
    void require_frame() const {
        (void)proxy_.with_connected([](const Proxy::State& state) { return state.served().measured_at; });
    }

    void run(std::function<void(util::OperationContext&)> body) {
        proxy_.run_command([this, body = std::move(body)](util::OperationContext& ctx) {
            if (command_hook_) command_hook_();
            body(ctx);
        });
    }

    // A setting write: refused while faulted, forwarded, written through on success.
    template <class Apply>
    void write_setting(const char* member, RemoteParams params, Apply apply) {
        require_frame();
        run([this, member, params = std::move(params), apply = std::move(apply)](util::OperationContext& ctx) {
            proxy_.client().put(member, params);
            store(ctx, apply);
        });
    }

    // Write-through (decision 0010 rule 2): a superseded body writes nothing (decision 0006 rule 6), nor one whose
    // caller passed its check before a Disconnect (decision 0010 rule 8, #2446).
    template <class Apply>
    void write_through(util::OperationContext& ctx, Apply apply) {
        proxy_.with_state([this, &ctx, &apply](Proxy::State& state) {
            if (ctx.stop_reason() == util::StopReason::Superseded || !get_connected()) return;
            apply(state);
        });
    }

    template <class Apply>
    void write_frame(util::OperationContext& ctx, Apply apply) {
        write_through(ctx, [&apply](Proxy::State& state) { state.write(apply); });
    }

    template <class Apply>
    void store(util::OperationContext& ctx, Apply apply) {
        write_through(ctx, [this, &apply](Proxy::State&) { apply(settings_); });
    }

    // A GoTo, Park, FindHome, SideOfPier flip or MoveAxis the upstream took: this server owns the motion (for the
    // relink stop) until a poll measured after it reads Slewing false, the kept cap outcome ends, and Slewing reads
    // true at once. Only the command writes Slewing true; only the poll (or the upstream after a stop) writes it false.
    void started_motion(util::OperationContext& ctx) {
        proxy_.with_state([this](Proxy::State&) {
            ++motion_seq_;
            driver_motion_ = true;
            motion_error_.clear();
        });
        write_frame(ctx, [](Frame& frame) { frame.slewing = set_to(true); });
    }

    // The targets the upstream set from a coordinate slew or sync, read back rather than computed. A read with no
    // usable reply leaves them as they were and does not fail the command the upstream took.
    void read_targets(util::OperationContext& ctx) {
        RemoteAlpacaClient& client = proxy_.client();
        try {
            const Answer<double> ra = answer_of([&] { return client.get_double("targetrightascension"); });
            const Answer<double> dec = answer_of([&] { return client.get_double("targetdeclination"); });
            store(ctx, [&ra, &dec](Settings& s) {
                s.target_right_ascension = ra;
                s.target_declination = dec;
            });
        } catch (const RemoteTransportError&) {  // NOLINT(bugprone-empty-catch): the next Connect reads them
        }
    }

    // At the end of a session (a Connect or a Disconnect), with no lock held but transition_mutex_: a link lost while
    // this server's motion ran owes the relink stop (decision 0009, alternative E: a Disconnect leaves it owed).
    void end_motion_session() {
        const bool lost = proxy_.lost();  // takes the state lock itself
        proxy_.with_state([this, lost](Proxy::State&) {
            if (lost) relink_stop_.note_link_lost(driver_motion_);
            driver_motion_ = false;
            axis_moving_ = {false, false, false};
            motion_error_.clear();
        });
    }

    // ── The MoveAxis cap (#2651) ─────────────────────────────────────────────────────────────────────────────────
    // A non-zero MoveAxis starts a body in its axis's own slot: it waits through its context until the 60 s window
    // has passed, then runs util::run_motion_deadline()'s verified stop of that axis and keeps the outcome for
    // Slewing. A renewal supersedes it, MoveAxis(axis, 0) and AbortSlew cancel it, a Connect or Disconnect joins it.
    // Its stop runs as a command (caller order), so it never reaches the upstream after a later client command.

    void stop_move_axis_caps() {
        for (auto& cap : move_axis_cap_) cap->cancel_all_and_join();
    }

    // In the MoveAxis caller after its command, with no lock held (decision 0006 rule 4). A start the slot refuses
    // leaves the move with no cap, so the axis is stopped and the call fails (fail closed): InvalidOperation ("try
    // again") when a Connect or Disconnect is joining the caps (issue #1713), DriverException otherwise.
    void start_move_axis_cap(int axis) {
        util::HoldWindow window;
        window.arm(proxy_.clock().now(), util::kMaxMoveAxisDuration);
        try {
            move_axis_cap_[static_cast<std::size_t>(axis)]->start(
                [this, axis, window](util::OperationContext& ctx) { run_move_axis_cap(axis, window, ctx); });
        } catch (const std::exception& e) {
            const auto* refusal = dynamic_cast<const AlpacaException*>(&e);
            const int code = refusal != nullptr && refusal->error_code() == AlpacaError::InvalidOperation
                                 ? AlpacaError::InvalidOperation
                                 : AlpacaError::DriverException;
            if (code == AlpacaError::InvalidOperation) {
                // Decision 0007 rule 9: a racing Disconnect is the client's doing, not a WARNING.
                ALPACA_LOG_INFO(detail::kComponent, "MoveAxis cap could not start: " + std::string(e.what()));
            } else {
                ALPACA_LOG_WARN(detail::kComponent, "MoveAxis cap could not start: " + std::string(e.what()));
            }
            try {
                proxy_.client().put("moveaxis", {{"Axis", std::to_string(axis)}, {"Rate", "0"}});
            } catch (const std::exception& stop_error) {
                ALPACA_LOG_WARN(detail::kComponent,
                                "MoveAxis stop after the refused cap failed: " + std::string(stop_error.what()));
            }
            throw AlpacaException(
                std::string("MoveAxis: the cap on the move could not start, so the axis was stopped: ") + e.what(),
                code);
        }
    }

    void run_move_axis_cap(int axis, util::HoldWindow window, util::OperationContext& ctx) {
        const std::string expired = "MoveAxis ran for " + std::to_string(util::kMaxMoveAxisDuration.count()) +
                                    " s with no new MoveAxis; the axis was stopped";
        try {
            util::run_motion_deadline(ctx, proxy_.clock(), window, cap_hooks(axis, ctx), expired);
        } catch (const AlpacaException& e) {
            const std::string kept = e.what();
            proxy_.with_state([this, axis, &ctx, &kept](Proxy::State&) {
                if (ctx.stop_reason() != util::StopReason::None || !get_connected()) return;
                axis_moving_[static_cast<std::size_t>(axis)] = false;
                motion_error_ = kept;
            });
            ALPACA_LOG_INFO(detail::kComponent, "Upstream telescope: " + kept);
        }
    }

    bool axis_moving(int axis) {
        return proxy_.with_state([this, axis](Proxy::State&) { return axis_moving_[static_cast<std::size_t>(axis)]; });
    }

    // The verified stop of one axis. The stop is MoveAxis(axis, 0) in the command slot; the reads are live GETs in
    // the cap's own thread (the poll's frame may be a poll period old, and the guard needs reads made after its
    // stop). Slewing is the whole mount's, so it reads as stopped while the other axis runs this server's MoveAxis,
    // and the other axis's coordinate is pinned then.
    util::StopVerifyHooks cap_hooks(int axis, util::OperationContext& cap) {
        util::StopVerifyHooks hooks;
        const int other = axis == 0 ? 1 : 0;
        hooks.send_stop = [this, axis, &cap] {
            proxy_.run_command([this, axis, &cap](util::OperationContext&) {
                // A cap a renewal, a stop, AbortSlew or a disconnect ended sends nothing (decision 0006 rule 6).
                if (cap.stop_reason() != util::StopReason::None || !axis_moving(axis)) return;
                proxy_.client().put("moveaxis", {{"Axis", std::to_string(axis)}, {"Rate", "0"}});
            });
        };
        hooks.read_status = [this, other]() -> std::optional<util::MotionStatusReading> {
            if (!proxy_.connected()) return std::nullopt;
            if (axis_moving(other)) return util::MotionStatusReading{false, proxy_.clock().now()};
            const bool moving = proxy_.client().get_bool("slewing");
            return util::MotionStatusReading{moving, proxy_.clock().now()};
        };
        hooks.read_position = [this, axis, other]() -> std::optional<util::PositionReading> {
            if (!proxy_.connected()) return std::nullopt;
            RemoteAlpacaClient& client = proxy_.client();
            util::MountPosition position{client.get_double("rightascension"), client.get_double("declination")};
            if (axis_moving(other)) {
                if (axis == 0) position.dec_degrees = 0.0;
                if (axis == 1) position.ra_hours = 0.0;
            }
            return util::PositionReading{position, proxy_.clock().now()};
        };
        return hooks;
    }

    // ── Connect ─────────────────────────────────────────────────────────────────────────────────────────────────

    // At Connect, with no lock held, after the upstream's identity: the Settings, then the relink stop a lost link
    // owes when the mount still slews. Returns what to store under the state lock with the identity.
    std::function<void()> read_settings() {
        const int version = proxy_.upstream_interface_version();
        if (version < kFirstSupportedVersion) {
            const RemoteDeviceConfig& config = proxy_.config();
            throw AlpacaException("Upstream " + config.host + ":" + std::to_string(config.port) + " device " +
                                      std::to_string(config.upstream_device_number) + " is an ITelescopeV" +
                                      std::to_string(version) +
                                      "; a Remote Alpaca telescope needs ITelescopeV3 or later",
                                  AlpacaError::DriverException);
        }
        RemoteAlpacaClient& c = proxy_.client();
        Settings& s = read_settings_;
        const auto flag = [&c](const char* member) { return answer_of([&] { return c.get_bool(member); }); };
        const auto real = [&c](const char* member) { return answer_of([&] { return c.get_double(member); }); };
        const auto whole = [&c](const char* member) { return answer_of([&] { return c.get_int(member); }); };
        s.can_find_home = flag("canfindhome");
        s.can_park = flag("canpark");
        s.can_pulse_guide = flag("canpulseguide");
        s.can_set_declination_rate = flag("cansetdeclinationrate");
        s.can_set_guide_rates = flag("cansetguiderates");
        s.can_set_park = flag("cansetpark");
        s.can_set_pier_side = flag("cansetpierside");
        s.can_set_right_ascension_rate = flag("cansetrightascensionrate");
        s.can_set_tracking = flag("cansettracking");
        s.can_slew_alt_az_async = flag("canslewaltazasync");
        s.can_slew_async = flag("canslewasync");
        s.can_sync = flag("cansync");
        s.can_sync_alt_az = flag("cansyncaltaz");
        s.can_unpark = flag("canunpark");
        for (int axis = 0; axis < kAxes; ++axis) {
            const RemoteParams params{{"Axis", std::to_string(axis)}};
            const auto index = static_cast<std::size_t>(axis);
            s.can_move_axis[index] = answer_of([&] { return c.get_bool("canmoveaxis", params); });
            s.axis_rates[index] = answer_of([&] { return c.get_rate_list("axisrates", params); });
        }
        s.alignment_mode = whole("alignmentmode");
        s.equatorial_system = whole("equatorialsystem");
        s.aperture_area = real("aperturearea");
        s.aperture_diameter = real("aperturediameter");
        s.focal_length = real("focallength");
        s.does_refraction = flag("doesrefraction");
        s.slew_settle_time = whole("slewsettletime");
        s.site_latitude = real("sitelatitude");
        s.site_longitude = real("sitelongitude");
        s.site_elevation = real("siteelevation");
        s.tracking_rate = whole("trackingrate");
        s.tracking_rates = answer_of([&] { return c.get_int_list("trackingrates"); });
        s.right_ascension_rate = real("rightascensionrate");
        s.declination_rate = real("declinationrate");
        s.guide_rate_right_ascension = real("guideraterightascension");
        s.guide_rate_declination = real("guideratedeclination");
        s.target_right_ascension = real("targetrightascension");
        s.target_declination = real("targetdeclination");
        const bool stop_sent = send_relink_stop();
        return [this, stop_sent] {
            settings_ = read_settings_;
            if (stop_sent) relink_stop_.stop_sent();
        };
    }

    // The relink stop (#2405, #2396 AB-9): a link lost while this server's GoTo or MoveAxis ran left the mount moving
    // with nobody in control. When the upstream still reports Slewing, AbortSlew and MoveAxis(axis, 0) on every
    // movable axis go out before any client command. An upstream that answers them with an error has heard the stop;
    // a call with no reply fails the Connect, and the stop stays owed for the next one. True when nothing is owed now.
    bool send_relink_stop() {
        if (!proxy_.with_state([this](const Proxy::State&) { return relink_stop_.stop_owed(); })) return false;
        RemoteAlpacaClient& client = proxy_.client();
        if (!client.get_bool("slewing")) {
            ALPACA_LOG_INFO(detail::kComponent, "Relink: the upstream telescope is still, so no stop is sent");
            return true;
        }
        const auto heard = [](const std::function<void()>& call) {
            try {
                call();
            } catch (const RemoteTransportError&) {
                throw;
            } catch (const AlpacaException& e) {
                ALPACA_LOG_INFO(detail::kComponent, std::string("Relink stop answered: ") + e.what());
            }
        };
        ALPACA_LOG_WARN(detail::kComponent,
                        "Relink: the link was lost while this server's motion ran and the upstream telescope still "
                        "slews; sending AbortSlew and MoveAxis(axis, 0)");
        heard([&] { client.put("abortslew"); });
        for (int axis = 0; axis < kAxes; ++axis) {
            const auto& can_move = read_settings_.can_move_axis[static_cast<std::size_t>(axis)];
            if (!can_move.value.value_or(false)) continue;
            heard([&] { client.put("moveaxis", {{"Axis", std::to_string(axis)}, {"Rate", "0"}}); });
        }
        return true;
    }

    // One measurement of the upstream, with no lock held (decision 0016 rule 1): twelve GETs. Throws
    // RemoteTransportError when a call gets no usable reply; an upstream error is kept in the frame. A sample begun
    // after this server's last motion command that reads Slewing false ends this server's ownership of the motion.
    Frame sample() {
        RemoteAlpacaClient& c = proxy_.client();
        const std::uint64_t seq = proxy_.with_state([this](const Proxy::State&) { return motion_seq_; });
        Frame frame;
        frame.right_ascension = answer_of([&] { return c.get_double("rightascension"); });
        frame.declination = answer_of([&] { return c.get_double("declination"); });
        frame.altitude = answer_of([&] { return c.get_double("altitude"); });
        frame.azimuth = answer_of([&] { return c.get_double("azimuth"); });
        frame.sidereal_time = answer_of([&] { return c.get_double("siderealtime"); });
        const Answer<std::string> utc = answer_of([&] { return c.get_string("utcdate"); });
        if (!utc.value) {
            frame.utc_date = {std::nullopt, utc.error_number, utc.error_message};
        } else if (const auto parsed = parse_utc(*utc.value)) {
            frame.utc_date = set_to(*parsed);
        } else {
            frame.utc_date = {std::nullopt, AlpacaError::DriverException,
                              "Upstream UTCDate '" + *utc.value + "' is not an ISO 8601 UTC time"};
        }
        frame.slewing = answer_of([&] { return c.get_bool("slewing"); });
        frame.tracking = answer_of([&] { return c.get_bool("tracking"); });
        frame.at_park = answer_of([&] { return c.get_bool("atpark"); });
        frame.at_home = answer_of([&] { return c.get_bool("athome"); });
        frame.side_of_pier = answer_of([&] { return c.get_int("sideofpier"); });
        frame.is_pulse_guiding = answer_of([&] { return c.get_bool("ispulseguiding"); });
        if (frame.slewing.value && !*frame.slewing.value) {
            proxy_.with_state([this, seq](Proxy::State&) {
                if (motion_seq_ == seq) driver_motion_ = false;
            });
        }
        return frame;
    }

    const int device_number_;

    std::function<void()> command_hook_;  // test seam: set_command_hook_for_testing()

    // Under the proxy's state lock (with_connected(), with_state(), the Connect's store).
    Settings settings_;
    // This server's motion: a GoTo, Park, FindHome, flip or MoveAxis the upstream took and no later poll saw end;
    // motion_seq_ counts them, so a sample begun before the latest one cannot end it.
    bool driver_motion_ = false;
    std::uint64_t motion_seq_ = 0;
    std::array<bool, kAxes> axis_moving_{false, false, false};  // this server's MoveAxis per axis
    std::string motion_error_;                                  // a MoveAxis cap's outcome, raised by Slewing
    util::RelinkMotionStop relink_stop_;
    // The Connect's reads before the store, under transition_mutex_ (only the connect path touches it).
    Settings read_settings_;

    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    Proxy proxy_;
    // Last, so they are destroyed (their bodies joined) before the members the bodies touch die.
    // One generation per slot: a start on one axis's slot must not supersede another axis's cap.
    std::array<util::OperationGeneration, kAxes> cap_generation_;
    std::array<std::unique_ptr<util::AsyncOperation>, kAxes> move_axis_cap_;
};

}  // namespace

std::unique_ptr<TelescopeDriver> create_remotealpaca_telescope(int device_number, const RemoteDeviceConfig& config,
                                                               ServerAddressSource self) {
    return create_remotealpaca_telescope(device_number, config,
                                         util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"), std::move(self),
                                         util::default_task_clock());
}

std::unique_ptr<TelescopeDriver> create_remotealpaca_telescope(int device_number, const RemoteDeviceConfig& config,
                                                               std::shared_ptr<util::HttpClient> http,
                                                               ServerAddressSource self, util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaTelescopeDriver>(device_number, config, std::move(http), std::move(self),
                                                         clock);
}

void set_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaTelescopeDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

void set_command_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaTelescopeDriver*>(&driver)) {
        remote->set_command_hook(std::move(hook));
    }
}

void set_command_waiting_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* remote = dynamic_cast<RemoteAlpacaTelescopeDriver*>(&driver)) {
        remote->set_command_waiting_hook(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::remotealpaca

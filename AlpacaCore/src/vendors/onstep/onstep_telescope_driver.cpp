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

#include <alpacacore/async_connectable.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>

namespace alpacacore::vendor::onstep {

namespace {

// OnStep accepts a true arbitrary MoveAxis rate via :RA[n.n]#/:RE[n.n]#
// (real degrees/second, confirmed against Command.ino/Guide.ino) — this is
// a continuous range, not a small set of discrete presets. See
// OnStepProtocolWrapper::move_axis_start().
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
constexpr auto kPositionCacheTtl = std::chrono::seconds(2);
constexpr auto kStatusCacheTtl = std::chrono::seconds(2);
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
constexpr auto kSlewForceGrace = std::chrono::seconds(2);
// Informational only — OnStep does not expose a guide-rate query command in
// this driver, and CanSetGuideRates is false, so this value is never sent to
// or read from the mount; it merely satisfies the ASCOM GuideRate property.
constexpr double kInformationalGuideRateDegPerSec = (15.0411 / 3600.0) * 0.5;

double wrap_hours(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }
    return wrapped;
}

double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time, double longitude_degrees) {
    using namespace std::chrono;
    double days_since_epoch =
        static_cast<double>(duration_cast<seconds>(utc_time.time_since_epoch()).count()) / 86400.0;
    double jd = 2440587.5 + days_since_epoch;
    double t = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - (t * t * t) / 38710000.0;
    double lst_degrees = gmst + longitude_degrees;
    lst_degrees = std::fmod(lst_degrees, 360.0);
    if (lst_degrees < 0.0) {
        lst_degrees += 360.0;
    }
    return lst_degrees / 15.0;
}

double shortest_ra_delta_hours(double a, double b) {
    double delta = a - b;
    while (delta > 12.0) delta -= 24.0;
    while (delta < -12.0) delta += 24.0;
    return delta;
}

// Standard spherical-trig horizontal-to-equatorial transform (azimuth
// measured from North through East). Used to implement SlewToAltAz/
// SlewToAltAzAsync via an ordinary equatorial slew — OnStep/LX200 has no
// native Alt/Az goto command — mirroring the approach other equatorial-only
// drivers in this project use for the same capability.
void alt_az_to_ra_dec(double alt_deg, double az_deg, double lat_deg, double lst_hours, double& ra_hours_out,
                      double& dec_deg_out) {
    const double lat_rad = lat_deg * std::numbers::pi / 180.0;
    const double alt_rad = alt_deg * std::numbers::pi / 180.0;
    const double az_rad = az_deg * std::numbers::pi / 180.0;

    double sin_dec = std::sin(alt_rad) * std::sin(lat_rad) + std::cos(alt_rad) * std::cos(lat_rad) * std::cos(az_rad);
    sin_dec = std::clamp(sin_dec, -1.0, 1.0);
    const double dec_rad = std::asin(sin_dec);

    double cos_ha = (std::sin(alt_rad) - std::sin(lat_rad) * sin_dec) / (std::cos(lat_rad) * std::cos(dec_rad));
    cos_ha = std::clamp(cos_ha, -1.0, 1.0);
    double ha_hours = std::acos(cos_ha) * 180.0 / std::numbers::pi / 15.0;
    if (std::sin(az_rad) > 0.0) {
        ha_hours = 24.0 - ha_hours;
    }

    ra_hours_out = wrap_hours(lst_hours - ha_hours);
    dec_deg_out = dec_rad * 180.0 / std::numbers::pi;
}

void validate_ra_dec(double ra, double dec, const char* context) {
    if (ra < 0.0 || ra >= 24.0) {
        throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
    }
    if (dec < -90.0 || dec > 90.0) {
        throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
    }
}

}  // namespace

class OnStepTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    OnStepTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                          std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                          std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect)
        : AsyncConnectable("OnStep"),
          device_number_(device_number),
          connection_info_(connection_info),
          connected_(false),
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)) {}

    ~OnStepTelescopeDriver() override {
        // MUST be first, before members the async connection task touches
        // are destroyed (AsyncConnectable base contract). This driver spawns
        // no other worker threads: OnStep's LX200 text commands (:MS#, guide
        // pulses, park/unpark, move-axis) are all quick round trips issued
        // synchronously, so there is nothing else to cancel/join here.
        shutdown_connection();
        if (connected_) {
            try {
                // OnStepTelescopeDriver is the most-derived class (no further
                // override exists to miss), so the analyzer's "bypasses
                // virtual dispatch" warning doesn't apply here.
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (...) {            // NOLINT(bugprone-empty-catch)
                // Best-effort teardown during destruction.
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "OnStep Telescope"; }

    DeviceType get_device_type() const override { return DeviceType::Telescope; }

    std::string get_unique_id() const override { return "OnStep_" + std::to_string(device_number_); }

    std::string get_description() const override { return "OnStep LX200-Protocol Telescope Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore OnStep Driver v0.1"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Product name + firmware version captured at connect; web-UI only.
    // Guarded by its OWN narrow mutex, NOT the coarse mutex_ that
    // set_connected() holds across the multi-second connect, so a
    // configureddevices poll never blocks on the mount connection.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lock(mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during
        // an in-flight connect looks idempotent (both sides see
        // disconnected) and would be silently dropped without the record; a
        // connect must honor a newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }

        auto& protocol = OnStepProtocolWrapper::instance();
        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to OnStep mount");
            }
            connected_ = true;
            status_cache_valid_ = false;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            site_info_valid_ = false;
            last_utc_valid_ = false;
            target_set_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            pulse_guiding_active_ = false;
            tracking_before_slew_ = true;
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();

            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                try {
                    std::string product = protocol.get_product_name();
                    std::string version = protocol.get_version_number();
                    firmware_cache_ = product + (version.empty() ? "" : (" " + version));
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    firmware_cache_.clear();
                }
            }

            // site_info_valid_ must only be set from here when BOTH pending
            // values were provided AND both writes succeeded -- it's a single
            // flag covering both fields together (see the atomic :Gt#/:Gg#
            // fetch in ensure_site_info_cached_locked()). Setting it after
            // only one field actually landed (the mount can reject a site
            // update depending on alignment state) would pin the other field
            // at its stale/default 0.0 while callers trust it as valid --
            // e.g. compute_alt_az_target_locked() feeding a wrong latitude
            // into the Alt/Az->RA/Dec transform for SlewToAltAz. Leaving the
            // flag false on any partial failure just falls through to
            // ensure_site_info_cached_locked()'s atomic re-fetch on next
            // access instead.
            bool latitude_pushed = false;
            bool longitude_pushed = false;
            if (pending_site_latitude_.has_value()) {
                try {
                    protocol.set_latitude(pending_site_latitude_.value());
                    site_latitude_cached_ = pending_site_latitude_.value();
                    latitude_pushed = true;
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // TODO: Confirm OnStep accepts site updates in every alignment state.
                }
            }
            if (pending_site_longitude_.has_value()) {
                try {
                    protocol.set_longitude(pending_site_longitude_.value());
                    site_longitude_cached_ = pending_site_longitude_.value();
                    longitude_pushed = true;
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // TODO: see above.
                }
            }
            if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value() && latitude_pushed &&
                longitude_pushed) {
                site_info_valid_ = true;
            }
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            if (sync_time_on_connect_) {
                try {
                    set_utc_date_locked(std::chrono::system_clock::now());
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                }
            }
            try {
                // Continuous-motion (:Mn#/:Ms#/:Me#/:Mw#) commands run at
                // whatever slew rate the firmware currently has selected;
                // pin it to max on connect so MoveAxis completes promptly.
                protocol.select_max_slew_rate();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }

            // Warm caches so the first property reads stay within ConformU's
            // fast-response targets. NOTE: this does not help the very first
            // DeviceState call — ConformU's Connect()/Connected preamble
            // issues a fast connect/disconnect/reconnect dance and calls
            // DeviceState immediately after, faster than this warm-up can
            // complete (measured: DeviceState's ~0.65s live-read cost is
            // identical whether or not this block runs). Kept anyway because
            // every OTHER first-property-read benefits from it.
            try {
                refresh_status_cache_locked(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                Position pos = protocol.get_position();
                cached_ra_hours_ = pos.ra_hours;
                cached_dec_degrees_ = pos.dec_degrees;
                equatorial_cache_valid_ = true;
                last_equatorial_update_ = std::chrono::steady_clock::now();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                ensure_site_info_cached_locked();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                AltAz altaz = protocol.get_alt_az();
                cached_alt_degrees_ = altaz.altitude_degrees;
                cached_az_degrees_ = altaz.azimuth_degrees;
                altaz_cache_valid_ = true;
                last_altaz_update_ = std::chrono::steady_clock::now();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                refresh_status_cache_locked(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        } else {
            protocol.disconnect();
            connected_ = false;
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_.clear();
            }
            target_set_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            pulse_guiding_active_ = false;
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name));
    }

    bool can_action(std::string_view action_name) const override {
        (void)action_name;
        return false;
    }

    std::string command_blind(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        OnStepProtocolWrapper::instance().send_command_blind(std::string(command));
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = OnStepProtocolWrapper::instance();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return response == "1";
        }
        protocol.send_command_blind(std::string(command));
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return OnStepProtocolWrapper::instance().send_command(std::string(command));
    }

    AlignmentMode get_alignment_mode() const override { return AlignmentMode::GermanPolar; }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_altaz_cache_locked();
        return cached_alt_degrees_;
    }

    double get_aperture_diameter() const override { return aperture_diameter_m_; }

    void set_aperture_diameter(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
        }
        aperture_diameter_m_ = meters;
        aperture_area_m2_ = meters > 0.0 ? (meters / 2.0) * (meters / 2.0) * std::numbers::pi : 0.0;
    }

    double get_aperture_area() const override { return aperture_area_m2_; }

    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked(false);
        return cached_status_.is_at_home;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked(false);
        return cached_status_.is_parked;
    }

    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_altaz_cache_locked();
        return cached_az_degrees_;
    }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (pulse_guiding_active_ && std::chrono::steady_clock::now() >= pulse_guide_end_time_) {
            pulse_guiding_active_ = false;
        }
        return pulse_guiding_active_;
    }

    // OnStep's custom tracking-rate commands were not confirmed against real
    // firmware during this implementation (see AGENTS.md OnStep notes) —
    // default both to false rather than advertise a setter that would
    // silently no-op.
    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return false; }
    bool get_can_set_park() const override { return true; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_tracking() const override { return true; }
    bool get_can_slew_alt_az() const override { return true; }
    bool get_can_slew_alt_az_async() const override { return true; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return true; }
    bool get_can_slew_async() const override { return true; }
    bool get_can_sync() const override { return true; }
    bool get_can_unpark() const override { return true; }

    double get_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_equatorial_cache_locked();
        return cached_dec_degrees_;
    }

    double get_declination_rate() const override { return 0.0; }

    void set_declination_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Declination rate not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked(false);
        return cached_status_.is_tracking;
    }

    void set_tracking(bool tracking) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (tracking) {
            check_not_parked_locked("Tracking");
        }
        auto& protocol = OnStepProtocolWrapper::instance();
        if (tracking) {
            protocol.start_tracking();
        } else {
            protocol.stop_tracking();
        }
        cached_status_.is_tracking = tracking;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
    }

    double get_focal_length() const override { return focal_length_m_; }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        return {kInformationalGuideRateDegPerSec, kInformationalGuideRateDegPerSec};
    }

    void set_guide_rate(const GuideRate& rate) override {
        (void)rate;
        throw AlpacaException("Guide rate is fixed by OnStep firmware", AlpacaError::PropertyNotImplemented);
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_equatorial_cache_locked();
        return cached_ra_hours_;
    }

    double get_right_ascension_rate() const override { return 0.0; }

    void set_right_ascension_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Right ascension rate not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // Always compute from hour angle — same convention as the project's
        // other GEM drivers (SynScan/iOptron/Celestron), and the only source
        // that satisfies ASCOM's SideOfPier contract (a pointing state
        // defined purely by hour-angle sign). :GU#'s E/W flag (see
        // AGENTS.md OnStep notes) was tried as the primary source instead,
        // but ConformU proved it reports the mount's PHYSICAL pier
        // orientation — which only changes on an actual mechanical meridian
        // flip — not the ASCOM pointing state: "Reported SideofPier at HA
        // -9, +9: EE" (constant) instead of the required "WE" (flips with
        // HA sign), confirmed against real hardware. Do not reintroduce a
        // firmware-flag-preferred branch here without re-validating against
        // ConformU's SideofPier/DestinationSideofPier checks.
        //
        // Deliberately does NOT call refresh_equatorial_cache_locked() (which
        // enforces kPositionCacheTtl's normal 2s freshness window): a live
        // :GRa#/:GDe# round trip (~0.13s each, ~0.26s+ together on this
        // serial link) landing inside this call is enough to blow ConformU's
        // 0.1s FAST target — confirmed against real hardware, where this
        // showed up as an occasional "SideOfPier Read ... OUTSIDE FAST
        // RESPONSE TIME TARGET" once the TTL happened to have just lapsed.
        // Hour-angle SIGN (the only thing this needs) is insensitive to a
        // couple of seconds of RA staleness — sidereal drift over that
        // window is a few arcseconds, utterly negligible next to the ±6h
        // scale this decision operates on — so reuse whatever is already
        // cached and only force a live fetch if a position has never been
        // read at all (e.g. immediately after connect).
        ensure_site_info_cached_locked();
        if (!equatorial_cache_valid_) {
            refresh_equatorial_cache_locked();
        }
        const double lst = compute_local_sidereal_time_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = shortest_ra_delta_hours(lst, cached_ra_hours_);
        return hour_angle >= 0.0 ? 0 : 1;
    }

    void set_side_of_pier(int side) override {
        (void)side;
        throw AlpacaException("Pier side is not settable", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        (void)dec;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_site_info_cached_locked();
        const double lst = compute_local_sidereal_time_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = shortest_ra_delta_hours(lst, ra);
        return hour_angle >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override { return EquatorialSystem::Topocentric; }

    bool get_does_refraction() const override { return does_refraction_; }

    void set_does_refraction(bool does_refraction) override { does_refraction_ = does_refraction; }

    int get_slew_settle_time() const override { return slew_settle_time_seconds_; }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_site_info_cached_locked();
        return compute_local_sidereal_time_hours(current_utc_time_locked(), site_longitude_cached_);
    }

    double get_site_elevation() const override { return site_elevation_m_; }

    void set_site_elevation(double elevation) override {
        if (elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters", AlpacaError::InvalidValue);
        }
        // Client-side only: the LX200/OnStep protocol has no elevation command.
        site_elevation_m_ = elevation;
    }

    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_site_info_cached_locked();
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        if (latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // Populate longitude (and mark the pair valid) from the mount BEFORE
        // overwriting just latitude below -- otherwise, if longitude was
        // never cached yet, unconditionally setting site_info_valid_ = true
        // here would silently promote a stale/default 0.0 longitude to
        // "valid" alongside the freshly-set latitude. No-op (single flag
        // check) once the pair is already cached.
        ensure_site_info_cached_locked();
        OnStepProtocolWrapper::instance().set_latitude(latitude);
        site_latitude_cached_ = latitude;
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_site_info_cached_locked();
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        if (longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // See set_site_latitude(): populate latitude from the mount first so
        // marking the pair valid below never promotes a stale/default
        // latitude alongside the freshly-set longitude.
        ensure_site_info_cached_locked();
        OnStepProtocolWrapper::instance().set_longitude(longitude);
        site_longitude_cached_ = longitude;
    }

    bool get_slewing() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return get_slewing_locked();
    }

    double get_target_declination() const override {
        if (!target_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        if (dec < -90.0 || dec > 90.0) {
            throw AlpacaException("TargetDeclination must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
        }
        target_dec_degrees_ = dec;
        target_set_ = true;
    }

    double get_target_right_ascension() const override {
        if (!target_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        if (ra < 0.0 || ra >= 24.0) {
            throw AlpacaException("TargetRightAscension must be in range 0 to <24 hours", AlpacaError::InvalidValue);
        }
        target_ra_hours_ = ra;
        target_set_ = true;
    }

    int get_tracking_rate() const override { return 0; }

    void set_tracking_rate(int rate) override {
        if (rate != 0) {
            throw AlpacaException("Only sidereal tracking rate is supported", AlpacaError::PropertyNotImplemented);
        }
    }

    std::vector<int> get_tracking_rates() const override { return {0}; }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!last_utc_valid_) {
            try {
                TimeInfo info = OnStepProtocolWrapper::instance().get_time();
                using namespace std::chrono;
                int year = 2000 + info.year;
                sys_days date =
                    sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(info.month)} /
                             std::chrono::day{static_cast<unsigned>(info.day)}};
                auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
                auto offset = duration_cast<system_clock::duration>(
                    std::chrono::duration<double, std::ratio<3600>>(info.utc_offset_hours));
                last_utc_set_ = local_time - offset;
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                last_utc_set_ = std::chrono::system_clock::now();
            }
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
        }
        return current_utc_time_locked();
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        set_utc_date_locked(utc);
    }

    // find_home()/park() are fire-and-forget, matching iOptron: send the
    // command and return immediately rather than blocking until the mount
    // physically finishes. ConformU classifies a Park/FindHome call that
    // blocks for the whole physical operation under the STANDARD (1.0s)
    // timing target and fails it outright; a call that returns immediately
    // and lets the client poll AtHome/AtPark afterward is classified
    // EXTENDED (600s) instead — confirmed against this project's iOptron
    // ConformU logs, where the real (tens-of-seconds) physical park/home
    // is entirely absorbed by the client's post-call AtPark/AtHome polling.
    void find_home() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("FindHome");
        refresh_status_cache_locked(true);
        if (cached_status_.is_at_home) {
            return;
        }
        OnStepProtocolWrapper::instance().find_home();
        cached_status_.is_at_home = false;
        cached_status_.is_slewing = true;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
    }

    void park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked(true);
        if (cached_status_.is_parked) {
            return;  // ASCOM: Park() on an already-parked mount is a no-op.
        }
        OnStepProtocolWrapper::instance().park();
        cached_status_.is_parked = false;
        cached_status_.is_slewing = true;
        cached_status_.is_at_home = false;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
    }

    void pulse_guide(int direction, int duration) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("PulseGuide");
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }
        if (duration < 0) {
            throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
        }
        // Hardware pulse guide: the mount times the pulse internally
        // (:Mgn####/:Mgs####/:Mge####/:Mgw####), so unlike a software-timed
        // implementation there is no background stop thread to manage here —
        // is_pulse_guiding() just compares against the known end time.
        // OnStepProtocolWrapper::pulse_guide() silently clamps duration_ms to
        // [0, 9999] to fit the wire command's fixed 4-digit field; clamp the
        // same way here so the cached end time matches what the mount
        // actually pulses for, rather than IsPulseGuiding over-reporting for
        // whatever a caller-supplied duration beyond 9999ms overshot by.
        const int clamped_duration = std::clamp(duration, 0, 9999);
        OnStepProtocolWrapper::instance().pulse_guide(direction, duration);
        pulse_guiding_active_ = true;
        pulse_guide_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(clamped_duration);
        equatorial_cache_valid_ = false;
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        OnStepProtocolWrapper::instance().set_park();
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinates");
        start_slew_locked(ra, dec, "SlewToCoordinates");
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinatesAsync");
        start_slew_locked(ra, dec, "SlewToCoordinatesAsync");
    }

    void slew_to_target() override {
        if (!target_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        slew_to_coordinates(target_ra_hours_, target_dec_degrees_);
    }

    void slew_to_target_async() override {
        if (!target_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        slew_to_coordinates_async(target_ra_hours_, target_dec_degrees_);
    }

    void sync_to_coordinates(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        validate_ra_dec(ra, dec, "SyncToCoordinates");
        auto& protocol = OnStepProtocolWrapper::instance();
        // Use the mount's own sync command — do NOT maintain a driver-level
        // offset, which would cause coordinate divergence during subsequent
        // slews (project-wide rule; see AGENTS.md iOptron/Celestron notes).
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        protocol.sync_to_target();
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        cached_ra_hours_ = ra;
        cached_dec_degrees_ = dec;
        equatorial_cache_valid_ = true;
        last_equatorial_update_ = std::chrono::steady_clock::now();
    }

    void sync_to_target() override {
        if (!target_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        sync_to_coordinates(target_ra_hours_, target_dec_degrees_);
    }

    void unpark() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        OnStepProtocolWrapper::instance().unpark();
        cached_status_.is_parked = false;
        status_cache_valid_ = true;
        last_status_update_ = std::chrono::steady_clock::now();
    }

    bool get_can_move_axis(int axis) const override { return axis == 0 || axis == 1; }

    void move_axis(int axis, double rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("MoveAxis");
        if (axis != 0 && axis != 1) {
            throw AlpacaException("MoveAxis axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        if (std::isnan(rate) || std::isinf(rate)) {
            throw AlpacaException("MoveAxis rate must be finite", AlpacaError::InvalidValue);
        }
        if (std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("MoveAxis rate exceeds supported range", AlpacaError::InvalidValue);
        }

        auto& protocol = OnStepProtocolWrapper::instance();
        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;

        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        const int positive_dir = (axis == 0) ? 2 : 0;
        const int negative_dir = (axis == 0) ? 3 : 1;

        if (!moving) {
            // Defensive dual-stop, matching the project's convention for
            // fixed-direction (rather than signed-rate) motion protocols.
            try {
                protocol.move_axis_stop(positive_dir);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                protocol.move_axis_stop(negative_dir);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        } else {
            protocol.move_axis_start(rate > 0.0 ? positive_dir : negative_dir, std::abs(rate));
        }

        manual_axis_slewing_[axis] = moving;
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 2) {
            return {0.0, 0.0};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0, 1 or 2", AlpacaError::InvalidValue);
        }
        // OnStep's continuous-motion commands run at a single
        // firmware-selected rate (pinned to max at connect — see
        // select_max_slew_rate()), not an arbitrary requested deg/s value.
        // The advertised range therefore covers what MoveAxis will accept
        // without throwing, not a literal achievable-velocity curve.
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            return {};  // Tertiary axis unsupported.
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0, 1 or 2", AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("AbortSlew");
        auto& protocol = OnStepProtocolWrapper::instance();
        protocol.abort_slew();
        for (int dir = 0; dir < 4; ++dir) {
            try {
                protocol.move_axis_stop(dir);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        status_cache_valid_ = false;
    }

    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            compute_alt_az_target_locked(altitude, azimuth, ra, dec);
        }
        slew_to_coordinates(ra, dec);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            compute_alt_az_target_locked(altitude, azimuth, ra, dec);
        }
        slew_to_coordinates_async(ra, dec);
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

private:
    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
        }
    }

    void check_not_parked_locked(const char* operation) const {
        if (cached_status_.is_parked) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    void compute_alt_az_target_locked(double altitude, double azimuth, double& ra_out, double& dec_out) const {
        if (altitude < -90.0 || altitude > 90.0) {
            throw AlpacaException("Altitude out of range", AlpacaError::InvalidValue);
        }
        if (azimuth < 0.0 || azimuth > 360.0) {
            throw AlpacaException("Azimuth out of range", AlpacaError::InvalidValue);
        }
        ensure_site_info_cached_locked();
        const double lst = compute_local_sidereal_time_hours(current_utc_time_locked(), site_longitude_cached_);
        alt_az_to_ra_dec(altitude, azimuth, site_latitude_cached_, lst, ra_out, dec_out);
    }

    void start_slew_locked(double ra, double dec, const char* context) {
        validate_ra_dec(ra, dec, context);
        auto& protocol = OnStepProtocolWrapper::instance();
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        if (!protocol.slew_to_target()) {
            throw AlpacaException("Mount rejected slew target (below horizon or limit)", AlpacaError::InvalidOperation);
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        status_cache_valid_ = false;
        slewing_cached_ = true;
        slew_force_until_ = std::chrono::steady_clock::now() + kSlewForceGrace;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        tracking_before_slew_ = cached_status_.is_tracking;
    }

    bool get_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (std::chrono::steady_clock::now() < slew_force_until_) {
            return true;
        }
        const bool was_slewing = cached_status_.is_slewing;
        refresh_status_cache_locked(false);  // mutates cached_status_.is_slewing as a side effect
        // cppcheck-suppress incorrectLogicOperator
        // was_slewing is captured BEFORE the refresh call above; the checker's
        // simple dataflow model doesn't see that the call mutates
        // cached_status_, so it (wrongly) assumes both sides always match.
        if (was_slewing && !cached_status_.is_slewing) {
            // Slew just completed: caches are stale and, per the telescope
            // lessons in AGENTS.md, some LX200-family mounts stop tracking
            // during a GOTO — best-effort restore it. TODO: confirm against
            // real OnStep firmware whether this restoration is needed.
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            if (tracking_before_slew_) {
                try {
                    OnStepProtocolWrapper::instance().start_tracking();
                    cached_status_.is_tracking = true;
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                }
            }
        }
        return cached_status_.is_slewing;
    }

    // Poll for a status condition, RELEASING the driver mutex around every
    // sleep so a synchronous Park/FindHome/SlewToCoordinates doesn't block
    // all GETs and Disconnect for the whole operation (Bisque/SynScan
    // unlock-sleep-relock pattern). `lock` must be held on entry and is held
    // again on return/throw.
    template <typename Predicate>
    void wait_for_condition_locked(std::unique_lock<std::mutex>& lock, Predicate done) const {
        const auto timeout = std::chrono::seconds(120);
        const auto start = std::chrono::steady_clock::now();
        while (!done()) {
            if (std::chrono::steady_clock::now() - start > timeout) {
                throw AlpacaException("Operation timed out waiting for mount status");
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            lock.lock();
            check_connected();
            refresh_status_cache_locked(true);
        }
    }

    void wait_for_slew_complete_locked(std::unique_lock<std::mutex>& lock) const {
        wait_for_condition_locked(lock, [this] { return !get_slewing_locked(); });
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
    }

    void refresh_status_cache_locked(bool force) const {
        auto now = std::chrono::steady_clock::now();
        if (!force && status_cache_valid_ && (now - last_status_update_) < kStatusCacheTtl) {
            return;
        }
        try {
            cached_status_ = OnStepProtocolWrapper::instance().get_status();
            status_cache_valid_ = true;
            last_status_update_ = now;
        } catch (...) {
            if (!status_cache_valid_) {
                throw;
            }
        }
    }

    void refresh_equatorial_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (equatorial_cache_valid_ && (now - last_equatorial_update_) < kPositionCacheTtl) {
            return;
        }
        try {
            Position pos = OnStepProtocolWrapper::instance().get_position();
            cached_ra_hours_ = pos.ra_hours;
            cached_dec_degrees_ = pos.dec_degrees;
            equatorial_cache_valid_ = true;
            last_equatorial_update_ = now;
        } catch (...) {
            if (!equatorial_cache_valid_) {
                throw;
            }
        }
    }

    void refresh_altaz_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (altaz_cache_valid_ && (now - last_altaz_update_) < kPositionCacheTtl) {
            return;
        }
        try {
            AltAz altaz = OnStepProtocolWrapper::instance().get_alt_az();
            cached_alt_degrees_ = altaz.altitude_degrees;
            cached_az_degrees_ = altaz.azimuth_degrees;
            altaz_cache_valid_ = true;
            last_altaz_update_ = now;
        } catch (...) {
            if (!altaz_cache_valid_) {
                throw;
            }
        }
    }

    void ensure_site_info_cached_locked() const {
        if (!connected_) {
            return;
        }
        // Site coordinates only change via set_site_latitude/longitude
        // (which update the cache directly) — once valid, never re-fetch.
        // Missing this check meant every SiteLatitude/SiteLongitude/
        // SiderealTime read re-issued both :Gt#/:Gg# unconditionally,
        // confirmed against real hardware as repeated round trips blowing
        // the FAST response-time target on every single call, not just the
        // first.
        if (site_info_valid_) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (last_site_info_attempt_ != std::chrono::steady_clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSiteInfoRetryDelay) {
            return;
        }
        last_site_info_attempt_ = now;
        try {
            SiteInfo info = OnStepProtocolWrapper::instance().get_site_info();
            site_latitude_cached_ = info.latitude_degrees;
            site_longitude_cached_ = info.longitude_degrees;
            site_info_valid_ = true;
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            site_info_valid_ = false;
        }
    }

    // Body of set_utc_date with mutex_ already held — called from the
    // connect path (sync_time_on_connect_), which holds mutex_; calling the
    // public locking method there would self-deadlock (non-recursive mutex).
    void set_utc_date_locked(std::chrono::system_clock::time_point utc) {
        check_connected();
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);

        // Send UTC itself as the mount's "local" time with a 0 offset,
        // rather than converting to the host's local time/DST — confirmed
        // against real hardware: with a non-zero offset, the mount's
        // internal sidereal-time (:GS#) calculation came out ~85 minutes
        // wrong (not a clean ±1h double-application of the offset either,
        // so this is a genuine firmware quirk, not just a sign error like
        // the longitude one above). Skipping the offset entirely — long
        // established practice for LX200-family mounts precisely to dodge
        // per-firmware timezone/DST bugs like this — made :GS# match the
        // correct GMST-based value to within a second.
        std::tm utc_tm{};
#ifdef _WIN32
        gmtime_s(&utc_tm, &utc_time_t);
#else
        utc_tm = *std::gmtime(&utc_time_t);
#endif
        TimeInfo info;
        info.hour = utc_tm.tm_hour;
        info.minute = utc_tm.tm_min;
        info.second = utc_tm.tm_sec;
        info.month = utc_tm.tm_mon + 1;
        info.day = utc_tm.tm_mday;
        info.year = (utc_tm.tm_year + 1900) % 100;
        info.utc_offset_hours = 0.0;

        OnStepProtocolWrapper::instance().set_time(info);
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
    }

    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (!last_utc_valid_) {
            last_utc_set_ = std::chrono::system_clock::now();
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
        }
        auto elapsed = std::chrono::steady_clock::now() - last_utc_set_monotonic_;
        return last_utc_set_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
    }

    int device_number_;
    ConnectionInfo connection_info_;
    mutable std::mutex mutex_;
    bool connected_;

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    mutable bool target_set_ = false;

    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;

    mutable double cached_ra_hours_ = 0.0;
    mutable double cached_dec_degrees_ = 0.0;
    mutable bool equatorial_cache_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_equatorial_update_;

    mutable double cached_alt_degrees_ = 0.0;
    mutable double cached_az_degrees_ = 0.0;
    mutable bool altaz_cache_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_altaz_update_;

    mutable MountStatus cached_status_{};
    mutable bool status_cache_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_status_update_;

    mutable double site_latitude_cached_ = 0.0;
    mutable double site_longitude_cached_ = 0.0;
    mutable bool site_info_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_site_info_attempt_;
    double site_elevation_m_;

    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_ = false;

    mutable bool slewing_cached_ = false;
    mutable std::chrono::steady_clock::time_point slew_force_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    mutable bool tracking_before_slew_ = true;

    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;

    mutable bool pulse_guiding_active_ = false;
    mutable std::chrono::steady_clock::time_point pulse_guide_end_time_;

    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so
    // the get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;
};

std::unique_ptr<TelescopeDriver> create_onstep_telescope(int device_number, const ConnectionInfo& connection_info) {
    return create_onstep_telescope_with_site(device_number, connection_info, std::nullopt, std::nullopt, std::nullopt,
                                             std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_with_site(int device_number,
                                                                   const ConnectionInfo& connection_info,
                                                                   std::optional<double> site_latitude_deg,
                                                                   std::optional<double> site_longitude_deg,
                                                                   std::optional<double> site_elevation_m,
                                                                   std::optional<bool> sync_time_on_connect) {
    return std::make_unique<OnStepTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_auto(int device_number, int mount_index,
                                                              std::optional<double> site_latitude_deg,
                                                              std::optional<double> site_longitude_deg,
                                                              std::optional<double> site_elevation_m,
                                                              std::optional<bool> sync_time_on_connect) {
    auto ports = enumerate_onstep_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("OnStep mount"));
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                              std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("OnStep", "Auto-detected mount on " + port.port_path + " (fw " + port.version_string + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;

    return create_onstep_telescope_with_site(device_number, conn, site_latitude_deg, site_longitude_deg,
                                             site_elevation_m, sync_time_on_connect);
}

}  // namespace alpacacore::vendor::onstep

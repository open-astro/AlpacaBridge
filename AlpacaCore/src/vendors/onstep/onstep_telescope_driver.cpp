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
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
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
// open-astro#1378: how far inside the horizon and overhead limits a MoveAxis
// re-send must stay.
constexpr double kAltitudeLimitMarginDeg = 1.0;
// open-astro#1378: how often a running MoveAxis is re-sent so OnStepX's guide
// time limit (10 s by default) does not end it. Tests advance a FakeTaskClock.
constexpr auto kMoveAxisResendInterval = std::chrono::seconds(7);
// Informational only — OnStep does not expose a guide-rate query command in
// this driver, and CanSetGuideRates is false, so this value is never sent to
// or read from the mount; it merely satisfies the ASCOM GuideRate property.
// OnStepX numbers its releases from 10 (the HM-17 answers :GVN# with
// "10.23a"); legacy OnStep is 4.x and older. Only OnStepX knows :GXEE#.
bool is_onstepx_version(const std::string& version) {
    std::size_t digits = 0;
    while (digits < version.size() && std::isdigit(static_cast<unsigned char>(version[digits]))) ++digits;
    return digits > 0 && digits <= 4 && std::stoi(version.substr(0, digits)) >= 10;
}

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
    double days_since_epoch = duration<double>(utc_time.time_since_epoch()).count() / 86400.0;
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
    if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
        throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
    }
    if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
        throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
    }
}

}  // namespace

class OnStepTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    OnStepTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                          std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                          std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                          util::ConnectionResolver<ConnectionInfo> connection_resolver = {},
                          util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("OnStep"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)) {}

    ~OnStepTelescopeDriver() override {
        // MUST be first, before members the async connection task touches
        // are destroyed (AsyncConnectable base contract). The only other
        // worker is the MoveAxis re-send (#1378), joined next without mutex_
        // (it takes mutex_ to send).
        shutdown_connection();
        move_axis_resend_.cancel_all_and_join();
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
        // Last: the process-wide wrapper must not keep a clock that may die
        // with this driver (a test's FakeTaskClock).
        OnStepProtocolWrapper::instance().release_task_clock(clock_);
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

    // Lock-free on purpose (open-astro#910): set_connected(true) holds mutex_
    // for the whole connect (port open, handshake, site and time sync, cache
    // warm-up, and an auto-detect scan when the port is not explicit). The
    // router checks Connecting before most Connected reads, but DeviceState
    // calls this with no such check, and a read that lands just before the
    // connect task shows as running gets past it; with mutex_ taken here those
    // waited for the whole connect. link_alive() takes no lock a connect holds: the wrapper mutex_
    // only by try_lock, otherwise only short leaf mutexes (a stat(), a few fields).
    // It reads published_connected_, raised only at the end of a connect, so
    // Connected stays false while Connecting is true. Same rule as SynScan
    // (#130); the blocking-driver list lives in async_connectable.h.
    bool get_connected() const override {
        // A removed or re-enumerated adapter reads as not connected (#1241).
        return published_connected_.load() && OnStepProtocolWrapper::instance().link_alive();
    }

    // Why the serial link was lost, kept until the next Connect or Disconnect
    // (decision 0009). Wrapper state only: never waits on mutex_.
    std::string get_link_fault() const override {
        auto& protocol = OnStepProtocolWrapper::instance();
        (void)protocol.link_alive();  // notice an unplug the listing would otherwise miss
        const std::string reason = protocol.link_lost_reason();
        if (reason.empty()) {
            return {};
        }
        return "Link to the mount lost (" + reason + "); reconnect";
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lock(mutex_);
        auto& protocol = OnStepProtocolWrapper::instance();
        // A Connected=true against a lost link must reconnect, not return as
        // already connected (decision 0009 point 4, #1241).
        if (connected && connected_ && !protocol.link_alive()) {
            ALPACA_LOG_WARN("OnStep", "Connect requested on a lost link; reconnecting");
            published_connected_ = false;
            protocol.disconnect();
            connected_ = false;
            reset_session_state_locked();
        }
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

        if (connected) {
            // The wrapper's loss stamps read this driver's clock (decision 0005).
            protocol.set_task_clock(clock_);
            // An auto-detected mount resolves its port here, not in the factory (#659).
            util::connect_resolved(
                connection_info_, connection_resolved_, connection_resolver_,
                [&protocol](const ConnectionInfo& info) {
                    if (!protocol.connect(info)) {
                        // Refused TCP connect or vanished serial node: stale, re-scan.
                        if (info.type == ConnectionType::Network || util::device_node_missing(info.port_path)) {
                            throw util::StaleEndpoint("Failed to connect to OnStep mount");
                        }
                        throw AlpacaException("Failed to connect to OnStep mount");
                    }
                },
                "OnStep");
            connected_ = true;
            stop_motion_after_long_outage_locked(protocol);
            status_cache_valid_ = false;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            site_info_valid_ = false;
            utc_offset_.clear();
            client_disagreement_warned_ = false;
            firmware_reports_guide_move_ = false;
            target_ra_set_ = false;
            target_dec_set_ = false;
            clear_manual_motion_locked();
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            pulse_guiding_active_ = false;
            tracking_before_slew_ = false;
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();

            std::string firmware_version;
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                try {
                    std::string product = protocol.get_product_name();
                    firmware_version = protocol.get_version_number();
                    firmware_cache_ = product + (firmware_version.empty() ? "" : (" " + firmware_version));
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    firmware_cache_.clear();
                }
            }

            // OnStepX applies refraction to the RA/Dec it reports unless it was
            // built for observed-place coordinates; :GXEE# says which (#1216).
            refraction_from_firmware_.reset();
            if (is_onstepx_version(firmware_version)) {
                try {
                    if (const auto mode = protocol.get_mount_coords_mode()) {
                        refraction_from_firmware_ = (*mode != 0);
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Unknown: DoesRefraction keeps the settable local flag.
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
            // fast-response targets. This covers the first DeviceState too:
            // after ConformU's Connected=false / Connect() preamble from a
            // single client it answers in 3-5 ms with no serial reads. It
            // only misses when another client keeps the device open, so the
            // preamble does not reconnect and this block does not run
            // (#1242).
            try {
                refresh_status_cache_locked(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                Position pos = protocol.get_position();
                cached_ra_hours_ = pos.ra_hours;
                cached_dec_degrees_ = pos.dec_degrees;
                equatorial_cache_valid_ = true;
                last_equatorial_update_ = clock_.now();
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
                last_altaz_update_ = clock_.now();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            try {
                refresh_status_cache_locked(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            published_connected_ = true;  // the connect is complete (#910)
        } else {
            published_connected_ = false;
            protocol.disconnect();
            connected_ = false;
            reset_session_state_locked();
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
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
        if (pulse_guiding_active_ && clock_.now() >= pulse_guide_end_time_) {
            pulse_guiding_active_ = false;
        }
        return pulse_guiding_active_;
    }

    // OnStep's custom tracking-rate commands were not confirmed against real
    // firmware during this implementation (see .github/instructions/onstep.instructions.md) —
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
        last_status_update_ = clock_.now();
        // The client has said what it wants; a pending GOTO restore must not
        // override it when the slew ends (#1230).
        tracking_before_slew_ = false;
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
        // .github/instructions/onstep.instructions.md) was tried as the primary source instead,
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
        // :GRa#/:GDe# round trip (~25 ms each on the HM-17 at 9600 baud;
        // ~0.16s each while read_response() slept per character, #1242)
        // landing inside this call (~50 ms together) uses about half of
        // ConformU's 0.1s FAST target, and about three times it before #1242 — confirmed against real
        // hardware, where this showed up as an occasional "SideOfPier Read
        // ... OUTSIDE FAST RESPONSE TIME TARGET" once the TTL had just lapsed.
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

    // OnStepX reports its refraction mode at connect (#1216). It is fixed when
    // the firmware is built, so only the value the firmware already uses can
    // be written. Older firmware keeps the local flag.
    bool get_does_refraction() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return refraction_from_firmware_.value_or(does_refraction_);
    }

    void set_does_refraction(bool does_refraction) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (refraction_from_firmware_.has_value()) {
            if (does_refraction != *refraction_from_firmware_) {
                throw AlpacaException(std::string("This OnStepX firmware ") +
                                          (*refraction_from_firmware_ ? "always applies" : "never applies") +
                                          " refraction to its coordinates; it is set when the firmware is built",
                                      AlpacaError::PropertyNotImplemented);
            }
            return;
        }
        does_refraction_ = does_refraction;
    }

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
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
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
        if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
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
        if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
            throw AlpacaException("TargetDeclination must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
            throw AlpacaException("TargetRightAscension must be in range 0 to <24 hours", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_ra_hours_ = ra;
        target_ra_set_ = true;
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
        if (!utc_offset_.valid()) {
            std::chrono::system_clock::time_point seeded;
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
                seeded = local_time - offset;
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                seeded = std::chrono::system_clock::now();
            }
            utc_offset_.set(seeded, clock_.now());
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
        // OnStepX ends a home with tracking off; never restart it afterwards (#1218).
        tracking_before_slew_ = false;
        cached_status_.is_at_home = false;
        cached_status_.is_slewing = true;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
    }

    void park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_status_cache_locked(true);
        if (cached_status_.is_parked) {
            return;  // ASCOM: Park() on an already-parked mount is a no-op.
        }
        OnStepProtocolWrapper::instance().park();
        // A parked mount must not track; never restart it afterwards (#1218).
        tracking_before_slew_ = false;
        cached_status_.is_parked = false;
        cached_status_.is_slewing = true;
        cached_status_.is_at_home = false;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
        clear_manual_motion_locked();
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
        pulse_guide_end_time_ = clock_.now() + std::chrono::milliseconds(clamped_duration);
        equatorial_cache_valid_ = false;
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        OnStepProtocolWrapper::instance().set_park();
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_equatorial_slew_locked(ra, dec, "SlewToCoordinates");
        start_slew_locked(ra, dec, "SlewToCoordinates");
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_equatorial_slew_locked(ra, dec, "SlewToCoordinatesAsync");
        start_slew_locked(ra, dec, "SlewToCoordinatesAsync");
    }

    void slew_to_target() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates(ra, dec);
    }

    void slew_to_target_async() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates_async(ra, dec);
    }

    void sync_to_coordinates(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        validate_ra_dec(ra, dec, "SyncToCoordinates");
        auto& protocol = OnStepProtocolWrapper::instance();
        // Use the mount's own sync command — do NOT maintain a driver-level
        // offset, which would cause coordinate divergence during subsequent
        // slews (project-wide rule; see the iOptron and Celestron files under .github/instructions/).
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        protocol.sync_to_target();
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        cached_ra_hours_ = ra;
        cached_dec_degrees_ = dec;
        equatorial_cache_valid_ = true;
        last_equatorial_update_ = clock_.now();
    }

    void sync_to_target() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        sync_to_coordinates(ra, dec);
    }

    void unpark() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        OnStepProtocolWrapper::instance().unpark();
        cached_status_.is_parked = false;
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
    }

    bool get_can_move_axis(int axis) const override {
        if (axis != 0 && axis != 1 && axis != 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        std::unique_lock<std::mutex> lock(mutex_);
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

        // Drop this axis's earlier move from the re-send first: a stop asks
        // for none whatever its outcome, and a new move sets it again only
        // once its own command went out. This only withdraws the old move,
        // under mutex_ where no body runs; decision 0006 rule 3's bump still
        // comes before the new move's flags below.
        manual_axis_resend_[axis] = false;
        if (!moving) {
            // Defensive dual-stop, matching the project's convention for
            // fixed-direction (rather than signed-rate) motion protocols.
            // Both stops are always tried. The driver cannot tell which
            // direction the mount needed stopped, so any failed stop may be
            // the one that mattered: report it and keep the axis moving
            // (#742).
            std::string stop_error;
            const auto try_stop = [&](int dir) {
                try {
                    protocol.move_axis_stop(dir);
                } catch (const std::exception& ex) {
                    if (stop_error.empty()) {
                        stop_error = ex.what();
                    }
                } catch (...) {
                    if (stop_error.empty()) {
                        stop_error = "unknown exception";
                    }
                }
            };
            try_stop(positive_dir);
            try_stop(negative_dir);
            if (!stop_error.empty()) {
                throw AlpacaException("MoveAxis stop failed: " + stop_error, AlpacaError::DriverException);
            }
        } else {
            const int dir = rate > 0.0 ? positive_dir : negative_dir;
            protocol.move_axis_start(dir, std::abs(rate));
            // Decision 0006 rule 3: supersede the running re-send under mutex_
            // before this move's flags change; start() below replaces it. Only
            // once the move went out: a failed send must leave the other
            // axis's re-send running.
            move_axis_generation_.bump();
            manual_axis_dir_[axis] = dir;
            manual_axis_rate_[axis] = std::abs(rate);
            manual_axis_resend_[axis] = true;
            manual_axis_resend_due_[axis] = clock_.now() + kMoveAxisResendInterval;
            status_read_since_move_ = false;  // a :GU# from before the move must not end it (#1378)
        }

        manual_axis_slewing_[axis] = moving;
        if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
            clear_manual_motion_locked();
            return;
        }
        if (!moving) {
            return;
        }
        // Always a fresh body, never "already running": a body that has just
        // found nothing to re-send may still read as running while it exits.
        // The new one supersedes it.
        lock.unlock();  // start() without mutex_: decision 0006 rule 4, async_operation.h rule 10
        start_move_axis_resend();
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 2) {
            return {0.0, 0.0};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
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
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("AbortSlew");
        auto& protocol = OnStepProtocolWrapper::instance();
        // Try every stop on its own, so one lost command cannot skip the others (#781).
        std::string stop_error;
        const auto try_stop = [&stop_error](const std::function<void()>& stop) {
            try {
                stop();
            } catch (const std::exception& ex) {
                if (stop_error.empty()) {
                    stop_error = ex.what();
                }
            } catch (...) {
                if (stop_error.empty()) {
                    stop_error = "unknown exception";
                }
            }
        };
        try_stop([&protocol]() { protocol.abort_slew(); });
        for (int dir = 0; dir < 4; ++dir) {
            try_stop([&protocol, dir]() { protocol.move_axis_stop(dir); });
        }
        if (!stop_error.empty()) {
            // The mount may still be moving: leave Slewing as it was, but
            // stop re-sending a MoveAxis the client asked to end.
            manual_axis_resend_[0] = false;
            manual_axis_resend_[1] = false;
            throw AlpacaException("AbortSlew stop failed: " + stop_error, AlpacaError::DriverException);
        }
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        clear_manual_motion_locked();
        status_cache_valid_ = false;
        // ASCOM: after AbortSlew, tracking returns to its state before the
        // slew. Decide now instead of at a later slew end the driver may never
        // see (#1230). Only an aborted GOTO costs the extra :GU#.
        if (tracking_before_slew_) {
            try {
                refresh_status_cache_locked(true);
                finish_slew_tracking_locked();
            } catch (...) {
                tracking_before_slew_ = false;  // status unknown: never restart blind
            }
        }
    }

    // An Alt/Az slew is not equatorial, so it skips the Tracking check
    // (open-astro#1274) and goes straight to the GOTO.
    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        compute_alt_az_target_locked(altitude, azimuth, ra, dec);
        check_not_parked_locked("SlewToCoordinates");
        start_slew_locked(ra, dec, "SlewToCoordinates");
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        compute_alt_az_target_locked(altitude, azimuth, ra, dec);
        check_not_parked_locked("SlewToCoordinatesAsync");
        start_slew_locked(ra, dec, "SlewToCoordinatesAsync");
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
        // A removed or re-enumerated adapter is NotConnected, not a generic
        // send failure while Connected still reads true (#1241).
        if (!OnStepProtocolWrapper::instance().link_alive()) {
            const std::string reason = OnStepProtocolWrapper::instance().link_lost_reason();
            throw AlpacaException("OnStep link lost" + (reason.empty() ? std::string{} : " (" + reason + ")") +
                                      "; set Connected true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // Ends the driver's record of a MoveAxis and its re-send (#1378).
    // cancel() may be called with mutex_ held (async_operation.h rule 10).
    // Slews, Park and AbortSlew end the re-send here with cancel() rather
    // than a supersede (decision 0006 rule 3): safe, because the body checks
    // ctx.stop_reason() under mutex_ before it sends anything, and these
    // callers hold mutex_.
    void clear_manual_motion_locked() const {
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        manual_axis_resend_[0] = false;
        manual_axis_resend_[1] = false;
        move_axis_resend_.cancel();
    }

    // :GU# says the firmware ended the running MoveAxis (#1378): no 'g',
    // from firmware that has shown 'g' since this connect. 'N' covers a GOTO
    // only. Firmware that never sends 'g' never ends it here, and a pulse
    // guide ('G') hides 'g' while it runs, so it is no evidence either. Only
    // a :GU# read after the move was sent counts (the firmware runs commands
    // in order): the setters that patch cached_status_ (Tracking, park,
    // unpark, FindHome) keep its older 'g'.
    bool manual_move_ended_locked() const {
        if (!status_read_since_move_ || cached_status_.is_pulse_guide_active) {
            return false;
        }
        if (cached_status_.is_guide_active) {
            firmware_reports_guide_move_ = true;
            return false;
        }
        return firmware_reports_guide_move_;
    }

    // open-astro#1378: OnStepX stops an equatorial mount at the horizon and
    // overhead limits only on the crossing (Limits::poll calls stop() when the
    // error turns on), and Guide::validate skips limits.isError() for a custom
    // rate, so a re-send after that crossing, or during the braking that
    // follows it, would carry the mount past the limit. Re-send only with the
    // altitude at least kAltitudeLimitMarginDeg inside both limits. The margin
    // covers the whole-degree :Gh#/:Go# replies and the pointing model. A
    // failed or out-of-range read sends nothing. The meridian, axis and switch
    // limits need no check here: OnStepX enforces them on every poll and
    // refuses a move further out (Guide::validAxis1/validAxis2).
    bool resend_within_altitude_limits_locked(OnStepProtocolWrapper& protocol) const {
        double altitude = 0.0;
        AltitudeLimits limits;
        try {
            altitude = protocol.get_alt_az().altitude_degrees;
            limits = protocol.get_altitude_limits();
        } catch (const std::exception& ex) {
            ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send skipped, altitude read failed: ") + ex.what());
            return false;
        }
        if (limits.horizon_degrees < -30.0 || limits.horizon_degrees > 30.0 || limits.overhead_degrees < 60.0 ||
            limits.overhead_degrees > 90.0) {
            ALPACA_LOG_WARN("OnStep", "MoveAxis re-send skipped, altitude limits out of range");
            return false;
        }
        const bool overhead_on = limits.overhead_degrees < 90.0;
        if (altitude < limits.horizon_degrees + kAltitudeLimitMarginDeg ||
            (overhead_on && altitude > limits.overhead_degrees - kAltitudeLimitMarginDeg)) {
            char text[160];
            std::snprintf(
                text, sizeof(text),
                "MoveAxis not re-sent: altitude %.2f deg is within %.0f deg of, or past, a limit (horizon %.0f, "
                "overhead %.0f); the firmware ends the move",
                altitude, kAltitudeLimitMarginDeg, limits.horizon_degrees, limits.overhead_degrees);
            ALPACA_LOG_INFO("OnStep", text);  // a client can cause it: decision 0007 rule 9
            return false;
        }
        return true;
    }

    // open-astro#1378: OnStepX ends a :Mn#/:Ms#/:Me#/:Mw# move after
    // GUIDE_TIME_LIMIT (default 10 s, 1-120 s or 0 for none at firmware
    // build; Guide.command.cpp), but ASCOM runs a MoveAxis until the client
    // sends rate 0. Each repeat of the move restarts that timer
    // (Guide::startAxis1), so re-send every running move while one is set,
    // every 7 s by default: inside the 10 s default, and seldom, because a
    // move command that lands while an axis brakes after a stop restarts it
    // (Axis::autoSlew; 'g' stays set for about 0.4 s of braking at 0.5 deg/s
    // on the HM-17). Each tick reads :GU# first and re-sends only while the
    // mount shows the move ('g') in that read, so a stop from a hand
    // controller or another app is undone only when the tick lands in that
    // braking. Near the horizon and overhead limits nothing is re-sent (see
    // resend_within_altitude_limits_locked()). A failed read skips the tick.
    // A lost link ends the re-send; the #1241 path handles the link and the
    // firmware limit stops the mount, as before.
    void start_move_axis_resend() {
        move_axis_resend_.start([this](util::OperationContext& ctx) {
            const auto wants_resend = [this](int axis) {
                return manual_axis_slewing_[axis] && manual_axis_resend_[axis];
            };
            for (;;) {
                // Each axis is due on its own clock, as OnStepX times each
                // axis on its own: a MoveAxis on the other axis replaces this
                // body but must not push this axis's re-send back.
                std::chrono::steady_clock::duration wait{};
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (ctx.stop_reason() != util::StopReason::None || !connected_) {
                        return;
                    }
                    if (!wants_resend(0) && !wants_resend(1)) {
                        return;  // stopped, even if the stop failed: nothing left to keep going
                    }
                    auto earliest = std::chrono::steady_clock::time_point::max();
                    for (int axis = 0; axis < 2; ++axis) {
                        if (wants_resend(axis)) {
                            earliest = std::min(earliest, manual_axis_resend_due_[axis]);
                        }
                    }
                    wait = std::max(earliest - clock_.now(), std::chrono::steady_clock::duration::zero());
                }
                if (!ctx.wait_for(wait)) {
                    return;
                }
                // The tick holds mutex_ across its serial exchanges (:GU#, the
                // altitude reads, the re-send), as every OnStep path does, so a
                // getter can wait for it. Decision 0016 rule 1 calls that a
                // defect; moving OnStep's I/O off mutex_ is #1449.
                std::lock_guard<std::mutex> lock(mutex_);
                if (ctx.stop_reason() != util::StopReason::None || !connected_) {
                    return;
                }
                const auto now = clock_.now();
                bool due[2] = {false, false};
                for (int axis = 0; axis < 2; ++axis) {
                    due[axis] = wants_resend(axis) && manual_axis_resend_due_[axis] <= now;
                }
                if (!due[0] && !due[1]) {
                    continue;  // a newer MoveAxis moved the due time
                }
                const bool keep_going = resend_tick_locked(due);
                // Due again a full interval after this tick ends, whatever it
                // did: a skipped tick never spins, and mutex_ is free between
                // ticks.
                const auto next = clock_.now() + kMoveAxisResendInterval;
                for (int axis = 0; axis < 2; ++axis) {
                    if (due[axis]) {
                        manual_axis_resend_due_[axis] = next;
                    }
                }
                if (!keep_going) {
                    return;
                }
            }
        });
    }

    // One re-send tick for the axes in `due` (#1378). False ends the body.
    bool resend_tick_locked(const bool (&due)[2]) {
        auto& protocol = OnStepProtocolWrapper::instance();
        if (!protocol.link_alive()) {
            return false;
        }
        try {
            cached_status_ = protocol.get_status();
            status_cache_valid_ = true;
            status_read_since_move_ = true;
            last_status_update_ = clock_.now();
        } catch (const std::exception& ex) {
            ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send skipped, status read failed: ") + ex.what());
            return true;
        }
        if (manual_move_ended_locked()) {
            clear_manual_motion_locked();  // cancel() from its own body is allowed
            return false;
        }
        // Re-send only a move the mount shows now. A move the firmware let
        // lapse stays ended: its time limit, or the 5 s GUIDE_TIME_NO_LIMITS
        // cap while its soft limits are off (Guide::finalizeTimeLimit), which
        // asks for a fresh command on purpose. Firmware without 'g' gets no
        // re-send, and a pulse guide ('G') skips the tick.
        if (!cached_status_.is_guide_active) {
            return true;
        }
        if (!resend_within_altitude_limits_locked(protocol)) {
            return true;  // the firmware ends the move; 'g' then ends it here
        }
        for (int axis = 0; axis < 2; ++axis) {
            if (!due[axis]) {
                continue;
            }
            try {
                protocol.move_axis_start(manual_axis_dir_[axis], manual_axis_rate_[axis]);
            } catch (const std::exception& ex) {
                ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send failed: ") + ex.what());
            }
        }
        return true;
    }

    // The per-session state a disconnect or a lost link leaves behind.
    void reset_session_state_locked() {
        refraction_from_firmware_.reset();
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        target_ra_set_ = false;
        target_dec_set_ = false;
        clear_manual_motion_locked();
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        pulse_guiding_active_ = false;
        utc_offset_.clear();  // decision 0014: the written UTCDate lives until the next connect or disconnect
    }

    // open-astro#521: OnStepX keeps running a GOTO while the serial link is
    // out (on the HM-17 a GOTO pulled mid-slew stopped only when this sent
    // the stops, #1241), and a MoveAxis for up to about 10 s before the
    // firmware ends it by itself (#1098). On the connect that follows a
    // loss, an outage at least as long as the preserve window means nobody
    // was in control of that motion, so stop it with the same commands
    // AbortSlew sends. A shorter outage is a glitch the client rides out,
    // and no recorded loss (e.g. a fresh start) preserves motion: no stamp
    // is not evidence that anything ran unattended. The outage runs from the
    // last time the link was seen alive to the end of this reconnect (an
    // auto-detect re-scan counts): rounds toward stopping. Tracking is kept.
    void stop_motion_after_long_outage_locked(OnStepProtocolWrapper& protocol) {
        const auto lost_at = protocol.consume_link_lost_at();
        const auto alive_at = protocol.consume_last_alive_before_loss();
        if (!lost_at) {
            return;
        }
        // Timed from the last moment the link was known to be there, not from
        // when the loss was noticed: with no poll thread (only a MoveAxis
        // re-send checks the link, #1378), the first to notice
        // may be the reconnect itself (the router reads Connected before every
        // Connect), which would make a long unattended outage look like 0 s.
        // The wrapper stamped both times on clock_ (set at connect).
        const auto outage = clock_.now() - (alive_at ? *alive_at : *lost_at);
        if (outage < detail::relink_motion_preserve_window()) {
            return;
        }
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1f", std::chrono::duration<double>(outage).count());
        ALPACA_LOG_WARN("OnStep", std::string("Serial link was last seen alive ") + seconds +
                                      " s ago; stopping any slew or MoveAxis that ran unattended (open-astro#521)");
        // Each stop on its own, as AbortSlew does (#781): a failed :Q# must
        // not skip the per-axis stops that end a MoveAxis.
        const auto try_stop = [](const char* what, const std::function<void()>& stop) {
            try {
                stop();
            } catch (const std::exception& ex) {
                ALPACA_LOG_ERROR("OnStep", std::string("Stop after a long outage failed (") + what + "): " + ex.what());
            }
        };
        try_stop(":Q#", [&protocol]() { protocol.abort_slew(); });
        for (int dir = 0; dir < 4; ++dir) {
            try_stop("axis stop", [&protocol, dir]() { protocol.move_axis_stop(dir); });
        }
    }

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). Runs after the parked check and argument validation and
    // before any target or GOTO command is sent, and reads Tracking as the getter
    // does (a :GU# query when the cache is stale), so a Tracking=false just
    // written counts as off.
    void check_equatorial_slew_locked(double ra, double dec, const char* who) const {
        check_connected();
        check_not_parked_locked(who);
        validate_ra_dec(ra, dec, who);
        refresh_status_cache_locked(false);
        if (!cached_status_.is_tracking) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    void check_not_parked_locked(const char* operation) const {
        if (cached_status_.is_parked) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    void compute_alt_az_target_locked(double altitude, double azimuth, double& ra_out, double& dec_out) const {
        if (!std::isfinite(altitude) || altitude < -90.0 || altitude > 90.0) {
            throw AlpacaException("Altitude out of range", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth > 360.0) {
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
        target_ra_set_ = true;
        target_dec_set_ = true;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        status_cache_valid_ = false;
        slewing_cached_ = true;
        slew_force_until_ = clock_.now() + kSlewForceGrace;
        clear_manual_motion_locked();
        tracking_before_slew_ = cached_status_.is_tracking;
    }

    bool get_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            // Slewing follows the mount once it has shown the move (#1378).
            try {
                refresh_status_cache_locked(false);
            } catch (...) {
                return true;  // keep reporting the move; the read error is not a stop
            }
            if (!manual_move_ended_locked()) {
                return true;
            }
            clear_manual_motion_locked();
        }
        if (clock_.now() < slew_force_until_) {
            return true;
        }
        const bool was_slewing = cached_status_.is_slewing;
        refresh_status_cache_locked(false);  // mutates cached_status_.is_slewing as a side effect
        // cppcheck-suppress incorrectLogicOperator
        // was_slewing is captured BEFORE the refresh call above; the checker's
        // simple dataflow model doesn't see that the call mutates
        // cached_status_, so it (wrongly) assumes both sides always match.
        if (was_slewing && !cached_status_.is_slewing) {
            // Slew just completed: caches are stale. Per the SynScan
            // tracking-restoration lesson in
            // .github/instructions/synscan.instructions.md, some LX200-family
            // mounts stop tracking during a GOTO, so restore it once, and only
            // after a coordinate slew that started with tracking on and ended
            // without it. OnStepX keeps tracking through a GOTO (seen on the
            // TeSeek HM-17), so there this normally sends nothing. Park and
            // FindHome clear the flag: they end with tracking off on purpose,
            // and restarting it moved the mount off home (#1218).
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            finish_slew_tracking_locked();
        }
        return cached_status_.is_slewing;
    }

    // Uses and clears the restore flag that start_slew_locked() set: restore
    // tracking once if that slew started with tracking on and the mount now
    // reports it off. Every path that ends a coordinate slew calls this (the
    // async edge above, wait_for_slew_complete_locked(), abort_slew()), so the
    // flag cannot outlive its slew and fire at an unrelated one (#1230).
    void finish_slew_tracking_locked() const {
        const bool restore_tracking = tracking_before_slew_ && !cached_status_.is_tracking;
        tracking_before_slew_ = false;
        if (restore_tracking) {
            try {
                OnStepProtocolWrapper::instance().start_tracking();
                cached_status_.is_tracking = true;
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
    }

    // Poll for a status condition, RELEASING the driver mutex around every
    // sleep so a synchronous Park/FindHome/SlewToCoordinates doesn't block
    // all GETs and Disconnect for the whole operation (Bisque/SynScan
    // unlock-sleep-relock pattern). `lock` must be held on entry and is held
    // again on return/throw.
    template <typename Predicate>
    void wait_for_condition_locked(std::unique_lock<std::mutex>& lock, Predicate done) const {
        const auto timeout = std::chrono::seconds(120);
        const auto start = clock_.now();
        while (!done()) {
            if (clock_.now() - start > timeout) {
                throw AlpacaException("Operation timed out waiting for mount status");
            }
            lock.unlock();
            clock_.sleep_for(std::chrono::milliseconds(250));
            lock.lock();
            check_connected();
            refresh_status_cache_locked(true);
        }
    }

    void wait_for_slew_complete_locked(std::unique_lock<std::mutex>& lock) const {
        wait_for_condition_locked(lock, [this] { return !get_slewing_locked(); });
        // The loop's forced refresh hides the slewing -> idle edge from
        // get_slewing_locked(), so decide here; the status was just read (#1230).
        finish_slew_tracking_locked();
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
    }

    void refresh_status_cache_locked(bool force) const {
        auto now = clock_.now();
        if (!force && status_cache_valid_ && (now - last_status_update_) < kStatusCacheTtl) {
            return;
        }
        try {
            cached_status_ = OnStepProtocolWrapper::instance().get_status();
            status_cache_valid_ = true;
            status_read_since_move_ = true;
            last_status_update_ = now;
        } catch (...) {
            if (!status_cache_valid_) {
                throw;
            }
        }
    }

    void refresh_equatorial_cache_locked() const {
        auto now = clock_.now();
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
        auto now = clock_.now();
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
        auto now = clock_.now();
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

        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        OnStepProtocolWrapper::instance().set_time(info);
        utc_offset_.set(utc, clock_.now());
        // The mount now runs on the client's clock and so does the LST above;
        // on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("OnStep", client_minus_host, client_disagreement_warned_);
    }

    // The lazy re-seed below can now also follow a disconnect, which clears
    // utc_offset_. That is harmless: every caller is guarded by NotConnected.
    std::chrono::system_clock::time_point current_utc_time_locked() const {
        const auto now = clock_.now();
        if (const auto utc = utc_offset_.current(now)) {
            return *utc;
        }
        const auto seeded = std::chrono::system_clock::now();
        utc_offset_.set(seeded, now);
        return seeded;
    }

    // Decision 0005: every wait, sleep and deadline of this driver (#1378),
    // and the wrapper's link-loss stamps (set_task_clock() at connect).
    util::TaskClock& clock_;
    int device_number_;
    ConnectionInfo connection_info_;
    // Set by the auto-detect factory; empty for an explicit port or host.
    // connection_resolved_ is true once a connect has run the resolver, so a
    // later connect retries that endpoint before scanning again (#659).
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    mutable std::mutex mutex_;
    bool connected_;
    // What get_connected() reports, read lock-free (#910). Raised only once a
    // connect has finished, so Connected never reads true while Connecting
    // does; dropped before any disconnect. Written under mutex_.
    std::atomic<bool> published_connected_{false};
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    // open-astro#346 (the shape #304 fixed on the Sky-Watcher driver): ASCOM
    // treats the two target properties as independent, so each must throw
    // ValueNotSet until that property itself has been written. One shared flag
    // let a write to either unlock both, and a client reading the one it did
    // not set got a default 0 instead of an error. The paths that legitimately
    // define both coordinates at once -- the slew and sync coordinate forms,
    // the position-override and arrival reads, and the connect/disconnect
    // resets -- still set or clear both.
    mutable bool target_ra_set_ = false;
    mutable bool target_dec_set_ = false;

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

    // Decision 0014: the UTCDate a client wrote (or the seed) and the task-clock
    // time since. Cleared on connect and disconnect. Under mutex_.
    mutable util::UtcOffset utc_offset_;

    mutable bool slewing_cached_ = false;
    mutable std::chrono::steady_clock::time_point slew_force_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    // The running MoveAxis per axis, for the re-send (#1378). Under mutex_.
    int manual_axis_dir_[2] = {0, 0};
    double manual_axis_rate_[2] = {0.0, 0.0};
    mutable bool manual_axis_resend_[2] = {false, false};
    std::chrono::steady_clock::time_point manual_axis_resend_due_[2]{};
    // :GU# has shown 'g' since this connect, so its absence ends a MoveAxis (#1378).
    mutable bool firmware_reports_guide_move_ = false;
    // A real :GU# read has landed since the last MoveAxis start (#1378).
    mutable bool status_read_since_move_ = false;
    // Set only by a coordinate slew; consumed when that slew completes (#1218).
    mutable bool tracking_before_slew_ = false;

    bool does_refraction_ = false;
    // From :GXEE# at connect on OnStepX; empty when unknown or disconnected.
    std::optional<bool> refraction_from_firmware_;
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

    // Last, so it is destroyed (and its body joined) before every member the
    // body touches. The destructor joins it explicitly as well.
    util::OperationGeneration move_axis_generation_;
    mutable util::AsyncOperation move_axis_resend_{"OnStep MoveAxis re-send", move_axis_generation_, clock_};
};

std::unique_ptr<TelescopeDriver> create_onstep_telescope(int device_number, const ConnectionInfo& connection_info,
                                                         util::TaskClock& clock) {
    return create_onstep_telescope_with_site(device_number, connection_info, std::nullopt, std::nullopt, std::nullopt,
                                             std::nullopt, clock);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock) {
    return std::make_unique<OnStepTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                   util::ConnectionResolver<ConnectionInfo>{}, clock);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect) {
    if (!connection_resolver) {
        throw AlpacaException("OnStep telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<OnStepTelescopeDriver>(device_number, ConnectionInfo{}, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                   std::move(connection_resolver));
}

ConnectionInfo resolve_onstep_serial_auto(int mount_index) {
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
    return conn;
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_auto(int device_number, int mount_index,
                                                              std::optional<double> site_latitude_deg,
                                                              std::optional<double> site_longitude_deg,
                                                              std::optional<double> site_elevation_m,
                                                              std::optional<bool> sync_time_on_connect) {
    // The serial scan runs at connect time (#659), not here.
    return create_onstep_telescope_deferred(
        device_number, [mount_index] { return resolve_onstep_serial_auto(mount_index); }, site_latitude_deg,
        site_longitude_deg, site_elevation_m, sync_time_on_connect);
}

namespace detail {

namespace {
std::atomic<std::chrono::milliseconds::rep>& relink_window_ms() {
    static std::atomic<std::chrono::milliseconds::rep> window{
        std::chrono::duration_cast<std::chrono::milliseconds>(util::kRelinkMotionPreserveWindow).count()};
    return window;
}
}  // namespace

void set_relink_motion_preserve_window(std::chrono::milliseconds window) { relink_window_ms() = window.count(); }

std::chrono::milliseconds relink_motion_preserve_window() {
    return std::chrono::milliseconds(relink_window_ms().load());
}

}  // namespace detail

}  // namespace alpacacore::vendor::onstep

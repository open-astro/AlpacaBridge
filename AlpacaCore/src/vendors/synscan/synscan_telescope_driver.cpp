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
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <thread>

namespace alpacacore::vendor::synscan {

namespace {

constexpr double kHoursToDegrees = 15.0;
constexpr auto kPositionCacheTtl = std::chrono::seconds(2);
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
constexpr double kDefaultGuideRateDegPerSec = 7.5 / 3600.0;
constexpr double kSiderealDegPerSec = 15.0411 / 3600.0;
constexpr auto kPulseGuideCompletionDelay = std::chrono::milliseconds(1000);
constexpr auto kPulseGuidePositionGrace = std::chrono::milliseconds(3000);

double wrap_degrees(double deg) {
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

double decode_angle(uint32_t raw, int bits) {
    const double denom = bits == 24 ? 16777216.0 : 65536.0;
    double degrees = (static_cast<double>(raw) / denom) * 360.0;
    if (degrees > 180.0) {
        degrees -= 360.0;
    }
    return degrees;
}

uint32_t encode_angle(double degrees, int bits) {
    const double denom = bits == 24 ? 16777216.0 : 65536.0;
    double wrapped = wrap_degrees(degrees);
    double fraction = wrapped / 360.0;
    uint32_t raw = static_cast<uint32_t>(std::round(fraction * denom));
    if (raw >= static_cast<uint32_t>(denom)) {
        raw = 0;
    }
    return raw;
}

double decode_ra_hours(uint32_t raw, int bits) {
    const double denom = bits == 24 ? 16777216.0 : 65536.0;
    double hours = (static_cast<double>(raw) / denom) * 24.0;
    if (hours >= 24.0) {
        hours -= 24.0;
    }
    return hours;
}

uint32_t encode_ra_raw(double hours, int bits) {
    double wrapped_hours = std::fmod(hours, 24.0);
    if (wrapped_hours < 0.0) {
        wrapped_hours += 24.0;
    }
    return encode_angle(wrapped_hours * kHoursToDegrees, bits);
}

struct LocalTimeInfo {
    int offset_minutes = 0;
    bool dst = false;
};

LocalTimeInfo compute_local_timezone_info(std::time_t base_time) {
    LocalTimeInfo info{};
    std::tm local_tm {};
    std::tm utc_tm {};
#ifdef _WIN32
    localtime_s(&local_tm, &base_time);
    gmtime_s(&utc_tm, &base_time);
#else
    local_tm = *std::localtime(&base_time);
    utc_tm = *std::gmtime(&base_time);
#endif
    std::time_t local_time = std::mktime(&local_tm);
    std::time_t utc_as_local = std::mktime(&utc_tm);
    double offset_seconds = std::difftime(local_time, utc_as_local);
    info.offset_minutes = static_cast<int>(std::round(offset_seconds / 60.0));
    info.dst = local_tm.tm_isdst > 0;
    return info;
}

// SynScan V3/V4 hand controller model IDs (from "m" command).
// Only includes mounts that ship with the V3/V4 handset.
std::string synscan_model_id_to_name(int model_id) {
    switch (model_id) {
        case 0:  return "EQ6 Pro";
        case 1:  return "HEQ5 Pro";
        case 2:  return "EQ5";
        case 3:  return "EQ3";
        case 4:  return "EQ8";
        case 5:  return "AZ-EQ6";
        case 6:  return "AZ-EQ5";
        case 56: return "HEQ5 Pro";
        case 160: return "AllView";
        default:
            if (model_id >= 128 && model_id <= 143) return "AZ GOTO";
            if (model_id >= 144 && model_id <= 159) return "DOB GOTO";
            return "Mount (ID " + std::to_string(model_id) + ")";
    }
}

} // namespace

class SynScanTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    SynScanTelescopeDriver(int device_number, const ConnectionInfo& connection_info, SynScanVersion version,
                           std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                           std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect)
        : AsyncConnectable("SynScan"),
          device_number_(device_number),
          connection_info_(connection_info),
          version_(version),
          connected_(false),
          target_ra_hours_(0.0),
          target_dec_degrees_(0.0),
          aperture_diameter_m_(0.0),
          aperture_area_m2_(0.0),
          focal_length_m_(0.0),
          site_latitude_cached_(0.0),
          site_longitude_cached_(0.0),
          site_info_valid_(false),
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          timezone_offset_minutes_(0),
          timezone_offset_valid_(false),
          dst_observed_(false),
          last_utc_set_{},
          last_utc_set_monotonic_(std::chrono::steady_clock::now()),
          last_utc_valid_(false),
          tracking_mode_cached_(0),
          tracking_mode_valid_(false),
          parked_(false),
          at_home_(false),
          mount_model_id_(-1),
          use_precise_commands_(version_ != SynScanVersion::V3),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~SynScanTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // Cancel + join the background slew/pulse task threads before any
        // member they touch is destroyed. Must run WITHOUT mutex_ held (the
        // task threads take mutex_).
        cancel_async_tasks();
        if (connected_) {
            try {
                set_connected(false);
            } catch (...) {
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        if (mount_model_id_ >= 0) {
            return "Sky-Watcher " + synscan_model_id_to_name(mount_model_id_);
        }
        return "Sky-Watcher SynScan Mount";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string get_unique_id() const override {
        return "SynScan_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "Sky-Watcher SynScan V3/V4 Mount Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore SynScan Driver v0.1";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Handset firmware captured at connect; surfaced in the web UI only. Guarded
    // by its OWN narrow firmware_mutex_, NOT the coarse mutex_ that set_connected()
    // holds across the multi-second connect, so a configureddevices poll never
    // blocks on the mount connection.
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

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        if (!connected) {
            // Cancel + join the background slew/pulse task threads BEFORE
            // taking mutex_: the task threads take mutex_, so joining under
            // the lock would deadlock, and leaving them running across the
            // protocol disconnect would race the shared wrapper.
            cancel_async_tasks();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }

        auto& protocol = SynScanProtocolWrapper::instance();
        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to SynScan mount");
            }
            connected_ = true;
            mount_firmware_version_ = "";
            mount_model_id_ = -1;
            site_info_valid_ = false;
            timezone_offset_valid_ = false;
            tracking_mode_valid_ = false;
            target_set_ = false;
            parked_ = false;
            parking_ = false;
            at_home_ = false;
            pulse_guiding_active_ = false;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            last_utc_valid_ = false;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();

            try {
                mount_firmware_version_ = protocol.get_handset_firmware_version();
            } catch (...) {
                // TODO: Confirm SynScan firmware query reliability on all V3/V4 handsets.
            }
            // Publish to the web-UI getter under its OWN narrow mutex so a
            // configureddevices poll never blocks on the coarse mutex_ this
            // connect sequence holds for several seconds.
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = (mount_firmware_version_ != "0.0.0") ? mount_firmware_version_ : std::string();
            }
            try {
                mount_model_id_ = protocol.get_model_id();
            } catch (...) {
                // TODO: Confirm SynScan model query reliability on all V3/V4 handsets.
            }

            if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value()) {
                LocationInfo loc;
                loc.latitude_degrees = pending_site_latitude_.value();
                loc.longitude_degrees = pending_site_longitude_.value();
                try {
                    protocol.set_location(loc);
                    site_latitude_cached_ = loc.latitude_degrees;
                    site_longitude_cached_ = loc.longitude_degrees;
                    site_info_valid_ = true;
                } catch (...) {
                    // TODO: Confirm SynScan accepts location updates while aligned.
                }
            }
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            if (sync_time_on_connect_) {
                sync_mount_time_locked();
            }
            // Warm caches so first property reads stay within Conform fast-time targets.
            try {
                auto raw = protocol.get_ra_dec_raw(use_precise_commands_);
                int bits = use_precise_commands_ ? 24 : 16;
                cached_ra_hours_ = decode_ra_hours(raw.first, bits);
                cached_dec_degrees_ = decode_angle(raw.second, bits);
                equatorial_cache_valid_ = true;
                last_equatorial_update_ = std::chrono::steady_clock::now();
            } catch (...) {
            }
            try {
                auto raw = protocol.get_alt_az_raw(use_precise_commands_);
                int bits = use_precise_commands_ ? 24 : 16;
                cached_az_degrees_ = wrap_degrees(decode_angle(raw.first, bits));
                cached_alt_degrees_ = decode_angle(raw.second, bits);
                altaz_cache_valid_ = true;
                last_altaz_update_ = std::chrono::steady_clock::now();
            } catch (...) {
            }
            try {
                LocationInfo info = protocol.get_location();
                site_latitude_cached_ = info.latitude_degrees;
                site_longitude_cached_ = info.longitude_degrees;
                site_info_valid_ = true;
            } catch (...) {
            }
        } else {
            protocol.disconnect();
            connected_ = false;
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_.clear();
            }
            target_set_ = false;
            parked_ = false;
            parking_ = false;
            at_home_ = false;
            pulse_guiding_active_ = false;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name));
    }

    bool can_action(std::string_view action_name) const override {
        (void)action_name;
        return false;
    }

    std::string command_blind(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            protocol.send_command_blind(std::string(command));
        } else {
            protocol.send_command_blind(std::string(command));
        }
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return response == "1";
        }
        protocol.send_command_blind(std::string(command));
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            return protocol.send_command(std::string(command));
        }
        return protocol.send_command(std::string(command));
    }

    AlignmentMode get_alignment_mode() const override {
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_altaz_cache_locked();
        return cached_alt_degrees_;
    }

    double get_aperture_diameter() const override {
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
        }
        aperture_diameter_m_ = meters;
        if (meters > 0.0) {
            double radius = meters / 2.0;
            aperture_area_m2_ = radius * radius * std::numbers::pi;
        } else {
            aperture_area_m2_ = 0.0;
        }
    }

    double get_aperture_area() const override {
        return aperture_area_m2_;
    }

    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return at_home_;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return parked_;
    }

    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_altaz_cache_locked();
        return cached_az_degrees_;
    }

    bool get_can_find_home() const override {
        return false;
    }

    bool get_can_park() const override {
        return true;
    }

    bool get_can_pulse_guide() const override {
        return true;
    }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (pulse_guiding_active_ && std::chrono::steady_clock::now() >= pulse_guide_end_time_) {
            pulse_guiding_active_ = false;
        }
        return pulse_guiding_active_;
    }

    bool get_can_set_declination_rate() const override {
        return false;
    }

    bool get_can_set_guide_rates() const override {
        return true;
    }

    bool get_can_set_park() const override {
        return true;
    }

    bool get_can_set_pier_side() const override {
        return false;
    }

    bool get_can_set_right_ascension_rate() const override {
        return false;
    }

    bool get_can_set_tracking() const override {
        return true;
    }

    bool get_can_slew_alt_az() const override {
        return false;
    }

    bool get_can_slew_alt_az_async() const override {
        return false;
    }

    bool get_can_sync_alt_az() const override {
        return false;
    }

    bool get_can_slew() const override {
        return true;
    }

    bool get_can_slew_async() const override {
        return true;
    }

    bool get_can_sync() const override {
        return true;
    }

    bool get_can_unpark() const override {
        return true;
    }

    double get_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (target_set_ && std::chrono::steady_clock::now() < position_override_until_ && !get_slewing_locked()) {
            return std::clamp(target_dec_degrees_, -90.0, 90.0);
        }
        refresh_equatorial_cache_locked();
        return std::clamp(cached_dec_degrees_, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        return 0.0;
    }

    void set_declination_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Declination rate not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return get_tracking_locked();
    }

    void set_tracking(bool tracking) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (tracking) {
            check_not_parked_locked("Tracking");
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        if (tracking) {
            // TODO: Confirm correct SynScan tracking mode for specific mount types.
            protocol.set_tracking_mode(2);
            tracking_mode_cached_ = 2;
        } else {
            protocol.set_tracking_mode(0);
            tracking_mode_cached_ = 0;
        }
        tracking_mode_valid_ = true;
    }

    double get_focal_length() const override {
        return focal_length_m_;
    }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double ra_percent = (rate.ra / kSiderealDegPerSec) * 100.0;
        double dec_percent = (rate.dec / kSiderealDegPerSec) * 100.0;
        if (ra_percent < 0.0 || ra_percent > 100.0 || dec_percent < 0.0 || dec_percent > 100.0) {
            throw AlpacaException("Guide rate must be between 0 and 1x sidereal",
                                  AlpacaError::InvalidValue);
        }
        guide_rate_ = rate;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (target_set_ && std::chrono::steady_clock::now() < position_override_until_ && !get_slewing_locked()) {
            return target_ra_hours_;
        }
        refresh_equatorial_cache_locked();
        return cached_ra_hours_;
    }

    double get_right_ascension_rate() const override {
        return 0.0;
    }

    void set_right_ascension_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Right ascension rate not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        try {
            char side = SynScanProtocolWrapper::instance().get_pointing_state();
            side_of_pier_cached_ = map_pointing_state_to_side(side);
            side_of_pier_valid_ = side_of_pier_cached_ >= 0;
        } catch (...) {
            if (!side_of_pier_valid_) {
                throw;
            }
        }
        return side_of_pier_cached_;
    }

    void set_side_of_pier(int side) override {
        (void)side;
        throw AlpacaException("Pier side not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        (void)dec;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
        }
        if (!site_info_valid_) {
            return side_of_pier_valid_ ? side_of_pier_cached_ : -1;
        }
        double lst_hours = compute_local_sidereal_time_hours(std::chrono::system_clock::now(),
                                                            site_longitude_cached_);
        double hour_angle = shortest_ra_delta_hours(lst_hours, ra);
        return hour_angle >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::J2000;
    }

    bool get_does_refraction() const override {
        return does_refraction_;
    }

    void set_does_refraction(bool does_refraction) override {
        does_refraction_ = does_refraction;
    }

    int get_slew_settle_time() const override {
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
        }
        return compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_cached_);
    }

    double get_site_elevation() const override {
        return site_elevation_m_;
    }

    void set_site_elevation(double elevation) override {
        if (elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters",
                                  AlpacaError::InvalidValue);
        }
        site_elevation_m_ = elevation;
    }

    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
        }
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        if (latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        LocationInfo info = current_location_locked();
        info.latitude_degrees = latitude;
        SynScanProtocolWrapper::instance().set_location(info);
        site_latitude_cached_ = latitude;
        site_longitude_cached_ = info.longitude_degrees;
        site_info_valid_ = true;
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
        }
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        if (longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        LocationInfo info = current_location_locked();
        info.longitude_degrees = longitude;
        SynScanProtocolWrapper::instance().set_location(info);
        site_latitude_cached_ = info.latitude_degrees;
        site_longitude_cached_ = longitude;
        site_info_valid_ = true;
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
            throw AlpacaException("TargetDeclination must be in range -90 to 90 degrees",
                                  AlpacaError::InvalidValue);
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
            throw AlpacaException("TargetRightAscension must be in range 0 to <24 hours",
                                  AlpacaError::InvalidValue);
        }
        target_ra_hours_ = ra;
        target_set_ = true;
    }

    int get_tracking_rate() const override {
        return 0;
    }

    void set_tracking_rate(int rate) override {
        (void)rate;
        throw AlpacaException("Tracking rates not supported", AlpacaError::PropertyNotImplemented);
    }

    std::vector<int> get_tracking_rates() const override {
        return {0};
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!last_utc_valid_) {
            try {
                TimeInfo info = SynScanProtocolWrapper::instance().get_time();
                timezone_offset_minutes_ = info.timezone_offset_minutes;
                timezone_offset_valid_ = true;
                dst_observed_ = info.dst_enabled;

                using namespace std::chrono;
                int year = 2000 + info.year;
                sys_days date = sys_days{std::chrono::year{year} /
                                         std::chrono::month{static_cast<unsigned>(info.month)} /
                                         std::chrono::day{static_cast<unsigned>(info.day)}};
                auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
                int total_offset = info.timezone_offset_minutes + (info.dst_enabled ? 60 : 0);
                last_utc_set_ = local_time - minutes{total_offset};
            } catch (...) {
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

private:
    // Body of set_utc_date with mutex_ already held — called from the connect
    // path (sync_mount_time_locked), which holds mutex_; calling the public
    // locking method there would self-deadlock on the non-recursive mutex.
    void set_utc_date_locked(std::chrono::system_clock::time_point utc) {
        check_connected();
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);
        LocalTimeInfo tz_info = compute_local_timezone_info(utc_time_t);

        std::tm local_tm {};
#ifdef _WIN32
        localtime_s(&local_tm, &utc_time_t);
#else
        local_tm = *std::localtime(&utc_time_t);
#endif

        TimeInfo info;
        info.hour = local_tm.tm_hour;
        info.minute = local_tm.tm_min;
        info.second = local_tm.tm_sec;
        info.month = local_tm.tm_mon + 1;
        info.day = local_tm.tm_mday;
        info.year = (local_tm.tm_year + 1900) % 100;
        info.timezone_offset_minutes = tz_info.offset_minutes;
        info.dst_enabled = tz_info.dst;

        SynScanProtocolWrapper::instance().set_time(info);
        timezone_offset_minutes_ = tz_info.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = tz_info.dst;
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
    }

public:
    void find_home() override {
        throw AlpacaException("FindHome not supported", AlpacaError::MethodNotImplemented);
    }

    // Park is an asynchronous initiator (ITelescopeV4; ConformU 4.5 times it
    // against the 1 s STANDARD target): the park slew is dispatched in the
    // slew task thread and this call returns at once. Slewing reports true
    // until the mount reaches the park position and tracking is stopped, at
    // which point AtPark flips true in the same locked step (no window where
    // a poller sees Slewing false with AtPark false). Issue #208.
    void park() override {
        double park_ra = 0.0;
        double park_dec = 0.0;
        // Cancel + join any previous slew task first. Must run without mutex_
        // held: the task takes mutex_.
        reap_slew_task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // ASCOM: Park on a parked (or parking) mount is harmless.
            }
            if (!park_position_set_) {
                refresh_equatorial_cache_locked();
                park_ra_hours_ = cached_ra_hours_;
                park_dec_degrees_ = cached_dec_degrees_;
                park_position_set_ = true;
            }
            park_ra = park_ra_hours_;
            park_dec = park_dec_degrees_;
            validate_ra_dec(park_ra, park_dec, "Park");
            // Publish the slewing state before the task starts so a poller
            // never sees Slewing false between Park returning and dispatch.
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            at_home_ = false;
            parking_ = true;
        }

        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (slew_task_thread_.joinable()) {
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            slew_task_thread_.join();
            slew_task_cancel_.store(false);
        }
        slew_task_thread_ = std::thread([this, park_ra, park_dec]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load() || !parking_) {
                parking_ = false;
                return;
            }
            try {
                do_slew_to_coordinates_locked(park_ra, park_dec);
            } catch (const std::exception& ex) {
                fail_park_locked(std::string("Park slew dispatch failed: ") + ex.what());
                return;
            } catch (...) {
                fail_park_locked("Park slew dispatch failed with unknown exception");
                return;
            }
            // Poll for completion with mutex_ released between polls so the
            // HTTP getters (Slewing, RightAscension, ...) stay responsive.
            const auto timeout = std::chrono::seconds(120);
            const auto start = std::chrono::steady_clock::now();
            const auto start_grace = std::chrono::seconds(2);
            bool saw_slewing = false;
            while (true) {
                lock.unlock();
                const bool keep_going = task_wait_for(std::chrono::milliseconds(250), slew_task_cancel_);
                lock.lock();
                // Cancelled (disconnect / destruction / superseding slew),
                // aborted, or unparked meanwhile: the canceller owns the state.
                if (!keep_going || !connected_ || !parking_) {
                    parking_ = false;
                    return;
                }
                const bool slewing = poll_hardware_slewing_locked();
                if (slewing) {
                    saw_slewing = true;
                } else {
                    if (!saw_slewing && (std::chrono::steady_clock::now() - start) < start_grace) {
                        continue;
                    }
                    break;
                }
                if (std::chrono::steady_clock::now() - start > timeout) {
                    fail_park_locked("Park slew timed out after 120s");
                    return;
                }
            }
            if (slew_settle_time_seconds_ > 0) {
                lock.unlock();
                const bool keep_going =
                    task_wait_for(std::chrono::seconds(slew_settle_time_seconds_), slew_task_cancel_);
                lock.lock();
                if (!keep_going || !connected_ || !parking_) {
                    parking_ = false;
                    return;
                }
            }
            try {
                SynScanProtocolWrapper::instance().set_tracking_mode(0);
                tracking_mode_cached_ = 0;
                tracking_mode_valid_ = true;
            } catch (const std::exception& ex) {
                fail_park_locked(std::string("Park: stopping tracking failed: ") + ex.what());
                return;
            } catch (...) {
                fail_park_locked("Park: stopping tracking failed with unknown exception");
                return;
            }
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            position_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            // AtPark and Slewing flip in the same locked step.
            parked_ = true;
            parking_ = false;
        });
    }

    // Park task failure path: drop the parking state so Slewing/AtPark both
    // read false and the caller can retry. mutex_ must be held.
    void fail_park_locked(const std::string& message) {
        parking_ = false;
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        ALPACA_LOG_WARN("SynScan", message);
    }

    void pulse_guide(int direction, int duration) override {
        int axis = -1;
        int tracking_mode = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");

            if (duration < 0) {
                throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
            }

            auto& protocol = SynScanProtocolWrapper::instance();

            double slew_rate_deg_per_sec = 0.0;

            switch (direction) {
                case 0:
                    axis = 1;
                    slew_rate_deg_per_sec = guide_rate_.dec;
                    break;
                case 1:
                    axis = 1;
                    slew_rate_deg_per_sec = -guide_rate_.dec;
                    break;
                case 2:
                    axis = 0;
                    slew_rate_deg_per_sec = -guide_rate_.ra;
                    break;
                case 3:
                    axis = 0;
                    slew_rate_deg_per_sec = guide_rate_.ra;
                    break;
                default:
                    throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
            }

            if (axis == 1) {
                char pier = protocol.get_pointing_state();
                if (pier == 'W') {
                    slew_rate_deg_per_sec = -slew_rate_deg_per_sec;
                }
            }

            const double duration_sec = duration / 1000.0;
            const auto now = std::chrono::steady_clock::now();

            // Initialize target coords from mount if position override isn't active
            if (!target_set_ || now >= position_override_until_) {
                refresh_equatorial_cache_locked();
                target_ra_hours_ = cached_ra_hours_;
                target_dec_degrees_ = cached_dec_degrees_;
                target_set_ = true;
            }

            // Accumulate expected pulse delta directly into target coordinates
            if (direction == 0 || direction == 1) {
                double delta_deg = guide_rate_.dec * duration_sec;
                if (direction == 1) delta_deg = -delta_deg;
                target_dec_degrees_ = std::clamp(target_dec_degrees_ + delta_deg, -90.0, 90.0);
            } else {
                double delta_hours = (guide_rate_.ra * duration_sec) / 15.0;
                if (direction == 3) delta_hours = -delta_hours;
                target_ra_hours_ = std::fmod(target_ra_hours_ + delta_hours, 24.0);
                if (target_ra_hours_ < 0.0) target_ra_hours_ += 24.0;
            }

            // Keep position override active so get_ra/get_dec return target coords
            position_override_until_ =
                now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay + kPulseGuidePositionGrace;

            protocol.move_axis_variable_rate(axis, slew_rate_deg_per_sec);

            pulse_guiding_active_ = true;
            pulse_guide_end_time_ = now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;

            tracking_mode = tracking_mode_cached_;
        }

        // Stop-the-pulse timer: a joinable member thread (never detached),
        // cancelled + joined on disconnect and in the destructor. Spawned
        // OUTSIDE mutex_ because its failure path takes mutex_.
        reap_pulse_task();
        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (pulse_task_thread_.joinable()) {
            // A racing caller spawned between our reap and this lock — cancel
            // and join it INSIDE the same critical section as the assignment,
            // so a joinable thread can never be overwritten (std::terminate)
            // or joined from two threads (UB). [stress] telescope finding.
            pulse_task_cancel_.store(true);
            task_cv_.notify_all();
            pulse_task_thread_.join();
            pulse_task_cancel_.store(false);
        }
        pulse_task_thread_ = std::thread([this, axis, duration, tracking_mode]() {
            if (!task_wait_for(std::chrono::milliseconds(duration), pulse_task_cancel_)) {
                // Cancelled by disconnect/destruction. Still make a best-effort
                // attempt to stop the axis before exiting — the mount would
                // otherwise keep slewing.
                try {
                    SynScanProtocolWrapper::instance().move_axis_variable_rate(axis, 0.0);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Cancellation path (disconnect/destruction) — the caller
                    // is tearing the connection down; nothing more to do.
                }
                return;
            }
            // The stop command MUST land: silently swallowing a failure leaves
            // the axis slewing at guide rate indefinitely (mount runaway).
            // Retry a few times; on final failure log loudly and flag the axis
            // as still moving so Slewing reports the true state.
            constexpr int kStopAttempts = 3;
            bool stopped = false;
            std::string last_error;
            auto& proto = SynScanProtocolWrapper::instance();
            for (int attempt = 0; attempt < kStopAttempts && !stopped; ++attempt) {
                try {
                    proto.move_axis_variable_rate(axis, 0.0);
                    stopped = true;
                } catch (const std::exception& e) {
                    last_error = e.what();
                } catch (...) {
                    last_error = "unknown error";
                }
                if (!stopped && !task_wait_for(std::chrono::milliseconds(100), pulse_task_cancel_)) {
                    break;
                }
            }
            if (stopped) {
                if (axis == 0 && tracking_mode > 0) {
                    try {
                        proto.set_tracking_mode(tracking_mode);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("SynScan", std::string("PulseGuide: failed to restore tracking: ") + e.what());
                    } catch (...) {
                        ALPACA_LOG_WARN("SynScan", "PulseGuide: failed to restore tracking");
                    }
                }
            } else {
                ALPACA_LOG_ERROR("SynScan", "PulseGuide STOP FAILED after " + std::to_string(kStopAttempts) +
                                                " attempts on axis " + std::to_string(axis) +
                                                " — axis may still be moving (mount runaway risk): " + last_error);
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_active_ = false;
                // Surface the error state: report the axis as slewing until an
                // AbortSlew/MoveAxis(0) or disconnect clears it.
                if (connected_) {
                    manual_axis_slewing_[axis] = true;
                }
            }
        });
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_equatorial_cache_locked();
        park_ra_hours_ = cached_ra_hours_;
        park_dec_degrees_ = cached_dec_degrees_;
        park_position_set_ = true;
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinates");
        do_slew_to_coordinates_locked(ra, dec);
        wait_for_slew_complete(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        uint32_t ra_raw = 0;
        uint32_t dec_raw = 0;
        bool precise = false;
        // Cancel + join any previous slew dispatch task first. Must run
        // without mutex_ held: the task takes mutex_.
        reap_slew_task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");

            int bits = use_precise_commands_ ? 24 : 16;
            ra_raw = encode_ra_raw(ra, bits);
            dec_raw = encode_angle(dec, bits);
            precise = use_precise_commands_;

            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_set_ = true;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
        }

        // Dispatch in a joinable member thread (never detached), cancelled +
        // joined on disconnect and in the destructor.
        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (slew_task_thread_.joinable()) {
            // Racing SlewAsync between reap and this lock — see the pulse
            // spawn above; join under the same critical section.
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            slew_task_thread_.join();
            slew_task_cancel_.store(false);
        }
        slew_task_thread_ = std::thread([this, ra_raw, dec_raw, precise]() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                return;
            }
            try {
                SynScanProtocolWrapper::instance().goto_ra_dec_raw(ra_raw, dec_raw, precise);
            } catch (const std::exception& ex) {
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
                ALPACA_LOG_WARN("SynScan", std::string("Async slew dispatch failed: ") + ex.what());
            } catch (...) {
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
                ALPACA_LOG_WARN("SynScan", "Async slew dispatch failed with unknown exception");
            }
        });
    }

    void slew_to_target() override {
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
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t ra_raw = encode_ra_raw(ra, bits);
        uint32_t dec_raw = encode_angle(dec, bits);
        protocol.sync_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_);
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        position_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }

    void sync_to_target() override {
        if (!target_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        sync_to_coordinates(target_ra_hours_, target_dec_degrees_);
    }

    void unpark() override {
        bool was_parking = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            parked_ = false;
            was_parking = parking_;
            if (was_parking) {
                // Unpark during a park wins the race: stop the park slew and
                // drop the parking state; the task below is then joined.
                parking_ = false;
                auto& protocol = SynScanProtocolWrapper::instance();
                try {
                    protocol.cancel_goto();
                    protocol.move_axis_fixed_rate(0, 0);
                    protocol.move_axis_fixed_rate(1, 0);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                }
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
            }
        }
        if (was_parking) {
            reap_slew_task();  // without mutex_ held
        }
    }

    bool get_can_move_axis(int axis) const override {
        return axis == 0 || axis == 1;
    }

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
            throw AlpacaException("MoveAxis rate exceeds supported range",
                                  AlpacaError::InvalidValue);
        }

        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;
        manual_axis_slewing_[axis] = moving;
        if (moving) {
            parked_ = false;
            at_home_ = false;
        }

        SynScanProtocolWrapper::instance().move_axis_variable_rate(axis, moving ? rate : 0.0);
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            // Tertiary axis is not supported; return an empty range set per ASCOM semantics.
            return {};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("AbortSlew");
        auto& protocol = SynScanProtocolWrapper::instance();
        protocol.cancel_goto();
        protocol.move_axis_fixed_rate(0, 0);
        protocol.move_axis_fixed_rate(1, 0);
        parking_ = false;  // an aborted park never reaches AtPark
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
    }

    void slew_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SlewToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SlewToAltAzAsync not supported", AlpacaError::MethodNotImplemented);
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

private:
    static int map_pointing_state_to_side(char side) {
        // SynScan 'W' = pointing west → OTA east of pier (HA > 0) → ASCOM pierEast (0).
        // SynScan 'E' = pointing east → OTA west of pier (HA < 0) → ASCOM pierWest (1).
        // TODO: Adjust mapping for southern hemisphere per SynScan pointing-state rules.
        if (side == 'W') {
            return 0;
        }
        if (side == 'E') {
            return 1;
        }
        return -1;
    }

    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to SynScan mount");
        }
    }

    // ── Background task threads (async slew dispatch, pulse-guide stop) ──
    // Never detached: each is a joinable member thread with a cancel flag +
    // condition_variable, cancelled and joined in the destructor and on
    // disconnect so a wakeup can never touch a destroyed/disconnected driver.

    // Interruptible sleep for a task thread. Returns false if cancelled.
    bool task_wait_for(std::chrono::milliseconds d, std::atomic<bool>& cancel) const {
        std::unique_lock<std::mutex> tlock(task_mutex_);
        task_cv_.wait_for(tlock, d, [&] { return cancel.load(); });
        return !cancel.load();
    }

    // Cancel and join both task threads. Must be called WITHOUT mutex_ held
    // (both task threads take mutex_).
    void cancel_async_tasks() {
        slew_task_cancel_.store(true);
        pulse_task_cancel_.store(true);
        task_cv_.notify_all();
        std::thread slew_thread;
        std::thread pulse_thread;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            slew_thread = std::move(slew_task_thread_);
            pulse_thread = std::move(pulse_task_thread_);
        }
        if (slew_thread.joinable()) {
            slew_thread.join();
        }
        if (pulse_thread.joinable()) {
            pulse_thread.join();
        }
    }

    // Join the previous slew task (if any) and reset its cancel flag so a new
    // one can start. Must be called WITHOUT mutex_ held (see above).
    void reap_slew_task() {
        slew_task_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(slew_task_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        slew_task_cancel_.store(false);
    }

    // Join the previous pulse-stop task (if any) and reset its cancel flag.
    // Must be called WITHOUT mutex_ held (its failure path takes mutex_).
    void reap_pulse_task() {
        pulse_task_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(pulse_task_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        pulse_task_cancel_.store(false);
    }

    void check_not_parked_locked(const char* operation) const {
        if (parked_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    void refresh_equatorial_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (equatorial_cache_valid_ && (now - last_equatorial_update_) < kPositionCacheTtl) {
            return;
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        try {
            auto raw = protocol.get_ra_dec_raw(use_precise_commands_);
            cached_ra_hours_ = decode_ra_hours(raw.first, bits);
            cached_dec_degrees_ = decode_angle(raw.second, bits);
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
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        try {
            auto raw = protocol.get_alt_az_raw(use_precise_commands_);
            cached_az_degrees_ = wrap_degrees(decode_angle(raw.first, bits));
            cached_alt_degrees_ = decode_angle(raw.second, bits);
            altaz_cache_valid_ = true;
            last_altaz_update_ = now;
        } catch (...) {
            if (!altaz_cache_valid_) {
                throw;
            }
        }
    }

    bool get_slewing_locked() const {
        // A park in flight reports Slewing until the park task flips AtPark
        // (same locked step) — never Slewing false with AtPark false.
        if (parking_) {
            return true;
        }
        return poll_hardware_slewing_locked();
    }

    bool poll_hardware_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (std::chrono::steady_clock::now() < slew_force_until_) {
            return true;
        }
        bool was_slewing = slewing_cached_;
        try {
            slewing_cached_ = SynScanProtocolWrapper::instance().is_goto_in_progress();
        } catch (...) {
            // Keep last known state if polling times out.
        }
        if (was_slewing && !slewing_cached_) {
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            position_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
        return slewing_cached_;
    }

    bool get_tracking_locked() const {
        if (!tracking_mode_valid_) {
            tracking_mode_cached_ = SynScanProtocolWrapper::instance().get_tracking_mode();
            tracking_mode_valid_ = true;
        }
        return tracking_mode_cached_ != 0;
    }

    void ensure_site_info_cached_locked() const {
        if (!connected_) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (!site_info_valid_ &&
            last_site_info_attempt_ != std::chrono::steady_clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSiteInfoRetryDelay) {
            return;
        }
        last_site_info_attempt_ = now;
        try {
            LocationInfo info = SynScanProtocolWrapper::instance().get_location();
            site_latitude_cached_ = info.latitude_degrees;
            site_longitude_cached_ = info.longitude_degrees;
            site_info_valid_ = true;
        } catch (...) {
            site_info_valid_ = false;
        }
    }

    LocationInfo current_location_locked() const {
        LocationInfo info;
        if (site_info_valid_) {
            info.latitude_degrees = site_latitude_cached_;
            info.longitude_degrees = site_longitude_cached_;
            return info;
        }
        return SynScanProtocolWrapper::instance().get_location();
    }

    void do_slew_to_coordinates_locked(double ra, double dec) {
        validate_ra_dec(ra, dec, "SlewToCoordinates");
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t ra_raw = encode_ra_raw(ra, bits);
        uint32_t dec_raw = encode_angle(dec, bits);
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        slewing_cached_ = true;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        protocol.goto_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_);
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parked_ = false;
        at_home_ = false;
    }

    void do_slew_to_altaz_locked(double altitude, double azimuth) {
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t az_raw = encode_angle(azimuth, bits);
        uint32_t alt_raw = encode_angle(altitude, bits);
        protocol.goto_alt_az_raw(az_raw, alt_raw, use_precise_commands_);
        parked_ = false;
        at_home_ = false;
    }

    // Poll for slew completion, RELEASING the driver mutex around every sleep
    // so a sync slew/park doesn't block all GETs and disconnect for up to
    // 120 s (Bisque's unlock/sleep/relock loop is the reference pattern).
    // `lock` must be held on entry; it is held again on return/throw. The
    // connection state is re-checked after each relock.
    void wait_for_slew_complete(std::unique_lock<std::mutex>& lock) const {
        const auto timeout = std::chrono::seconds(120);
        auto start = std::chrono::steady_clock::now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            std::this_thread::sleep_for(d);
            lock.lock();
            // The mount may have been disconnected while the lock was released.
            check_connected();
        };
        while (true) {
            bool slewing = get_slewing_locked();
            if (slewing) {
                saw_slewing = true;
            }
            if (!slewing) {
                if (!saw_slewing && (std::chrono::steady_clock::now() - start) < start_grace) {
                    sleep_unlocked(std::chrono::milliseconds(200));
                    continue;
                }
                break;
            }
            if (std::chrono::steady_clock::now() - start > timeout) {
                throw AlpacaException("Slew timed out");
            }
            sleep_unlocked(std::chrono::milliseconds(250));
        }
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        position_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        if (slew_settle_time_seconds_ > 0) {
            sleep_unlocked(std::chrono::seconds(slew_settle_time_seconds_));
        }
    }

    void sync_mount_time_locked() {
        auto now_utc = std::chrono::system_clock::now();
        // mutex_ is already held here — call the _locked body, not the public
        // locking method (non-recursive mutex would self-deadlock).
        set_utc_date_locked(now_utc);
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

    static double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time,
                                                    double longitude_degrees) {
        using namespace std::chrono;
        double days_since_epoch = duration_cast<seconds>(utc_time.time_since_epoch()).count() / 86400.0;
        double jd = 2440587.5 + days_since_epoch;
        double t = (jd - 2451545.0) / 36525.0;
        double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                      + 0.000387933 * t * t - (t * t * t) / 38710000.0;
        double lst = gmst + longitude_degrees;
        lst = std::fmod(lst, 360.0);
        if (lst < 0.0) {
            lst += 360.0;
        }
        return lst / kHoursToDegrees;
    }

    static double shortest_ra_delta_hours(double a, double b) {
        double delta = a - b;
        while (delta > 12.0) {
            delta -= 24.0;
        }
        while (delta < -12.0) {
            delta += 24.0;
        }
        return delta;
    }

    static void validate_ra_dec(double ra, double dec, const char* context) {
        if (ra < 0.0 || ra >= 24.0) {
            throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
        }
        if (dec < -90.0 || dec > 90.0) {
            throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
        }
    }

    int device_number_;
    ConnectionInfo connection_info_;
    SynScanVersion version_;
    mutable std::mutex mutex_;
    bool connected_;

    double target_ra_hours_;
    double target_dec_degrees_;
    double aperture_diameter_m_;
    double aperture_area_m2_;
    double focal_length_m_;
    mutable double cached_ra_hours_ = 0.0;
    mutable double cached_dec_degrees_ = 0.0;
    mutable double cached_alt_degrees_ = 0.0;
    mutable double cached_az_degrees_ = 0.0;
    mutable bool equatorial_cache_valid_ = false;
    mutable bool altaz_cache_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_equatorial_update_;
    mutable std::chrono::steady_clock::time_point last_altaz_update_;

    mutable double site_latitude_cached_;
    mutable double site_longitude_cached_;
    mutable bool site_info_valid_;
    mutable std::chrono::steady_clock::time_point last_site_info_attempt_;
    double site_elevation_m_;
    mutable int timezone_offset_minutes_;
    mutable bool timezone_offset_valid_;
    mutable bool dst_observed_;
    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;
    mutable int tracking_mode_cached_;
    mutable bool tracking_mode_valid_;
    mutable bool target_set_ = false;
    mutable bool parked_;
    mutable bool at_home_;
    mutable bool slewing_cached_ = false;
    mutable std::chrono::steady_clock::time_point slew_force_until_;
    mutable std::chrono::steady_clock::time_point position_override_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    mutable int side_of_pier_cached_ = -1;
    mutable bool side_of_pier_valid_ = false;
    std::string mount_firmware_version_;
    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so the
    // get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;
    int mount_model_id_;
    bool use_precise_commands_;
    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
    GuideRate guide_rate_{};
    mutable bool pulse_guiding_active_ = false;
    mutable std::chrono::steady_clock::time_point pulse_guide_end_time_;

    bool park_position_set_ = false;
    mutable bool parking_ = false;  // park task in flight (Slewing true, AtPark false)
    double park_ra_hours_ = 0.0;
    double park_dec_degrees_ = 0.0;

    // Background task threads — see the helpers above. task_mutex_ only guards
    // thread handles and the cv; it is never held across protocol I/O.
    mutable std::mutex task_mutex_;
    mutable std::condition_variable task_cv_;
    std::thread slew_task_thread_;
    std::thread pulse_task_thread_;
    mutable std::atomic<bool> slew_task_cancel_{false};
    mutable std::atomic<bool> pulse_task_cancel_{false};
};

std::unique_ptr<TelescopeDriver> create_synscan_telescope(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version) {
    return create_synscan_telescope_with_site(device_number, connection_info, version, std::nullopt, std::nullopt,
                                              std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {
    return std::make_unique<SynScanTelescopeDriver>(device_number, connection_info, version,
                                                    site_latitude_deg, site_longitude_deg,
                                                    site_elevation_m, sync_time_on_connect);
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_auto(
    int device_number,
    int mount_index,
    SynScanVersion version,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {

    auto ports = enumerate_synscan_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("SynScan mount"));
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) +
                              " out of range (found " + std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("SynScan", "Auto-detected mount on " + port.port_path +
                    " (HC fw " + port.firmware_version + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;

    return create_synscan_telescope_with_site(
        device_number, conn, version, site_latitude_deg, site_longitude_deg,
        site_elevation_m, sync_time_on_connect);
}

} // namespace alpacacore::vendor::synscan

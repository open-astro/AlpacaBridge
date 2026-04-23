// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <mutex>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <atomic>
#include <ctime>
#include <optional>
#include <sstream>
#include <numbers>
#include <algorithm>
#include <cstdio>

namespace alpacacore::vendor::celestron {

namespace {

constexpr double kHoursToDegrees = 15.0;
constexpr auto kPositionCacheTtl = std::chrono::seconds(2);
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
constexpr double kDefaultGuideRateDegPerSec = 7.5 / 3600.0;
constexpr double kSiderealDegPerSec = 15.0411 / 3600.0;
constexpr auto kPulseGuideCompletionDelay = std::chrono::milliseconds(1000);
constexpr auto kPulseGuideHoldGrace = std::chrono::milliseconds(200);
constexpr auto kPulseGuideCorrectionGrace = std::chrono::milliseconds(5000);

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
    local_tm = *std::localtime(&base_time);
    utc_tm = *std::gmtime(&base_time);
    std::time_t local_time = std::mktime(&local_tm);
    std::time_t utc_as_local = std::mktime(&utc_tm);
    double offset_seconds = std::difftime(local_time, utc_as_local);
    info.offset_minutes = static_cast<int>(std::round(offset_seconds / 60.0));
    info.dst = local_tm.tm_isdst > 0;
    return info;
}

} // namespace

class CelestronTelescopeDriver : public TelescopeDriver {
public:
    CelestronTelescopeDriver(int device_number,
                             const ConnectionInfo& connection_info,
                             std::optional<double> site_latitude_deg,
                             std::optional<double> site_longitude_deg,
                             std::optional<double> site_elevation_m,
                             std::optional<bool> sync_time_on_connect)
        : device_number_(device_number)
        , connection_info_(connection_info)
        , connected_(false)
        , target_ra_hours_(0.0)
        , target_dec_degrees_(0.0)
        , aperture_diameter_m_(0.0)
        , aperture_area_m2_(0.0)
        , focal_length_m_(0.0)
        , site_latitude_cached_(0.0)
        , site_longitude_cached_(0.0)
        , site_info_valid_(false)
        , site_elevation_m_(site_elevation_m.value_or(0.0))
        , timezone_offset_minutes_(0)
        , timezone_offset_valid_(false)
        , dst_observed_(false)
        , last_utc_set_{}
        , last_utc_set_monotonic_(std::chrono::steady_clock::now())
        , last_utc_valid_(false)
        , tracking_mode_cached_(0)
        , tracking_mode_valid_(false)
        , parked_(false)
        , at_home_(false)
        , mount_model_id_(-1)
        , use_precise_commands_(true)
        , pending_site_latitude_(site_latitude_deg)
        , pending_site_longitude_(site_longitude_deg)
        , pending_site_elevation_(site_elevation_m)
        , sync_time_on_connect_(sync_time_on_connect.value_or(false))
    {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~CelestronTelescopeDriver() override {
        stop_connection_thread();
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
        if (!mount_model_name_.empty()) {
            return "Celestron " + mount_model_name_;
        }
        return "Celestron NexStar Telescope";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string get_unique_id() const override {
        return "Celestron_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "Celestron NexStar Mount Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore Celestron NexStar Driver v0.1";
    }

    std::string get_driver_version() const override {
        return "1.0.0";
    }

    int get_interface_version() const override {
        return 3;
    }

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

    bool get_connecting() const override {
        return connecting_.load();
    }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (connected == connected_) {
            return;
        }

        auto& protocol = CelestronProtocolWrapper::instance();
        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to Celestron mount");
            }
            connected_ = true;
            mount_firmware_version_ = "";
            mount_model_id_ = -1;
            mount_model_name_ = "";
            site_info_valid_ = false;
            timezone_offset_valid_ = false;
            tracking_mode_valid_ = false;
            target_set_ = false;
            parked_ = false;
            at_home_ = false;
            homing_ = false;
            pulse_guide_active_ = false;
            pulse_guide_end_time_ = std::chrono::steady_clock::time_point::min();
            pulse_guide_axis_ = -1;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            last_utc_valid_ = false;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();

            try {
                mount_firmware_version_ = protocol.get_handset_firmware_version();
                // Determine if precise commands are available (v1.6+ for RA/Dec, v2.2+ for Alt/Az)
                // TODO: Parse firmware version to detect v1.6+ support; assume precise for now.
            } catch (...) {
            }
            // HC fw "0.0" means the hand controller isn't functional (e.g. WiFi-only connection).
            // In that case, HC-level commands ('E','e','R','r','S','s','L') don't work —
            // use pass-through MC commands instead (MC_GET_POSITION, MC_GOTO_FAST, etc.).
            hc_available_ = !mount_firmware_version_.empty() && mount_firmware_version_ != "0.0";
            ALPACA_LOG_WARN("Celestron", "HC firmware: " + mount_firmware_version_ +
                           " — HC " + (hc_available_ ? "available" : "unavailable, using pass-through commands"));
            try {
                mount_model_id_ = protocol.get_model_id();
                mount_model_name_ = protocol.get_model_name();
            } catch (...) {
            }

            // Probe the AUX bus to discover connected devices and capabilities.
            has_autoguider_port_ = false;
            has_ra_switch_ = false;
            has_dec_switch_ = false;
            has_gps_ = false;
            has_focuser_ = false;
            detected_devices_.clear();
            try {
                detected_devices_ = protocol.probe_bus();
                for (const auto& dev : detected_devices_) {
                    ALPACA_LOG_INFO("Celestron", "Detected: " + dev.name +
                                   " (0x" + ([](int a) {
                                       char buf[8];
                                       std::snprintf(buf, sizeof(buf), "%02X", a);
                                       return std::string(buf);
                                   })(dev.address) + ") fw " + dev.firmware_version);
                    switch (dev.address) {
                        case CelestronProtocolWrapper::DEV_DEC_AG:
                            has_autoguider_port_ = true;
                            break;
                        case CelestronProtocolWrapper::DEV_RA_SW:
                            has_ra_switch_ = true;
                            break;
                        case CelestronProtocolWrapper::DEV_DEC_SW:
                            has_dec_switch_ = true;
                            break;
                        case CelestronProtocolWrapper::DEV_GPS:
                            has_gps_ = true;
                            break;
                        case CelestronProtocolWrapper::DEV_FOCUSER:
                            has_focuser_ = true;
                            break;
                        default:
                            break;
                    }
                }
            } catch (...) {
                ALPACA_LOG_WARN("Celestron", "Bus probe failed, using default capabilities");
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
                if (hc_available_) {
                    auto raw = protocol.get_ra_dec_raw(use_precise_commands_);
                    int bits = use_precise_commands_ ? 24 : 16;
                    cached_ra_hours_ = decode_ra_hours(raw.first, bits);
                    cached_dec_degrees_ = decode_angle(raw.second, bits);
                } else {
                    uint32_t ra_raw = protocol.get_mc_position(0);
                    uint32_t dec_raw = protocol.get_mc_position(1);
                    cached_ra_hours_ = decode_ra_hours(ra_raw, 24);
                    cached_dec_degrees_ = decode_angle(dec_raw, 24);
                }
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
            target_set_ = false;
            parked_ = false;
            at_home_ = false;
            homing_ = false;
            pulse_guide_active_ = false;
            pulse_guide_end_time_ = std::chrono::steady_clock::time_point::min();
            pulse_guide_axis_ = -1;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            sync_completed_this_session_ = false;
            skip_next_ra_learn_ = false;
            flip_in_progress_ = false;
        }
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state.push_back({"Connected", connected_});
            state.push_back({"Slewing", connected_ ? get_slewing_locked() : false});
            state.push_back({"Tracking", connected_ ? get_tracking_locked() : false});
            state.push_back({"RightAscension", cached_ra_hours_});
            state.push_back({"Declination", cached_dec_degrees_});
            state.push_back({"Altitude", cached_alt_degrees_});
            state.push_back({"Azimuth", cached_az_degrees_});
        }
        return state;
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
        auto& protocol = CelestronProtocolWrapper::instance();
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
        auto& protocol = CelestronProtocolWrapper::instance();
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
        auto& protocol = CelestronProtocolWrapper::instance();
        if (raw) {
            return protocol.send_command(std::string(command));
        }
        return protocol.send_command(std::string(command));
    }

    AlignmentMode get_alignment_mode() const override {
        // Most Celestron NexStar mounts are Alt/Az, but CGE and Advanced GT are GEM.
        // TODO: Detect alignment mode from mount model for GEM mounts (CGE, Advanced GT, CGEM, AVX).
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
        if (homing_ && connected_) {
            check_homing_complete_locked();
        }
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
        return true;
    }

    bool get_can_park() const override {
        return true;
    }

    bool get_can_pulse_guide() const override {
        return has_autoguider_port_;
    }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (pulse_guide_active_ && std::chrono::steady_clock::now() >= pulse_guide_end_time_) {
            pulse_guide_active_ = false;
        }
        return pulse_guide_active_;
    }

    bool get_can_set_declination_rate() const override {
        return false;
    }

    bool get_can_set_guide_rates() const override {
        return has_autoguider_port_;
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
        const auto now = std::chrono::steady_clock::now();
        double dec_value;
        if (pg_hold_dec_valid_ && now < pg_hold_dec_until_) {
            dec_value = pg_hold_dec_degrees_;
        } else {
            refresh_equatorial_cache_locked();
            dec_value = cached_dec_degrees_;
        }
        if (pg_dec_correction_valid_ && now < pg_dec_correction_until_) {
            dec_value = pg_dec_baseline_degrees_ + pg_dec_expected_delta_degrees_;
            pg_dec_correction_valid_ = false;
        }
        return std::clamp(dec_value, -90.0, 90.0);
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
        auto& protocol = CelestronProtocolWrapper::instance();
        if (tracking) {
            // Default to EQ North tracking; user can override via tracking rate/mode if needed.
            // TODO: Auto-detect Alt/Az vs EQ mode from mount model.
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
        if (!has_autoguider_port_) {
            throw AlpacaException("Guide rates not supported", AlpacaError::PropertyNotImplemented);
        }
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        if (!has_autoguider_port_) {
            throw AlpacaException("Guide rates not supported", AlpacaError::PropertyNotImplemented);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double ra_percent = (rate.ra / kSiderealDegPerSec) * 100.0;
        double dec_percent = (rate.dec / kSiderealDegPerSec) * 100.0;
        auto& protocol = CelestronProtocolWrapper::instance();
        protocol.set_autoguide_rate(0, ra_percent);
        protocol.set_autoguide_rate(1, dec_percent);
        guide_rate_ = rate;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (target_set_ && std::chrono::steady_clock::now() < position_override_until_ && !get_slewing_locked()) {
            return target_ra_hours_;
        }
        const auto now = std::chrono::steady_clock::now();
        double ra_value;
        if (pg_hold_ra_valid_ && now < pg_hold_ra_until_) {
            ra_value = pg_hold_ra_hours_;
        } else {
            refresh_equatorial_cache_locked();
            ra_value = cached_ra_hours_;
        }
        if (pg_ra_correction_valid_ && now < pg_ra_correction_until_) {
            ra_value = std::fmod(pg_ra_baseline_hours_ + pg_ra_expected_delta_hours_, 24.0);
            if (ra_value < 0.0) ra_value += 24.0;
            pg_ra_correction_valid_ = false;
        }
        return ra_value;
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
        if (!hc_available_) {
            return -1;
        }
        auto& protocol = CelestronProtocolWrapper::instance();
        char side = protocol.get_pier_side();
        if (side == 'W') return 0;  // pierEast (ASCOM enum: 0=pierEast)
        if (side == 'E') return 1;  // pierWest (ASCOM enum: 1=pierWest)
        return -1;
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
            return -1;
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
        if (!site_info_valid_) {
            throw AlpacaException("SiteLatitude not available — location not set on mount",
                                  AlpacaError::ValueNotSet);
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
        CelestronProtocolWrapper::instance().set_location(info);
        site_latitude_cached_ = latitude;
        site_longitude_cached_ = info.longitude_degrees;
        site_info_valid_ = true;
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
        }
        if (!site_info_valid_) {
            throw AlpacaException("SiteLongitude not available — location not set on mount",
                                  AlpacaError::ValueNotSet);
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
        CelestronProtocolWrapper::instance().set_location(info);
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
                TimeInfo info = CelestronProtocolWrapper::instance().get_time();
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
        check_connected();
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);
        LocalTimeInfo tz_info = compute_local_timezone_info(utc_time_t);

        std::tm local_tm {};
        local_tm = *std::localtime(&utc_time_t);

        TimeInfo info;
        info.hour = local_tm.tm_hour;
        info.minute = local_tm.tm_min;
        info.second = local_tm.tm_sec;
        info.month = local_tm.tm_mon + 1;
        info.day = local_tm.tm_mday;
        info.year = (local_tm.tm_year + 1900) % 100;
        info.timezone_offset_minutes = tz_info.offset_minutes;
        info.dst_enabled = tz_info.dst;

        CelestronProtocolWrapper::instance().set_time(info);
        timezone_offset_minutes_ = tz_info.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = tz_info.dst;
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
    }

    void find_home() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("FindHome");
        auto& protocol = CelestronProtocolWrapper::instance();

        // MC_LEVEL_START (0x0B) — moves each axis to its hardware home switch.
        // Asynchronous: returns immediately, client polls Slewing/AtHome.
        protocol.level_start(0);
        protocol.level_start(1);

        homing_ = true;
        at_home_ = false;
        slewing_cached_ = true;
    }

    void park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!park_position_set_) {
            refresh_equatorial_cache_locked();
            park_ra_hours_ = cached_ra_hours_;
            park_dec_degrees_ = cached_dec_degrees_;
            park_position_set_ = true;
        }
        do_slew_to_coordinates_locked(park_ra_hours_, park_dec_degrees_);
        wait_for_slew_complete_locked();
        CelestronProtocolWrapper::instance().set_tracking_mode(0);
        tracking_mode_cached_ = 0;
        tracking_mode_valid_ = true;
        parked_ = true;
    }

    void pulse_guide(int direction, int duration) override {
        if (!has_autoguider_port_) {
            throw AlpacaException("PulseGuide not supported — no autoguider port detected",
                                  AlpacaError::MethodNotImplemented);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("PulseGuide");

        auto& protocol = CelestronProtocolWrapper::instance();

        int axis = -1;
        int velocity = 0;

        switch (direction) {
            case 0: // North → DEC axis, positive velocity
                axis = 1;
                velocity = static_cast<int>(std::round((guide_rate_.dec / kSiderealDegPerSec) * 100.0));
                break;
            case 1: // South → DEC axis, negative velocity
                axis = 1;
                velocity = -static_cast<int>(std::round((guide_rate_.dec / kSiderealDegPerSec) * 100.0));
                break;
            case 2: // East → RA axis, negative velocity (slow tracking = RA increases on sky)
                axis = 0;
                velocity = -static_cast<int>(std::round((guide_rate_.ra / kSiderealDegPerSec) * 100.0));
                break;
            case 3: // West → RA axis, positive velocity (speed tracking = RA decreases on sky)
                axis = 0;
                velocity = static_cast<int>(std::round((guide_rate_.ra / kSiderealDegPerSec) * 100.0));
                break;
            default:
                throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }

        if (velocity == 0) {
            velocity = (direction == 1 || direction == 2) ? -1 : 1;
        }

        // MC_AUX_GUIDE uses raw motor coordinates: positive rate = South
        // when pierWest (normal), North when pierEast (through-the-pole).
        // Flip DEC sign on the normal (West) side so North means North.
        if (axis == 1 && hc_available_) {
            char pier = protocol.get_pier_side();
            if (pier == 'W') {
                velocity = -velocity;
            }
        }

        int total_cs = std::clamp(duration / 10, 0, 2550);
        int first_chunk_cs = std::min(total_cs, 255);

        const bool is_dec_axis = (direction == 0 || direction == 1);
        const double duration_sec = duration / 1000.0;
        const auto now = std::chrono::steady_clock::now();
        const auto hold_end = now + std::chrono::milliseconds(duration) +
                              kPulseGuideCompletionDelay + kPulseGuideHoldGrace;
        const auto correction_end = now + std::chrono::milliseconds(duration) +
                                    kPulseGuideCompletionDelay + kPulseGuideCorrectionGrace;

        refresh_equatorial_cache_locked();

        if (is_dec_axis) {
            pg_hold_ra_hours_ = cached_ra_hours_;
            pg_hold_ra_valid_ = true;
            pg_hold_ra_until_ = hold_end;
            pg_hold_dec_valid_ = false;

            double expected_delta = guide_rate_.dec * duration_sec;
            if (direction == 1) expected_delta = -expected_delta;
            pg_dec_baseline_degrees_ = cached_dec_degrees_;
            pg_dec_expected_delta_degrees_ = expected_delta;
            pg_dec_correction_valid_ = true;
            pg_dec_correction_until_ = correction_end;
            pg_ra_correction_valid_ = false;
        } else {
            pg_hold_dec_degrees_ = cached_dec_degrees_;
            pg_hold_dec_valid_ = true;
            pg_hold_dec_until_ = hold_end;
            pg_hold_ra_valid_ = false;

            double expected_delta = (guide_rate_.ra * duration_sec) / 15.0;
            if (direction == 3) expected_delta = -expected_delta;
            pg_ra_baseline_hours_ = cached_ra_hours_;
            pg_ra_expected_delta_hours_ = expected_delta;
            pg_ra_correction_valid_ = true;
            pg_ra_correction_until_ = correction_end;
            pg_dec_correction_valid_ = false;
        }

        protocol.pulse_guide_axis(axis, velocity, first_chunk_cs);

        pulse_guide_active_ = true;
        pulse_guide_axis_ = axis;
        pulse_guide_end_time_ = now + std::chrono::milliseconds(duration) +
                                kPulseGuideCompletionDelay;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;

        // Chain remaining chunks in a background thread (MC_AUX_GUIDE max = 255cs = 2550ms).
        int remaining_cs = total_cs - first_chunk_cs;
        if (remaining_cs > 0) {
            std::thread([axis, velocity, remaining_cs, first_chunk_cs]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(first_chunk_cs * 10));
                int left = remaining_cs;
                auto& proto = CelestronProtocolWrapper::instance();
                while (left > 0) {
                    int chunk = std::min(left, 255);
                    try {
                        proto.pulse_guide_axis(axis, velocity, chunk);
                    } catch (...) {
                        break;
                    }
                    left -= chunk;
                    if (left > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(chunk * 10));
                    }
                }
            }).detach();
        }
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinates");
        do_slew_to_coordinates_locked(ra, dec);
        wait_for_slew_complete_locked();
        restore_tracking_after_slew_locked();
        learn_ra_offset_locked(ra);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        uint32_t ra_raw = 0;
        uint32_t dec_raw = 0;
        bool precise = false;
        bool use_passthrough = false;
        bool do_flip = false;
        uint32_t flip_ra = 0, flip_dec = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            check_slew_safety_locked("SlewToCoordinatesAsync");
            slew_aborted_ = false;

            use_passthrough = !hc_available_;
            int bits = use_passthrough ? 24 : (use_precise_commands_ ? 24 : 16);
            double ra_biased = std::fmod(ra + ra_slew_offset_hours_, 24.0);
            if (ra_biased < 0.0) ra_biased += 24.0;
            ra_raw = encode_ra_raw(ra_biased, bits);
            dec_raw = encode_angle(dec, bits);
            precise = use_precise_commands_;

            do_flip = compute_flip_locked(ra, dec, ra_biased, flip_ra, flip_dec);

            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "slew_to_coordinates_async request ra_h=%.6f dec_deg=%.6f "
                              "ra_biased=%.6f offset=%.1f\" "
                              "ra_raw=0x%06X dec_raw=0x%06X bits=%d path=%s flip=%d",
                              ra, dec, ra_biased, ra_slew_offset_hours_ * 54000.0,
                              ra_raw, dec_raw, bits,
                              use_passthrough ? "MC_PASSTHROUGH" : "HC",
                              do_flip ? 1 : 0);
                ALPACA_LOG_WARN("Celestron", std::string(buf));
            }

            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            flip_in_progress_ = do_flip;
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_set_ = true;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
        }

        double slew_target_ra = ra;
        std::thread([this, ra_raw, dec_raw, precise, use_passthrough,
                     do_flip, flip_ra, flip_dec, slew_target_ra]() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_) {
                    return;
                }
                try {
                    auto& protocol = CelestronProtocolWrapper::instance();
                    if (do_flip) {
                        protocol.mc_goto_fast(0, flip_ra);
                        protocol.mc_goto_fast(1, flip_dec);
                    } else if (use_passthrough) {
                        protocol.mc_goto_fast(0, ra_raw);
                        protocol.mc_goto_fast(1, dec_raw);
                    } else {
                        protocol.goto_ra_dec_raw(ra_raw, dec_raw, precise);
                    }
                } catch (const std::exception& ex) {
                    slewing_cached_ = false;
                    slew_force_until_ = std::chrono::steady_clock::time_point::min();
                    position_override_until_ = std::chrono::steady_clock::time_point::min();
                    flip_in_progress_ = false;
                    ALPACA_LOG_WARN("Celestron", std::string("Async slew dispatch failed: ") + ex.what());
                    return;
                }
            }
            // Poll for slew completion WITHOUT holding the mutex so that
            // HTTP endpoints (Slewing, RightAscension, etc.) remain responsive.
            auto timeout = std::chrono::seconds(120);
            auto start = std::chrono::steady_clock::now();
            auto start_grace = std::chrono::seconds(2);
            bool saw_slewing = false;
            bool use_axis_poll = do_flip || use_passthrough;
            auto& protocol = CelestronProtocolWrapper::instance();
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                bool still_slewing = false;
                try {
                    if (hc_available_ && !use_axis_poll) {
                        still_slewing = protocol.is_goto_in_progress();
                    } else {
                        still_slewing = !protocol.is_slew_done(0) || !protocol.is_slew_done(1);
                    }
                } catch (...) {
                    still_slewing = false;
                }
                if (still_slewing) {
                    saw_slewing = true;
                }
                if (!still_slewing) {
                    if (!saw_slewing && (std::chrono::steady_clock::now() - start) < start_grace) {
                        continue;
                    }
                    break;
                }
                if (std::chrono::steady_clock::now() - start > timeout) {
                    ALPACA_LOG_WARN("Celestron", "Async slew timed out after 120s");
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                equatorial_cache_valid_ = false;
                altaz_cache_valid_ = false;
                flip_in_progress_ = false;
                if (slew_settle_time_seconds_ > 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
                }
                restore_tracking_after_slew_locked();
                learn_ra_offset_locked(slew_target_ra);
            }
        }).detach();
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
        auto& protocol = CelestronProtocolWrapper::instance();
        if (hc_available_) {
            int bits = use_precise_commands_ ? 24 : 16;
            uint32_t ra_raw = encode_ra_raw(ra, bits);
            uint32_t dec_raw = encode_angle(dec, bits);
            protocol.sync_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_);
        } else {
            uint32_t ra_raw = encode_ra_raw(ra, 24);
            uint32_t dec_raw = encode_angle(dec, 24);
            protocol.mc_set_position(0, ra_raw);
            protocol.mc_set_position(1, dec_raw);
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        sync_completed_this_session_ = true;
        skip_next_ra_learn_ = true;
        ALPACA_LOG_WARN("Celestron",
                        "SyncToCoordinates completed — slew gate now OPEN for this session, "
                        "RA offset learning suspended for next slew");
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
        parked_ = false;
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

        CelestronProtocolWrapper::instance().move_axis_variable_rate(axis, moving ? rate : 0.0);
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
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
        auto& protocol = CelestronProtocolWrapper::instance();
        protocol.cancel_goto();
        protocol.move_axis_fixed_rate(0, 0);
        protocol.move_axis_fixed_rate(1, 0);
        homing_ = false;
        slewing_cached_ = false;
        slew_aborted_ = true;
        flip_in_progress_ = false;
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
    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to Celestron mount");
        }
    }

    void check_not_parked_locked(const char* operation) const {
        if (parked_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidOperation);
        }
    }

    void start_connection_task(bool connect) {
        if (connecting_.exchange(true)) {
            return;
        }
        stop_connection_thread();
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
            } catch (const std::exception& ex) {
                ALPACA_LOG_ERROR("Celestron", std::string("Connection task failed: ") + ex.what());
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    void refresh_equatorial_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (equatorial_cache_valid_ && (now - last_equatorial_update_) < kPositionCacheTtl) {
            return;
        }
        auto& protocol = CelestronProtocolWrapper::instance();
        try {
            if (hc_available_) {
                int bits = use_precise_commands_ ? 24 : 16;
                auto raw = protocol.get_ra_dec_raw(use_precise_commands_);
                cached_ra_hours_ = decode_ra_hours(raw.first, bits);
                cached_dec_degrees_ = decode_angle(raw.second, bits);
            } else {
                uint32_t ra_raw = protocol.get_mc_position(0);
                uint32_t dec_raw = protocol.get_mc_position(1);
                cached_ra_hours_ = decode_ra_hours(ra_raw, 24);
                cached_dec_degrees_ = decode_angle(dec_raw, 24);
            }
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
        auto& protocol = CelestronProtocolWrapper::instance();
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
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (std::chrono::steady_clock::now() < slew_force_until_) {
            return true;
        }
        if (homing_) {
            check_homing_complete_locked();
            return homing_; // still homing = still slewing
        }
        bool was_slewing = slewing_cached_;
        try {
            auto& protocol = CelestronProtocolWrapper::instance();
            if (hc_available_ && !flip_in_progress_) {
                slewing_cached_ = protocol.is_goto_in_progress();
            } else {
                bool ra_done = protocol.is_slew_done(0);
                bool dec_done = protocol.is_slew_done(1);
                slewing_cached_ = !ra_done || !dec_done;
            }
        } catch (...) {
        }
        if (was_slewing && !slewing_cached_) {
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            flip_in_progress_ = false;
        }
        return slewing_cached_;
    }

    void check_homing_complete_locked() const {
        try {
            auto& protocol = CelestronProtocolWrapper::instance();
            bool ra_done = protocol.is_level_done(0);
            bool dec_done = protocol.is_level_done(1);
            if (ra_done && dec_done) {
                homing_ = false;
                at_home_ = true;
                slewing_cached_ = false;
                equatorial_cache_valid_ = false;
                altaz_cache_valid_ = false;
            }
        } catch (...) {
        }
    }

    bool get_tracking_locked() const {
        if (!tracking_mode_valid_) {
            tracking_mode_cached_ = CelestronProtocolWrapper::instance().get_tracking_mode();
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
            LocationInfo info = CelestronProtocolWrapper::instance().get_location();
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
        return CelestronProtocolWrapper::instance().get_location();
    }

    // Centralized pier-safety gate used by both sync and async slew paths.
    // Must be called with mutex_ held. Throws if the slew must be refused.
    void check_slew_safety_locked(const char* context) const {
        auto& protocol = CelestronProtocolWrapper::instance();
        bool aligned = false;
        std::string aligned_detail;
        try {
            aligned = protocol.is_aligned();
            aligned_detail = aligned ? "J=truthy" : "J=false";
        } catch (const std::exception& e) {
            aligned_detail = std::string("J-query-failed: ") + e.what();
        }
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "slew-safety check (%s): %s, sync_completed_this_session=%d",
                          context, aligned_detail.c_str(),
                          sync_completed_this_session_ ? 1 : 0);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }

        if (!sync_completed_this_session_ && !aligned) {
            const char* msg =
                "REFUSED slew: mount is not aligned and no Sync has been performed "
                "in this driver session. Complete HC alignment (Last Alignment) or "
                "perform a SyncToCoordinates on a known target before slewing.";
            ALPACA_LOG_WARN("Celestron", std::string(msg));
            throw AlpacaException(msg, AlpacaError::InvalidOperation);
        }
    }

    // After a slew completes, re-apply sidereal tracking on axis 0. Observed
    // on CGX-L fw 7.18: axis 0 stays at rate=0 after goto completion and the
    // HC doesn't resume tracking automatically, so subsequent pulse-guide
    // tests run against a stopped RA motor and report spurious east drift.
    void restore_tracking_after_slew_locked() {
        if (!get_tracking_locked()) {
            return;
        }
        auto& protocol = CelestronProtocolWrapper::instance();
        try {
            int mode = tracking_mode_cached_;
            if (mode <= 0) {
                mode = 2; // EQ-North default
            }
            protocol.set_tracking_mode(mode);
            tracking_mode_cached_ = mode;
            tracking_mode_valid_ = true;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Celestron",
                            std::string("restore_tracking_after_slew: ") + e.what());
        }
    }

    void learn_ra_offset_locked(double target_ra) {
        if (slew_aborted_) {
            ALPACA_LOG_WARN("Celestron", "RA offset learning skipped (slew was aborted)");
            return;
        }
        if (skip_next_ra_learn_) {
            skip_next_ra_learn_ = false;
            ALPACA_LOG_WARN("Celestron", "RA offset learning skipped (first slew after SyncToCoordinates)");
            return;
        }
        equatorial_cache_valid_ = false;
        refresh_equatorial_cache_locked();
        double error = shortest_ra_delta_hours(target_ra, cached_ra_hours_);
        constexpr double kMaxResidualHours = 30.0 / 54000.0; // 30 arcsec
        if (std::abs(error) > kMaxResidualHours) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "RA offset learning: outlier rejected residual=%.1f\" (limit ±30\")",
                          error * 54000.0);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
            return;
        }
        constexpr double kAlpha = 0.5;
        constexpr double kMaxOffset = 0.002; // ±108 arcsec max
        ra_slew_offset_hours_ = std::clamp(
            ra_slew_offset_hours_ + kAlpha * error, -kMaxOffset, kMaxOffset);
        ra_offset_samples_++;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "RA offset learning: residual=%.1f\" offset=%.1f\" samples=%d",
                      error * 54000.0, ra_slew_offset_hours_ * 54000.0,
                      ra_offset_samples_);
        ALPACA_LOG_WARN("Celestron", std::string(buf));
    }

    // Determines whether the mount needs a forced meridian flip to reach
    // the target.  Returns true if mc_goto_fast should be used with flipped
    // encoder coordinates; fills ra_raw/dec_raw accordingly (24-bit).
    bool compute_flip_locked(double ra, double dec, double ra_biased,
                             uint32_t& ra_raw_out, uint32_t& dec_raw_out) const {
        if (!hc_available_) return false;
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
            if (!site_info_valid_) return false;
        }
        auto& protocol = CelestronProtocolWrapper::instance();
        char hw_side = protocol.get_pier_side();
        int curr = (hw_side == 'W') ? 0 : (hw_side == 'E') ? 1 : -1;
        if (curr < 0) return false;

        double lst = compute_local_sidereal_time_hours(
            std::chrono::system_clock::now(), site_longitude_cached_);
        double ha = shortest_ra_delta_hours(lst, ra);
        int dest = ha >= 0.0 ? 0 : 1;   // 0=pierEast, 1=pierWest
        if (dest == curr) return false;

        if (dest == 0) {
            // Flip to pierEast: RA encoder = RA+12h, DEC encoder = 180°-DEC
            double ra_flip = std::fmod(ra_biased + 12.0, 24.0);
            ra_raw_out = encode_ra_raw(ra_flip, 24);
            dec_raw_out = encode_angle(180.0 - dec, 24);
        } else {
            // Flip to pierWest: normal encoder coords
            ra_raw_out = encode_ra_raw(ra_biased, 24);
            dec_raw_out = encode_angle(dec, 24);
        }
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "forced meridian flip: curr=%c dest=%d ha=%.2fh "
                          "ra_raw=0x%06X dec_raw=0x%06X",
                          hw_side, dest, ha, ra_raw_out, dec_raw_out);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        return true;
    }

    void do_slew_to_coordinates_locked(double ra, double dec) {
        validate_ra_dec(ra, dec, "SlewToCoordinates");
        check_slew_safety_locked("SlewToCoordinates");
        slew_aborted_ = false;
        auto& protocol = CelestronProtocolWrapper::instance();

        double ra_biased = std::fmod(ra + ra_slew_offset_hours_, 24.0);
        if (ra_biased < 0.0) ra_biased += 24.0;

        const int bits = hc_available_ ? (use_precise_commands_ ? 24 : 16) : 24;
        const uint32_t ra_raw_target = encode_ra_raw(ra_biased, bits);
        const uint32_t dec_raw_target = encode_angle(dec, bits);

        {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "do_slew_to_coordinates request ra_h=%.6f dec_deg=%.6f "
                          "ra_biased=%.6f offset=%.1f\" "
                          "ra_raw=0x%06X dec_raw=0x%06X bits=%d path=%s hc=%d precise=%d",
                          ra, dec, ra_biased, ra_slew_offset_hours_ * 54000.0,
                          ra_raw_target, dec_raw_target, bits,
                          hc_available_ ? "HC" : "MC_PASSTHROUGH",
                          hc_available_ ? 1 : 0,
                          use_precise_commands_ ? 1 : 0);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }

        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        slewing_cached_ = true;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        position_override_until_ = std::chrono::steady_clock::time_point::min();

        uint32_t flip_ra = 0, flip_dec = 0;
        bool flip = compute_flip_locked(ra, dec, ra_biased, flip_ra, flip_dec);

        if (flip) {
            flip_in_progress_ = true;
            protocol.mc_goto_fast(0, flip_ra);
            protocol.mc_goto_fast(1, flip_dec);
        } else if (hc_available_) {
            flip_in_progress_ = false;
            protocol.goto_ra_dec_raw(ra_raw_target, dec_raw_target, use_precise_commands_);
        } else {
            flip_in_progress_ = false;
            protocol.mc_goto_fast(0, ra_raw_target);
            protocol.mc_goto_fast(1, dec_raw_target);
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parked_ = false;
        at_home_ = false;
        homing_ = false;
    }

    void wait_for_slew_complete_locked() const {
        const auto timeout = std::chrono::seconds(120);
        auto start = std::chrono::steady_clock::now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        while (true) {
            bool slewing = get_slewing_locked();
            if (slewing) {
                saw_slewing = true;
            }
            if (!slewing) {
                if (!saw_slewing && (std::chrono::steady_clock::now() - start) < start_grace) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                break;
            }
            if (std::chrono::steady_clock::now() - start > timeout) {
                throw AlpacaException("Slew timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        if (slew_settle_time_seconds_ > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
        }
    }

    void sync_mount_time_locked() {
        auto now_utc = std::chrono::system_clock::now();
        set_utc_date(now_utc);
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
    mutable std::mutex mutex_;
    std::atomic<bool> connecting_{false};
    std::thread connection_thread_;
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
    mutable bool homing_ = false;
    mutable bool slewing_cached_ = false;
    mutable std::chrono::steady_clock::time_point slew_force_until_;
    mutable std::chrono::steady_clock::time_point position_override_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    std::string mount_firmware_version_;
    bool hc_available_ = true;
    int mount_model_id_;
    std::string mount_model_name_;
    bool use_precise_commands_;
    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
    GuideRate guide_rate_{};
    mutable bool pulse_guide_active_ = false;
    mutable std::chrono::steady_clock::time_point pulse_guide_end_time_;
    int pulse_guide_axis_ = -1;
    bool has_autoguider_port_ = false;

    mutable double pg_hold_ra_hours_ = 0.0;
    mutable bool pg_hold_ra_valid_ = false;
    mutable std::chrono::steady_clock::time_point pg_hold_ra_until_;
    mutable double pg_hold_dec_degrees_ = 0.0;
    mutable bool pg_hold_dec_valid_ = false;
    mutable std::chrono::steady_clock::time_point pg_hold_dec_until_;

    mutable double pg_ra_baseline_hours_ = 0.0;
    mutable double pg_ra_expected_delta_hours_ = 0.0;
    mutable bool pg_ra_correction_valid_ = false;
    mutable std::chrono::steady_clock::time_point pg_ra_correction_until_;
    mutable double pg_dec_baseline_degrees_ = 0.0;
    mutable double pg_dec_expected_delta_degrees_ = 0.0;
    mutable bool pg_dec_correction_valid_ = false;
    mutable std::chrono::steady_clock::time_point pg_dec_correction_until_;
    mutable bool sync_completed_this_session_ = false;
    bool has_ra_switch_ = false;
    bool has_dec_switch_ = false;
    bool has_gps_ = false;
    bool has_focuser_ = false;
    std::vector<CelestronProtocolWrapper::BusDevice> detected_devices_;
    bool park_position_set_ = false;
    double park_ra_hours_ = 0.0;
    double park_dec_degrees_ = 0.0;
    double ra_slew_offset_hours_ = 0.0005; // ~27" initial seed for CGX-L fw 7.18 goto tracking deficit
    int ra_offset_samples_ = 0;
    bool slew_aborted_ = false;
    bool skip_next_ra_learn_ = false;
    mutable bool flip_in_progress_ = false;
};

std::unique_ptr<TelescopeDriver> create_celestron_telescope(
    int device_number,
    const ConnectionInfo& connection_info) {
    return create_celestron_telescope_with_site(device_number, connection_info,
                                                std::nullopt, std::nullopt,
                                                std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_celestron_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {
    return std::make_unique<CelestronTelescopeDriver>(device_number, connection_info,
                                                      site_latitude_deg, site_longitude_deg,
                                                      site_elevation_m, sync_time_on_connect);
}

std::unique_ptr<TelescopeDriver> create_celestron_telescope_auto(
    int device_number,
    int mount_index,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {

    auto ports = enumerate_celestron_ports();
    if (ports.empty()) {
        throw AlpacaException("No Celestron NexStar mount found on any serial port");
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) +
                              " out of range (found " + std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("Celestron", "Auto-detected mount on " + port.port_path +
                    " (HC fw " + port.firmware_version + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;

    return create_celestron_telescope_with_site(
        device_number, conn, site_latitude_deg, site_longitude_deg,
        site_elevation_m, sync_time_on_connect);
}

} // namespace alpacacore::vendor::celestron

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

#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace alpacacore::vendor::zwo {

namespace {

constexpr double kSiderealRateDegPerSec = 360.0 / 86164.0905;
constexpr double kMaxMoveAxisRateDegPerSec = kSiderealRateDegPerSec * 1440.0;
constexpr double kHoursToDegrees = 15.0;
constexpr double kMinSiteElevationMeters = -300.0;
constexpr double kMaxSiteElevationMeters = 10000.0;
constexpr auto kDefaultSlewTimeout = std::chrono::minutes(15);
constexpr auto kParkMotionSettle = std::chrono::seconds(2);

void validate_ra(double ra, const char* field_name) {
    if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
        throw AlpacaException(std::string(field_name) + " must be in [0,24) hours", AlpacaError::InvalidValue);
    }
}

void validate_dec(double dec, const char* field_name) {
    if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
        throw AlpacaException(std::string(field_name) + " must be in [-90,+90] degrees", AlpacaError::InvalidValue);
    }
}

void validate_latitude(double latitude) {
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
        throw AlpacaException("SiteLatitude must be in [-90,+90] degrees", AlpacaError::InvalidValue);
    }
}

void validate_longitude(double longitude) {
    if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
        throw AlpacaException("SiteLongitude must be in [-180,+180] degrees", AlpacaError::InvalidValue);
    }
}

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder > 0) != (divisor > 0))) {
        --quotient;
    }
    return quotient;
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t days, int& year, unsigned& month, unsigned& day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year += (month <= 2);
}

std::chrono::system_clock::time_point to_utc_time_point(const TimeInfo& info) {
    const auto days = days_from_civil(info.year, static_cast<unsigned>(info.month), static_cast<unsigned>(info.day));
    std::int64_t local_seconds = days * 86400 +
                                 static_cast<std::int64_t>(info.hour) * 3600 +
                                 static_cast<std::int64_t>(info.minute) * 60 +
                                 static_cast<std::int64_t>(info.second);
    local_seconds -= static_cast<std::int64_t>(info.timezone_offset_minutes) * 60;
    return std::chrono::system_clock::time_point{std::chrono::seconds(local_seconds)};
}

TimeInfo from_utc_time_point(std::chrono::system_clock::time_point utc, int timezone_offset_minutes) {
    const auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(utc.time_since_epoch()).count();
    const std::int64_t local_seconds = seconds_since_epoch + static_cast<std::int64_t>(timezone_offset_minutes) * 60;

    const std::int64_t day_count = floor_div(local_seconds, 86400);
    std::int64_t second_of_day = local_seconds - day_count * 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
    }

    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    civil_from_days(day_count, year, month, day);

    TimeInfo out;
    out.year = year;
    out.month = static_cast<int>(month);
    out.day = static_cast<int>(day);
    out.hour = static_cast<int>(second_of_day / 3600);
    out.minute = static_cast<int>((second_of_day % 3600) / 60);
    out.second = static_cast<int>(second_of_day % 60);
    out.timezone_offset_minutes = timezone_offset_minutes;
    return out;
}

std::string sanitize_identifier(std::string value) {
    for (char& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }
    return value;
}

double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time,
                                         double longitude_degrees) {
    using namespace std::chrono;
    const double unix_epoch_jd = 2440587.5;
    const double days_since_epoch = duration_cast<seconds>(utc_time.time_since_epoch()).count() / 86400.0;
    const double jd = unix_epoch_jd + days_since_epoch;
    const double t = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                  + 0.000387933 * t * t - (t * t * t) / 38710000.0;
    gmst = std::fmod(gmst, 360.0);
    if (gmst < 0.0) {
        gmst += 360.0;
    }

    double lst = gmst + longitude_degrees;
    lst = std::fmod(lst, 360.0);
    if (lst < 0.0) {
        lst += 360.0;
    }
    return lst / kHoursToDegrees;
}

} // namespace

class ZWOTelescopeDriver : public TelescopeDriver {
public:
    ZWOTelescopeDriver(int device_number,
                       const ConnectionInfo& connection_info,
                       std::optional<double> site_latitude_deg,
                       std::optional<double> site_longitude_deg,
                       std::optional<double> site_elevation_m,
                       std::optional<bool> sync_time_on_connect)
        : device_number_(device_number)
        , connection_info_(connection_info)
        , connected_(false)
        , connecting_(false)
        , mount_info_()
        , target_ra_hours_(0.0)
        , target_dec_degrees_(0.0)
        , target_set_(false)
        , aperture_diameter_m_(0.0)
        , aperture_area_m2_(0.0)
        , focal_length_m_(0.0)
        , site_latitude_deg_(site_latitude_deg.value_or(0.0))
        , site_longitude_deg_(site_longitude_deg.value_or(0.0))
        , site_coords_valid_(site_latitude_deg.has_value() && site_longitude_deg.has_value())
        , site_elevation_m_(site_elevation_m.value_or(0.0))
        , does_refraction_(false)
        , slew_settle_time_s_(0)
        , guide_rate_({0.5, 0.5})
        , tracking_rate_cached_(0)
        , tracking_rate_valid_(false)
        , last_utc_set_(std::chrono::system_clock::time_point{})
        , last_utc_set_monotonic_(std::chrono::steady_clock::time_point{})
        , last_utc_valid_(false)
        , timezone_offset_minutes_(0)
        , timezone_valid_(false)
        , manual_axis_slewing_({false, false})
        , slew_force_until_(std::chrono::steady_clock::time_point{})
        , pulse_guiding_end_(std::chrono::steady_clock::time_point{})
        , parked_cached_(false)
        , park_command_active_(false)
        , park_command_started_(std::chrono::steady_clock::time_point{})
        , park_motion_seen_(false)
        , pending_site_latitude_(site_latitude_deg)
        , pending_site_longitude_(site_longitude_deg)
        , pending_site_elevation_(site_elevation_m)
        , sync_time_on_connect_(sync_time_on_connect.value_or(true)) {}

    ~ZWOTelescopeDriver() override {
        stop_connection_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error while disconnecting ZWO mount driver: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!mount_info_.empty()) {
            return "ZWO " + mount_info_;
        }
        return "ZWO Mount";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!mount_info_.empty()) {
            return "ZWO_MOUNT_" + sanitize_identifier(mount_info_) + "_" + std::to_string(device_number_);
        }
        return "ZWO_MOUNT_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO AM Mount Driver (Serial/TCP LX200-compatible protocol)";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO Mount Driver";
    }

    std::string get_driver_version() const override {
        return "0.1.0";
    }

    int get_interface_version() const override {
        return 3;
    }

    bool get_connected() const override {
        return connected_.load();
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
        if (connected == connected_.load()) {
            return;
        }

        auto& protocol = ZWOMountProtocolWrapper::instance();
        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to ZWO mount", AlpacaError::NotConnected);
            }
            connected_.store(true);
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            slew_force_until_ = std::chrono::steady_clock::time_point{};
            pulse_guiding_end_ = std::chrono::steady_clock::time_point{};
            parked_cached_ = false;
            park_command_active_ = false;
            park_command_started_ = std::chrono::steady_clock::time_point{};
            park_motion_seen_ = false;
            tracking_rate_valid_ = false;

            try {
                mount_info_ = protocol.get_mount_info();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Unable to query mount info: " + std::string(e.what()));
                mount_info_.clear();
            }

            try {
                const SiteInfo site = protocol.get_site_info();
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
            } catch (const std::exception&) {
                if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value()) {
                    SiteInfo site;
                    site.latitude_degrees = pending_site_latitude_.value();
                    site.longitude_degrees = pending_site_longitude_.value();
                    protocol.set_site_info(site);
                    site_latitude_deg_ = site.latitude_degrees;
                    site_longitude_deg_ = site.longitude_degrees;
                    site_coords_valid_ = true;
                }
            }

            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }

            if (target_set_) {
                // Keep configured targets coherent across reconnects.
                protocol.set_target_ra(target_ra_hours_);
                protocol.set_target_dec(target_dec_degrees_);
            }

            try {
                const double guide_rate = protocol.get_guide_rate();
                guide_rate_ = {guide_rate, guide_rate};
            } catch (const std::exception&) {
            }

            try {
                tracking_rate_cached_ = protocol.get_tracking_rate();
                tracking_rate_valid_ = true;
            } catch (const std::exception&) {
            }

            if (sync_time_on_connect_) {
                const auto now = std::chrono::system_clock::now();
                if (site_coords_valid_) {
                    SiteInfo site;
                    site.latitude_degrees = site_latitude_deg_;
                    site.longitude_degrees = site_longitude_deg_;
                    protocol.set_site_info(site);
                }

                // Use UTC timezone on mount time sync to avoid timezone-sign ambiguities.
                const int offset_minutes = 0;
                TimeInfo info = from_utc_time_point(now, offset_minutes);
                protocol.set_time_info(info);
                last_utc_set_ = now;
                last_utc_set_monotonic_ = std::chrono::steady_clock::now();
                last_utc_valid_ = true;
                timezone_offset_minutes_ = offset_minutes;
                timezone_valid_ = true;
            }
            return;
        }

        protocol.disconnect();
        connected_.store(false);
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        slew_force_until_ = std::chrono::steady_clock::time_point{};
        pulse_guiding_end_ = std::chrono::steady_clock::time_point{};
        parked_cached_ = false;
        park_command_active_ = false;
        park_command_started_ = std::chrono::steady_clock::time_point{};
        park_motion_seen_ = false;
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        state.push_back({"Connected", connected_.load()});
        state.push_back({"Connecting", connecting_.load()});

        if (!connected_.load()) {
            return state;
        }

        try {
            state.push_back({"Tracking", get_tracking()});
        } catch (const std::exception&) {
        }
        try {
            state.push_back({"Slewing", get_slewing()});
        } catch (const std::exception&) {
        }
        try {
            state.push_back({"AtHome", get_at_home()});
        } catch (const std::exception&) {
        }
        try {
            state.push_back({"AtPark", get_at_park()});
        } catch (const std::exception&) {
        }
        try {
            state.push_back({"RightAscension", get_right_ascension()});
            state.push_back({"Declination", get_declination()});
            state.push_back({"Altitude", get_altitude()});
            state.push_back({"Azimuth", get_azimuth()});
        } catch (const std::exception&) {
        }

        return state;
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

    std::string command_blind(std::string_view command, bool) override {
        check_connected();
        ZWOMountProtocolWrapper::instance().send_command_blind(std::string(command));
        return "";
    }

    bool command_bool(std::string_view command, bool) override {
        check_connected();
        const std::string response = ZWOMountProtocolWrapper::instance().send_command(std::string(command), false);
        if (response.empty()) {
            return false;
        }
        const char ch = response[0];
        return ch == '1' || ch == 'T' || ch == 't' || ch == 'Y' || ch == 'y';
    }

    std::string command_string(std::string_view command, bool) override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().send_command(std::string(command), false);
    }

    AlignmentMode get_alignment_mode() const override {
        check_connected();
        const StatusInfo status = ZWOMountProtocolWrapper::instance().get_status();
        if (status.mode == MountMode::AltAzimuth) {
            return AlignmentMode::AltAz;
        }
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().get_current_horizontal().altitude_degrees;
    }

    double get_aperture_diameter() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override {
        if (!std::isfinite(meters) || meters < 0.0) {
            throw AlpacaException("ApertureDiameter must be >= 0", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        aperture_diameter_m_ = meters;
        const double radius = meters * 0.5;
        aperture_area_m2_ = std::numbers::pi * radius * radius;
    }

    double get_aperture_area() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return aperture_area_m2_;
    }

    bool get_at_home() const override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().get_status().at_home;
    }

    bool get_at_park() const override {
        check_connected();
        auto& protocol = ZWOMountProtocolWrapper::instance();

        std::optional<ParkStatus> park_status;
        try {
            park_status = protocol.get_park_status();
        } catch (const std::exception&) {
        }

        std::optional<StatusInfo> status;
        bool park_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            park_active = park_command_active_;
        }
        if (park_active || (park_status.has_value() && park_status.value() == ParkStatus::InProgress)) {
            try {
                status = protocol.get_status();
            } catch (const std::exception&) {
            }
        }

        const ParkEvaluation park_eval = evaluate_park_state(park_status, status, std::chrono::steady_clock::now());
        if (park_eval == ParkEvaluation::Parked) {
            return true;
        }
        if (park_eval == ParkEvaluation::InProgress || park_eval == ParkEvaluation::NotParked) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        return parked_cached_;
    }

    double get_azimuth() const override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().get_current_horizontal().azimuth_degrees;
    }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        return pulse_guiding_end_ > now;
    }

    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return true; }
    bool get_can_set_park() const override { return true; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_tracking() const override { return true; }
    bool get_can_slew_alt_az() const override { return false; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return true; }
    bool get_can_slew_async() const override { return true; }
    bool get_can_sync() const override { return true; }
    bool get_can_unpark() const override { return true; }

    double get_declination() const override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().get_current_equatorial().dec_degrees;
    }

    double get_declination_rate() const override {
        return 0.0;
    }

    void set_declination_rate(double) override {
        throw AlpacaException("DeclinationRate is not supported", AlpacaError::NotImplemented);
    }

    bool get_tracking() const override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().get_tracking_enabled();
    }

    void set_tracking(bool tracking) override {
        check_connected();
        auto& protocol = ZWOMountProtocolWrapper::instance();
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                protocol.set_tracking_enabled(tracking);
                return;
            } catch (const AlpacaException&) {
                if (!tracking && attempt < 2) {
                    // :Td can fail while firmware still reports "moving"; force a stop and retry.
                    protocol.abort_motion();
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    continue;
                }
                throw;
            }
        }
    }

    double get_focal_length() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focal_length_m_;
    }

    void set_focal_length(double meters) override {
        if (!std::isfinite(meters) || meters < 0.0) {
            throw AlpacaException("FocalLength must be >= 0", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        if (connected_.load()) {
            try {
                const double guide_rate = ZWOMountProtocolWrapper::instance().get_guide_rate();
                std::lock_guard<std::mutex> lock(mutex_);
                guide_rate_ = {guide_rate, guide_rate};
            } catch (const std::exception&) {
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate values must be finite", AlpacaError::InvalidValue);
        }

        const double effective = 0.5 * (rate.ra + rate.dec);
        if (effective < 0.10 || effective > 0.90) {
            throw AlpacaException("GuideRate must be in [0.10,0.90]", AlpacaError::InvalidValue);
        }

        if (connected_.load()) {
            ZWOMountProtocolWrapper::instance().set_guide_rate(effective);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        guide_rate_ = {effective, effective};
    }

    double get_right_ascension() const override {
        check_connected();
        return ZWOMountProtocolWrapper::instance().get_current_equatorial().ra_hours;
    }

    double get_right_ascension_rate() const override {
        return 0.0;
    }

    void set_right_ascension_rate(double) override {
        throw AlpacaException("RightAscensionRate is not supported", AlpacaError::NotImplemented);
    }

    int get_side_of_pier() const override {
        check_connected();
        const char direction = ZWOMountProtocolWrapper::instance().get_mount_direction();
        if (direction == 'E' || direction == 'e') {
            return 0;
        }
        if (direction == 'W' || direction == 'w') {
            return 1;
        }
        return -1;
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
        (void)ra;
        (void)dec;
        return get_side_of_pier();
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::Topocentric;
    }

    bool get_does_refraction() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return does_refraction_;
    }

    void set_does_refraction(bool does_refraction) override {
        std::lock_guard<std::mutex> lock(mutex_);
        does_refraction_ = does_refraction;
    }

    int get_slew_settle_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return slew_settle_time_s_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("SlewSettleTime must be >= 0", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        slew_settle_time_s_ = seconds;
    }

    void set_side_of_pier(int) override {
        throw AlpacaException("SideOfPier is read-only for ZWO mount", AlpacaError::NotImplemented);
    }

    double get_sidereal_time() const override {
        check_connected();
        double longitude = 0.0;
        bool has_longitude = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (site_coords_valid_) {
                longitude = site_longitude_deg_;
                has_longitude = true;
            }
        }
        if (!has_longitude) {
            try {
                const SiteInfo site = ZWOMountProtocolWrapper::instance().get_site_info();
                longitude = site.longitude_degrees;
                std::lock_guard<std::mutex> lock(mutex_);
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
            } catch (const std::exception&) {
            }
        }
        return compute_local_sidereal_time_hours(std::chrono::system_clock::now(), longitude);
    }

    double get_site_elevation() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return site_elevation_m_;
    }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < kMinSiteElevationMeters || elevation > kMaxSiteElevationMeters) {
            throw AlpacaException("SiteElevation must be in [-300,10000] meters", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        site_elevation_m_ = elevation;
        pending_site_elevation_ = elevation;
    }

    double get_site_latitude() const override {
        if (connected_.load()) {
            try {
                const SiteInfo site = ZWOMountProtocolWrapper::instance().get_site_info();
                std::lock_guard<std::mutex> lock(mutex_);
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
            } catch (const std::exception&) {
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_coords_valid_) {
            throw AlpacaException("Site coordinates not available", AlpacaError::ValueNotSet);
        }
        return site_latitude_deg_;
    }

    void set_site_latitude(double latitude) override {
        validate_latitude(latitude);

        std::lock_guard<std::mutex> lock(mutex_);
        site_latitude_deg_ = latitude;
        site_coords_valid_ = true;
        pending_site_latitude_ = latitude;

        const bool has_longitude = pending_site_longitude_.has_value() || site_coords_valid_;
        const double longitude = pending_site_longitude_.has_value() ? pending_site_longitude_.value() : site_longitude_deg_;
        if (connected_.load() && has_longitude) {
            SiteInfo site;
            site.latitude_degrees = site_latitude_deg_;
            site.longitude_degrees = longitude;
            ZWOMountProtocolWrapper::instance().set_site_info(site);
            pending_site_longitude_ = site.longitude_degrees;
            site_longitude_deg_ = site.longitude_degrees;
        }
    }

    double get_site_longitude() const override {
        if (connected_.load()) {
            try {
                const SiteInfo site = ZWOMountProtocolWrapper::instance().get_site_info();
                std::lock_guard<std::mutex> lock(mutex_);
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
            } catch (const std::exception&) {
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_coords_valid_) {
            throw AlpacaException("Site coordinates not available", AlpacaError::ValueNotSet);
        }
        return site_longitude_deg_;
    }

    void set_site_longitude(double longitude) override {
        validate_longitude(longitude);

        std::lock_guard<std::mutex> lock(mutex_);
        site_longitude_deg_ = longitude;
        site_coords_valid_ = true;
        pending_site_longitude_ = longitude;

        const bool has_latitude = pending_site_latitude_.has_value() || site_coords_valid_;
        const double latitude = pending_site_latitude_.has_value() ? pending_site_latitude_.value() : site_latitude_deg_;
        if (connected_.load() && has_latitude) {
            SiteInfo site;
            site.latitude_degrees = latitude;
            site.longitude_degrees = site_longitude_deg_;
            ZWOMountProtocolWrapper::instance().set_site_info(site);
            pending_site_latitude_ = site.latitude_degrees;
            site_latitude_deg_ = site.latitude_degrees;
        }
    }

    bool get_can_move_axis(int axis) const override {
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        check_connected();
        ensure_not_parked("MoveAxis");
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(rate) || std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("Axis rate out of range", AlpacaError::InvalidValue);
        }

        auto& protocol = ZWOMountProtocolWrapper::instance();

        if (std::abs(rate) < 1e-9) {
            if (axis == 0) {
                protocol.stop_move_east();
                protocol.stop_move_west();
            } else {
                protocol.stop_move_north();
                protocol.stop_move_south();
            }
            // Some firmware leaves motion state latched unless a generic stop is issued.
            protocol.abort_motion();

            std::lock_guard<std::mutex> lock(mutex_);
            manual_axis_slewing_[axis] = false;
            park_command_active_ = false;
            park_motion_seen_ = false;
            return;
        }

        const double multiplier = std::abs(rate) / kSiderealRateDegPerSec;
        protocol.set_move_rate_sidereal_multiple(multiplier);

        if (axis == 0) {
            if (rate > 0.0) {
                protocol.start_move_east();
            } else {
                protocol.start_move_west();
            }
        } else {
            if (rate > 0.0) {
                protocol.start_move_north();
            } else {
                protocol.start_move_south();
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[axis] = true;
        park_command_active_ = false;
        park_motion_seen_ = false;
        parked_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 0 || axis == 1) {
            return {0.0, kMaxMoveAxisRateDegPerSec};
        }
        return {0.0, 0.0};
    }

    bool get_slewing() const override {
        check_connected();

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
                return true;
            }
            if (slew_force_until_ > now) {
                return true;
            }
        }

        auto& protocol = ZWOMountProtocolWrapper::instance();
        std::optional<ParkStatus> park_status;
        try {
            park_status = protocol.get_park_status();
        } catch (const std::exception&) {
        }

        std::optional<StatusInfo> status;
        bool park_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            park_active = park_command_active_;
        }
        if (park_active || (park_status.has_value() && park_status.value() == ParkStatus::InProgress)) {
            try {
                status = protocol.get_status();
            } catch (const std::exception&) {
            }
        }

        const ParkEvaluation park_eval = evaluate_park_state(park_status, status, now);
        if (park_eval == ParkEvaluation::InProgress) {
            return true;
        }
        if (park_eval == ParkEvaluation::Parked) {
            return false;
        }

        const StatusInfo mount_status = status.has_value() ? status.value() : protocol.get_status();
        return !mount_status.stop_or_tracking;
    }

    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_set_) {
            throw AlpacaException("TargetDeclination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        validate_dec(dec, "TargetDeclination");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_dec_degrees_ = dec;
            target_set_ = true;
        }

        if (connected_.load()) {
            ZWOMountProtocolWrapper::instance().set_target_dec(dec);
        }
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_set_) {
            throw AlpacaException("TargetRightAscension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        validate_ra(ra, "TargetRightAscension");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_set_ = true;
        }

        if (connected_.load()) {
            ZWOMountProtocolWrapper::instance().set_target_ra(ra);
        }
    }

    int get_tracking_rate() const override {
        check_connected();
        const int rate = ZWOMountProtocolWrapper::instance().get_tracking_rate();
        std::lock_guard<std::mutex> lock(mutex_);
        tracking_rate_cached_ = rate;
        tracking_rate_valid_ = true;
        return rate;
    }

    void set_tracking_rate(int rate) override {
        if (rate < 0 || rate > 2) {
            throw AlpacaException("TrackingRate must be 0 (sidereal), 1 (lunar), or 2 (solar)",
                                  AlpacaError::InvalidValue);
        }
        check_connected();

        ZWOMountProtocolWrapper::instance().set_tracking_rate(rate);

        std::lock_guard<std::mutex> lock(mutex_);
        tracking_rate_cached_ = rate;
        tracking_rate_valid_ = true;
    }

    std::vector<int> get_tracking_rates() const override {
        return {0, 1, 2};
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        if (connected_.load()) {
            const TimeInfo mount_time = ZWOMountProtocolWrapper::instance().get_time_info();
            const auto utc = to_utc_time_point(mount_time);
            std::lock_guard<std::mutex> lock(mutex_);
            last_utc_set_ = utc;
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
            timezone_offset_minutes_ = mount_time.timezone_offset_minutes;
            timezone_valid_ = true;
            return utc;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (last_utc_valid_) {
            const auto elapsed = std::chrono::steady_clock::now() - last_utc_set_monotonic_;
            return last_utc_set_ +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
        }

        return std::chrono::system_clock::now();
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        // Use UTC timezone on mount time sync to avoid timezone-sign ambiguities.
        const int offset_minutes = 0;
        TimeInfo info = from_utc_time_point(utc, offset_minutes);

        if (connected_.load()) {
            ZWOMountProtocolWrapper::instance().set_time_info(info);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
        timezone_offset_minutes_ = offset_minutes;
        timezone_valid_ = true;
    }

    void find_home() override {
        check_connected();
        ensure_not_parked("FindHome");
        ZWOMountProtocolWrapper::instance().go_home();
        std::lock_guard<std::mutex> lock(mutex_);
        parked_cached_ = false;
        park_command_active_ = false;
        park_motion_seen_ = false;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    void park() override {
        check_connected();
        if (get_at_park()) {
            return;
        }
        auto& protocol = ZWOMountProtocolWrapper::instance();
        try {
            if (protocol.get_status().mode == MountMode::AltAzimuth) {
                throw AlpacaException(
                    "Park is only supported by the mount in equatorial (GEM) mode",
                    AlpacaError::InvalidOperation);
            }
        } catch (const AlpacaException&) {
            throw;
        } catch (const std::exception&) {
            // Continue: older firmware may not expose mode flags consistently.
        }

        protocol.park();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            if (protocol.get_park_status() == ParkStatus::Error) {
                throw AlpacaException("Mount reported park error after :hP", AlpacaError::InvalidOperation);
            }
        } catch (const AlpacaException&) {
            throw;
        } catch (const std::exception&) {
            // Some firmware only reports park status after motion starts.
        }

        std::lock_guard<std::mutex> lock(mutex_);
        parked_cached_ = false;
        park_command_active_ = true;
        park_command_started_ = std::chrono::steady_clock::now();
        park_motion_seen_ = false;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    void abort_slew() override {
        check_connected();
        ensure_not_parked("AbortSlew");
        ZWOMountProtocolWrapper::instance().abort_motion();
        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        park_command_active_ = false;
        park_motion_seen_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point{};
    }

    void pulse_guide(int direction, int duration) override {
        check_connected();
        ensure_not_parked("PulseGuide");
        if (duration < 0) {
            throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
        }

        const int effective_duration = std::clamp(duration, 0, 3000);
        if (duration > effective_duration) {
            ALPACA_LOG_WARN(
                "ZWO",
                "PulseGuide duration " + std::to_string(duration) +
                    "ms exceeds mount protocol max 3000ms; clamping to 3000ms");
        }

        auto& protocol = ZWOMountProtocolWrapper::instance();

        const bool ra_axis = (direction == 2 || direction == 3);
        const bool dec_axis = (direction == 0 || direction == 1);
        if (!ra_axis && !dec_axis) {
            throw AlpacaException("PulseGuide direction must be 0..3", AlpacaError::InvalidValue);
        }

        double guide_rate_fraction = 0.5;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            guide_rate_fraction = ra_axis ? guide_rate_.ra : guide_rate_.dec;
        }
        guide_rate_fraction = std::clamp(guide_rate_fraction, 0.10, 0.90);

        // Some AM firmware builds acknowledge :Mg but do not produce measurable axis motion.
        // Use timed directional movement at guide-rate speed so PulseGuide remains effective.
        protocol.set_move_rate_sidereal_multiple(guide_rate_fraction);
        switch (direction) {
        case 0: protocol.start_move_north(); break;
        case 1: protocol.start_move_south(); break;
        case 2: protocol.start_move_east(); break;
        case 3: protocol.start_move_west(); break;
        default:
            throw AlpacaException("PulseGuide direction must be 0..3", AlpacaError::InvalidValue);
        }

        std::thread([direction, effective_duration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(effective_duration));
            try {
                auto& protocol = ZWOMountProtocolWrapper::instance();
                switch (direction) {
                case 0: protocol.stop_move_north(); break;
                case 1: protocol.stop_move_south(); break;
                case 2: protocol.stop_move_east(); break;
                case 3: protocol.stop_move_west(); break;
                default: break;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "PulseGuide stop command failed: " + std::string(e.what()));
            }
        }).detach();

        std::lock_guard<std::mutex> lock(mutex_);
        pulse_guiding_end_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(effective_duration);
    }

    void set_park() override {
        check_connected();
        if (!ZWOMountProtocolWrapper::instance().set_custom_park_here()) {
            throw AlpacaException("Mount rejected set park request", AlpacaError::InvalidOperation);
        }
    }

    void slew_to_coordinates(double ra, double dec) override {
        ensure_not_parked("SlewToCoordinates");
        set_target_right_ascension(ra);
        set_target_declination(dec);
        slew_to_target();
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        ensure_not_parked("SlewToCoordinatesAsync");
        set_target_right_ascension(ra);
        set_target_declination(dec);
        slew_to_target_async();
    }

    void slew_to_target() override {
        ensure_not_parked("SlewToTarget");
        slew_to_target_async();

        const auto deadline = std::chrono::steady_clock::now() + kDefaultSlewTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!get_slewing()) {
                const int settle = get_slew_settle_time();
                if (settle > 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(settle));
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        throw AlpacaException("Slew operation timed out", AlpacaError::DriverException);
    }

    void slew_to_target_async() override {
        check_connected();
        ensure_not_parked("SlewToTargetAsync");

        double target_ra = 0.0;
        double target_dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_set_) {
                throw AlpacaException("Target coordinates not set", AlpacaError::ValueNotSet);
            }
            target_ra = target_ra_hours_;
            target_dec = target_dec_degrees_;
        }

        validate_ra(target_ra, "RightAscension");
        validate_dec(target_dec, "Declination");

        auto& protocol = ZWOMountProtocolWrapper::instance();
        protocol.set_target_ra(target_ra);
        protocol.set_target_dec(target_dec);
        bool used_inverted_longitude_retry = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                if (!protocol.goto_target()) {
                    throw AlpacaException("Mount rejected GOTO request", AlpacaError::InvalidOperation);
                }
                if (used_inverted_longitude_retry) {
                    ALPACA_LOG_WARN("ZWO", "GOTO succeeded after longitude-sign inversion retry");
                }
                break;
            } catch (const AlpacaException& ex) {
                if (attempt < 2 && is_ms_mount_busy_error(ex)) {
                    ALPACA_LOG_WARN("ZWO", "GOTO rejected with e3 (mount busy); aborting motion and retrying");
                    protocol.abort_motion();
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    continue;
                }
                if (attempt == 0 &&
                    (is_ms_time_site_not_synchronized_error(ex) ||
                     is_ms_target_under_horizon_error(ex))) {
                    ALPACA_LOG_WARN(
                        "ZWO",
                        "GOTO rejected with " +
                            std::string(is_ms_time_site_not_synchronized_error(ex) ? "e7" : "e5") +
                            "; synchronizing site/time and retrying once");
                    synchronize_mount_time_and_site_for_goto(false);
                    continue;
                }
                if (attempt == 1 && is_ms_target_under_horizon_error(ex)) {
                    ALPACA_LOG_WARN(
                        "ZWO",
                        "GOTO still rejected with e5 after normal sync; retrying with inverted longitude sign");
                    synchronize_mount_time_and_site_for_goto(true);
                    used_inverted_longitude_retry = true;
                    continue;
                }

                if (is_ms_time_site_not_synchronized_error(ex)) {
                    throw AlpacaException(
                        "GOTO rejected by mount (e7: time and position not synchronized). "
                        "Set SiteLatitude/SiteLongitude and UTCDate, or enable syncTimeOnConnect.",
                        AlpacaError::InvalidOperation);
                }
                if (is_ms_target_under_horizon_error(ex)) {
                    throw AlpacaException(
                        "GOTO rejected by mount (e5: target under horizon). "
                        "This can indicate mount time/site mismatch; verify SiteLatitude/SiteLongitude and UTCDate.",
                        AlpacaError::InvalidOperation);
                }
                throw;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        parked_cached_ = false;
        park_command_active_ = false;
        park_motion_seen_ = false;
    }

    void sync_to_coordinates(double ra, double dec) override {
        ensure_not_parked("SyncToCoordinates");
        set_target_right_ascension(ra);
        set_target_declination(dec);
        sync_to_target();
    }

    void sync_to_target() override {
        check_connected();
        ensure_not_parked("SyncToTarget");

        double target_ra = 0.0;
        double target_dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_set_) {
                throw AlpacaException("Target coordinates not set", AlpacaError::ValueNotSet);
            }
            target_ra = target_ra_hours_;
            target_dec = target_dec_degrees_;
        }

        validate_ra(target_ra, "RightAscension");
        validate_dec(target_dec, "Declination");

        auto& protocol = ZWOMountProtocolWrapper::instance();
        protocol.set_target_ra(target_ra);
        protocol.set_target_dec(target_dec);
        protocol.sync_target();
    }

    void slew_to_alt_az(double, double) override {
        throw AlpacaException("SlewToAltAz is not supported by ZWO mount protocol", AlpacaError::NotImplemented);
    }

    void slew_to_alt_az_async(double, double) override {
        throw AlpacaException("SlewToAltAzAsync is not supported by ZWO mount protocol", AlpacaError::NotImplemented);
    }

    void sync_to_alt_az(double, double) override {
        throw AlpacaException("SyncToAltAz is not supported by ZWO mount protocol", AlpacaError::NotImplemented);
    }

    void unpark() override {
        check_connected();
        auto& protocol = ZWOMountProtocolWrapper::instance();
        if (!protocol.unpark()) {
            std::optional<ParkStatus> park_status;
            try {
                park_status = protocol.get_park_status();
            } catch (const std::exception&) {
            }

            if (!(park_status == ParkStatus::Unknown || park_status == ParkStatus::NotParked ||
                  !park_status.has_value())) {
                throw AlpacaException("Mount rejected unpark request", AlpacaError::InvalidOperation);
            }

            ALPACA_LOG_WARN(
                "ZWO",
                "Unpark returned 0 while :Gps is unavailable/not-parked; treating unpark as successful");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        parked_cached_ = false;
        park_command_active_ = false;
        park_motion_seen_ = false;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }

private:
    enum class ParkEvaluation {
        Unknown,
        NotParked,
        InProgress,
        Parked
    };

    ParkEvaluation evaluate_park_state(const std::optional<ParkStatus>& park_status,
                                       const std::optional<StatusInfo>& mount_status,
                                       std::chrono::steady_clock::time_point now) const {
        std::lock_guard<std::mutex> lock(mutex_);

        auto infer_with_unavailable_park_status = [&]() -> ParkEvaluation {
            if (!park_command_active_) {
                return parked_cached_ ? ParkEvaluation::Parked : ParkEvaluation::Unknown;
            }

            // Some firmware revisions do not implement :Gps; infer completion from motion state after :hP.
            if (!mount_status.has_value()) {
                return ParkEvaluation::InProgress;
            }
            if (!mount_status->stop_or_tracking) {
                park_motion_seen_ = true;
                return ParkEvaluation::InProgress;
            }
            if (park_motion_seen_ || now - park_command_started_ >= kParkMotionSettle) {
                ALPACA_LOG_WARN(
                    "ZWO",
                    ":Gps unavailable after park command; mount is stationary, inferring park complete");
                park_command_active_ = false;
                park_motion_seen_ = false;
                parked_cached_ = true;
                return ParkEvaluation::Parked;
            }
            return ParkEvaluation::InProgress;
        };

        if (!park_status.has_value()) {
            return infer_with_unavailable_park_status();
        }

        switch (park_status.value()) {
        case ParkStatus::Unknown:
            return infer_with_unavailable_park_status();
        case ParkStatus::Completed:
            park_command_active_ = false;
            park_motion_seen_ = false;
            parked_cached_ = true;
            return ParkEvaluation::Parked;
        case ParkStatus::NotParked:
            if (!park_command_active_) {
                park_motion_seen_ = false;
                parked_cached_ = false;
                return ParkEvaluation::NotParked;
            }

            if (mount_status.has_value()) {
                if (!mount_status->stop_or_tracking) {
                    park_motion_seen_ = true;
                    return ParkEvaluation::InProgress;
                }

                if (park_motion_seen_ || now - park_command_started_ >= kParkMotionSettle) {
                    ALPACA_LOG_WARN(
                        "ZWO",
                        ":Gps reported not-parked after park command, but mount is stationary; inferring park complete");
                    park_command_active_ = false;
                    park_motion_seen_ = false;
                    parked_cached_ = true;
                    return ParkEvaluation::Parked;
                }
            }

            return ParkEvaluation::InProgress;
        case ParkStatus::Error:
            park_command_active_ = false;
            park_motion_seen_ = false;
            parked_cached_ = false;
            return ParkEvaluation::NotParked;
        case ParkStatus::InProgress:
            if (!park_command_active_) {
                // :Gps can report stale in-progress when no park command is active.
                return parked_cached_ ? ParkEvaluation::Parked : ParkEvaluation::Unknown;
            }

            if (mount_status.has_value()) {
                if (!mount_status->stop_or_tracking) {
                    park_motion_seen_ = true;
                    return ParkEvaluation::InProgress;
                }

                if (park_motion_seen_ || now - park_command_started_ >= kParkMotionSettle) {
                    ALPACA_LOG_WARN(
                        "ZWO",
                        ":Gps remained in-progress but mount is stationary; inferring park complete");
                    park_command_active_ = false;
                    park_motion_seen_ = false;
                    parked_cached_ = true;
                    return ParkEvaluation::Parked;
                }
            }

            return ParkEvaluation::InProgress;
        default:
            return parked_cached_ ? ParkEvaluation::Parked : ParkEvaluation::Unknown;
        }
    }

    bool is_ms_time_site_not_synchronized_error(const AlpacaException& ex) const {
        const std::string message = ex.what();
        return message.find(":MS") != std::string::npos && message.find("e7") != std::string::npos;
    }

    bool is_ms_target_under_horizon_error(const AlpacaException& ex) const {
        const std::string message = ex.what();
        return message.find(":MS") != std::string::npos && message.find("e5") != std::string::npos;
    }

    bool is_ms_mount_busy_error(const AlpacaException& ex) const {
        const std::string message = ex.what();
        return message.find(":MS") != std::string::npos && message.find("e3") != std::string::npos;
    }

    void ensure_not_parked(const char* operation) const {
        if (!connected_.load()) {
            return;
        }

        bool parked = false;
        try {
            parked = get_at_park();
        } catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(mutex_);
            parked = parked_cached_;
        }

        if (parked) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidOperation);
        }
    }

    void synchronize_mount_time_and_site_for_goto(bool invert_longitude_sign) {
        auto& protocol = ZWOMountProtocolWrapper::instance();

        std::optional<SiteInfo> site_to_write;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value()) {
                SiteInfo pending;
                pending.latitude_degrees = pending_site_latitude_.value();
                pending.longitude_degrees = pending_site_longitude_.value();
                site_to_write = pending;
            } else if (site_coords_valid_) {
                SiteInfo cached;
                cached.latitude_degrees = site_latitude_deg_;
                cached.longitude_degrees = site_longitude_deg_;
                site_to_write = cached;
            }
        }

        if (!site_to_write.has_value()) {
            try {
                site_to_write = protocol.get_site_info();
            } catch (const std::exception&) {
            }
        }

        if (site_to_write.has_value()) {
            SiteInfo normalized = site_to_write.value();
            if (invert_longitude_sign) {
                normalized.longitude_degrees = -normalized.longitude_degrees;
            }
            protocol.set_site_info(normalized);
            if (!invert_longitude_sign) {
                std::lock_guard<std::mutex> lock(mutex_);
                site_latitude_deg_ = site_to_write->latitude_degrees;
                site_longitude_deg_ = site_to_write->longitude_degrees;
                site_coords_valid_ = true;
            }
        }

        const auto now = std::chrono::system_clock::now();
        const int offset_minutes = 0;
        const TimeInfo info = from_utc_time_point(now, offset_minutes);
        protocol.set_time_info(info);

        std::lock_guard<std::mutex> lock(mutex_);
        last_utc_set_ = now;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
        timezone_offset_minutes_ = offset_minutes;
        timezone_valid_ = true;
    }

    void check_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Telescope is not connected", AlpacaError::NotConnected);
        }
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connecting_.load()) {
            return;
        }

        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }

        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR("ZWO", "ZWO mount connection change failed: " + std::string(e.what()));
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    const int device_number_;
    const ConnectionInfo connection_info_;

    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;

    mutable std::mutex mutex_;
    std::thread connection_thread_;
    mutable std::mutex connection_mutex_;

    std::string mount_info_;

    double target_ra_hours_;
    double target_dec_degrees_;
    bool target_set_;

    double aperture_diameter_m_;
    double aperture_area_m2_;
    double focal_length_m_;

    mutable double site_latitude_deg_;
    mutable double site_longitude_deg_;
    mutable bool site_coords_valid_;
    double site_elevation_m_;

    bool does_refraction_;
    int slew_settle_time_s_;

    mutable GuideRate guide_rate_;
    mutable int tracking_rate_cached_;
    mutable bool tracking_rate_valid_;

    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;

    mutable int timezone_offset_minutes_;
    mutable bool timezone_valid_;

    std::array<bool, 2> manual_axis_slewing_;
    std::chrono::steady_clock::time_point slew_force_until_;
    std::chrono::steady_clock::time_point pulse_guiding_end_;
    mutable bool parked_cached_;
    mutable bool park_command_active_;
    mutable std::chrono::steady_clock::time_point park_command_started_;
    mutable bool park_motion_seen_;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
};

std::unique_ptr<TelescopeDriver> create_zwo_telescope(
    int device_number,
    const ConnectionInfo& connection_info) {
    return create_zwo_telescope_with_site(
        device_number,
        connection_info,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_zwo_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect) {
    return std::make_unique<ZWOTelescopeDriver>(
        device_number,
        connection_info,
        site_latitude_deg,
        site_longitude_deg,
        site_elevation_m,
        sync_time_on_connect);
}

} // namespace alpacacore::vendor::zwo

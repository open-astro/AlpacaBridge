// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
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

#include <alpacacore/async_connectable.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>

namespace alpacacore::vendor::bisque {

namespace {

constexpr double kHoursToDegrees = 15.0;
constexpr auto kPositionCacheTtl = std::chrono::seconds(2);

// Sidereal rate in arcseconds per second (INDI TRACKRATE_SIDEREAL).
constexpr double kSiderealRateArcsecPerSec = 15.0411;

// Slew speed presets (multiples of sidereal rate).
constexpr int kNumSlewSpeeds = 9;
constexpr double kSlewSpeeds[kNumSlewSpeeds] = {
    1.0, 2.0, 4.0, 8.0, 32.0, 64.0, 128.0, 256.0, 512.0
};

// Maximum move axis rate in degrees per second (512x sidereal).
constexpr double kMaxMoveAxisRateDegPerSec =
    kSlewSpeeds[kNumSlewSpeeds - 1] * kSiderealRateArcsecPerSec / 3600.0;

// Default guide rate: 50% of sidereal, in degrees per second.
constexpr double kDefaultGuideRateDegPerSec = 0.5 * kSiderealRateArcsecPerSec / 3600.0;

// Find the closest slewspeed index for a given rate in degrees/sec.
int rate_to_speed_index(double rate_deg_per_sec) {
    double abs_rate = std::abs(rate_deg_per_sec);
    double rate_sidereal = abs_rate / (kSiderealRateArcsecPerSec / 3600.0);

    int best = 0;
    double best_diff = std::abs(rate_sidereal - kSlewSpeeds[0]);
    for (int i = 1; i < kNumSlewSpeeds; ++i) {
        double diff = std::abs(rate_sidereal - kSlewSpeeds[i]);
        if (diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    return best;
}

double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time,
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

double shortest_ra_delta_hours(double a, double b) {
    double delta = a - b;
    while (delta > 12.0) delta -= 24.0;
    while (delta < -12.0) delta += 24.0;
    return delta;
}

} // namespace

class BisqueTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    BisqueTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                          std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                          std::optional<double> site_elevation_m)
        : AsyncConnectable("Bisque"),
          device_number_(device_number),
          connection_info_(connection_info),
          site_latitude_(site_latitude_deg.value_or(0.0)),
          site_longitude_(site_longitude_deg.value_or(0.0)),
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          site_info_valid_(site_latitude_deg.has_value() && site_longitude_deg.has_value()) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~BisqueTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_) {
            try {
                set_connected(false);
            } catch (...) {
            }
        }
    }

    // ── AlpacaDriver base methods ──

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "Bisque Paramount Telescope";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string get_unique_id() const override {
        return "Bisque_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "Bisque Paramount / TheSkyX Mount Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore Bisque TheSkyX Driver v0.1";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

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

        auto& protocol = BisqueProtocolWrapper::instance();
        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to TheSkyX at " +
                                      connection_info_.host + ":" +
                                      std::to_string(connection_info_.tcp_port));
            }
            if (!protocol.handshake()) {
                protocol.disconnect();
                throw AlpacaException("TheSkyX handshake failed — is the mount connected in TheSkyX?");
            }
            connected_ = true;
            target_set_ = false;
            parked_ = false;
            at_home_ = false;
            homing_ = false;
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            pulse_guiding_ = false;

            // Check initial park/tracking state.
            try {
                parked_ = protocol.is_parked();
            } catch (...) {
            }

            // Warm caches so first property reads are fast.
            try {
                auto pos = protocol.get_ra_dec();
                cached_ra_hours_ = pos.ra_hours;
                cached_dec_degrees_ = pos.dec_degrees;
                equatorial_cache_valid_ = true;
                last_equatorial_update_ = std::chrono::steady_clock::now();
            } catch (...) {
            }
            try {
                auto altaz = protocol.get_alt_az();
                cached_alt_degrees_ = altaz.altitude_degrees;
                cached_az_degrees_ = altaz.azimuth_degrees;
                altaz_cache_valid_ = true;
                last_altaz_update_ = std::chrono::steady_clock::now();
            } catch (...) {
            }

            ALPACA_LOG_INFO("Bisque", "Connected to TheSkyX, parked=" +
                            std::string(parked_ ? "true" : "false"));
        } else {
            try {
                protocol.disconnect();
            } catch (...) {
            }
            connected_ = false;
            target_set_ = false;
            parked_ = false;
            at_home_ = false;
            homing_ = false;
            slewing_cached_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            pulse_guiding_ = false;
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view /*action_parameters*/) override {
        throw AlpacaException("Action not supported: " + std::string(action_name));
    }

    bool can_action(std::string_view /*action_name*/) const override {
        return false;
    }

    std::string command_blind(std::string_view command, bool /*raw*/) override {
        throw AlpacaException("CommandBlind not supported: " + std::string(command));
    }

    bool command_bool(std::string_view command, bool /*raw*/) override {
        throw AlpacaException("CommandBool not supported: " + std::string(command));
    }

    std::string command_string(std::string_view command, bool /*raw*/) override {
        throw AlpacaException("CommandString not supported: " + std::string(command));
    }

    // ── Telescope properties ──

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
        aperture_diameter_m_ = meters;
        if (meters > 0.0) {
            double radius = meters / 2.0;
            aperture_area_m2_ = std::numbers::pi * radius * radius;
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

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return pulse_guiding_;
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_equatorial_cache_locked();
        return std::clamp(cached_dec_degrees_, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        return 0.0;
    }

    void set_declination_rate(double /*rate*/) override {
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
        auto& protocol = BisqueProtocolWrapper::instance();
        // ignore_rates=true uses sidereal.
        protocol.set_tracking(tracking, true, 0.0, 0.0);
    }

    double get_focal_length() const override {
        return focal_length_m_;
    }

    void set_focal_length(double meters) override {
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        guide_rate_ = rate;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_equatorial_cache_locked();
        return cached_ra_hours_;
    }

    double get_right_ascension_rate() const override {
        return 0.0;
    }

    void set_right_ascension_rate(double /*rate*/) override {
        throw AlpacaException("RightAscension rate not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = BisqueProtocolWrapper::instance();
        int pier = protocol.get_pier_side();
        // TheSkyX: 1 = west side of pier, else = east.
        // Alpaca: 0 = pierEast (OTA west of pier), 1 = pierWest (OTA east of pier).
        return pier == 1 ? 1 : 0;
    }

    void set_side_of_pier(int /*side*/) override {
        throw AlpacaException("SetSideOfPier not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double /*dec*/) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double lst_hours = compute_local_sidereal_time_hours(
            std::chrono::system_clock::now(), site_longitude_);
        double hour_angle = shortest_ra_delta_hours(lst_hours, ra);
        // Positive HA → west of meridian → pier east (0).
        return hour_angle >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::Topocentric;
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
        return compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_);
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
        return site_latitude_;
    }

    void set_site_latitude(double latitude) override {
        if (latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees",
                                  AlpacaError::InvalidValue);
        }
        site_latitude_ = latitude;
        site_info_valid_ = true;
    }

    double get_site_longitude() const override {
        return site_longitude_;
    }

    void set_site_longitude(double longitude) override {
        if (longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees",
                                  AlpacaError::InvalidValue);
        }
        site_longitude_ = longitude;
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
        return 0; // Sidereal
    }

    void set_tracking_rate(int rate) override {
        if (rate == 0) return; // Sidereal — already default
        // TODO: Support solar/lunar/custom rates via SetTracking.
        throw AlpacaException("Only sidereal tracking rate is currently supported",
                              AlpacaError::InvalidValue);
    }

    std::vector<int> get_tracking_rates() const override {
        return {0}; // driveSidereal
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        // TheSkyX uses the host computer's clock — return system time.
        return std::chrono::system_clock::now();
    }

    void set_utc_date(std::chrono::system_clock::time_point /*utc*/) override {
        // TheSkyX manages time from the host computer — no mount time to set.
    }

    // ── Telescope methods ──

    void find_home() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked("FindHome");
        auto& protocol = BisqueProtocolWrapper::instance();
        // FindHome blocks in TheSkyX (with internal sleep loop), up to 60 seconds.
        protocol.find_home();
        homing_ = false;
        at_home_ = true;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
    }

    void park() override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.park();
        // Poll until parked.
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > 120) {
                throw AlpacaException("Park timed out after 120 seconds");
            }
            try {
                if (protocol.is_parked()) break;
            } catch (...) {
            }
            // Release lock briefly so other threads can query state.
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            lock.lock();
        }
        parked_ = true;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
    }

    void pulse_guide(int direction, int duration) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked("PulseGuide");

        auto& protocol = BisqueProtocolWrapper::instance();

        // Convert direction + duration to arcsecond displacement.
        // Formula from INDI reference: displacement = guide_rate * TRACKRATE_SIDEREAL * ms / 1000.
        double ra_arcsec = 0.0;
        double dec_arcsec = 0.0;

        // Guide rates stored in degrees/sec. Convert to fraction of sidereal.
        double ra_rate_fraction = guide_rate_.ra / (kSiderealRateArcsecPerSec / 3600.0);
        double dec_rate_fraction = guide_rate_.dec / (kSiderealRateArcsecPerSec / 3600.0);

        switch (direction) {
            case 0: // North
                dec_arcsec = dec_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            case 1: // South
                dec_arcsec = -dec_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            case 2: // East
                ra_arcsec = ra_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            case 3: // West
                ra_arcsec = -ra_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            default:
                throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }

        pulse_guiding_ = true;
        protocol.guide(ra_arcsec, dec_arcsec);
        pulse_guiding_ = false;
        equatorial_cache_valid_ = false;
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.set_park_position();
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked("SlewToCoordinates");
        validate_ra_dec(ra, dec);

        auto& protocol = BisqueProtocolWrapper::instance();
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;

        protocol.slew_to_ra_dec(ra, dec);
        slewing_cached_ = true;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;

        // Wait for completion.
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked("SlewToCoordinatesAsync");
        validate_ra_dec(ra, dec);

        auto& protocol = BisqueProtocolWrapper::instance();
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;

        protocol.slew_to_ra_dec(ra, dec);
        slewing_cached_ = true;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
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
        check_not_parked("SyncToCoordinates");
        validate_ra_dec(ra, dec);

        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.sync_to_coordinates(ra, dec);
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
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
        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.unpark();
        // Confirm unpark.
        if (protocol.is_parked()) {
            throw AlpacaException("Failed to unpark mount");
        }
        parked_ = false;
    }

    bool get_can_move_axis(int axis) const override {
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked("MoveAxis");

        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }

        auto& protocol = BisqueProtocolWrapper::instance();

        if (std::abs(rate) < 1e-9) {
            // Stop motion.
            protocol.stop_open_loop_motion();
            manual_axis_slewing_[axis] = false;
            return;
        }

        int speed_index = rate_to_speed_index(rate);

        // Map axis + sign to direction.
        // Axis 0 (Primary/RA): positive=East(2), negative=West(3)
        // Axis 1 (Secondary/Dec): positive=North(0), negative=South(1)
        int direction;
        if (axis == 0) {
            direction = rate > 0 ? 2 : 3; // East or West
        } else {
            direction = rate > 0 ? 0 : 1; // North or South
        }

        protocol.start_open_loop_motion(direction, speed_index);
        manual_axis_slewing_[axis] = true;
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) return {};
        return {get_axis_rate_range(axis)};
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.abort();
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
    }

    void slew_to_alt_az(double /*altitude*/, double /*azimuth*/) override {
        throw AlpacaException("SlewToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void slew_to_alt_az_async(double /*altitude*/, double /*azimuth*/) override {
        throw AlpacaException("SlewToAltAzAsync not supported", AlpacaError::MethodNotImplemented);
    }

    void sync_to_alt_az(double /*altitude*/, double /*azimuth*/) override {
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

private:
    void check_connected() const {
        if (!connected_) {
            throw AlpacaException("Not connected to TheSkyX", AlpacaError::NotConnected);
        }
    }

    void check_not_parked(const char* operation) const {
        if (parked_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    static void validate_ra_dec(double ra, double dec) {
        if (ra < 0.0 || ra >= 24.0) {
            throw AlpacaException("RA out of range [0, 24)", AlpacaError::InvalidValue);
        }
        if (dec < -90.0 || dec > 90.0) {
            throw AlpacaException("Dec out of range [-90, 90]", AlpacaError::InvalidValue);
        }
    }

    void refresh_equatorial_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (equatorial_cache_valid_ && (now - last_equatorial_update_) < kPositionCacheTtl) {
            return;
        }
        auto& protocol = BisqueProtocolWrapper::instance();
        auto pos = protocol.get_ra_dec();
        cached_ra_hours_ = pos.ra_hours;
        cached_dec_degrees_ = pos.dec_degrees;
        equatorial_cache_valid_ = true;
        last_equatorial_update_ = now;
    }

    void refresh_altaz_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (altaz_cache_valid_ && (now - last_altaz_update_) < kPositionCacheTtl) {
            return;
        }
        auto& protocol = BisqueProtocolWrapper::instance();
        auto altaz = protocol.get_alt_az();
        cached_alt_degrees_ = altaz.altitude_degrees;
        cached_az_degrees_ = altaz.azimuth_degrees;
        altaz_cache_valid_ = true;
        last_altaz_update_ = now;
    }

    bool get_slewing_locked() const {
        // Within the slew-force window, report as slewing without querying.
        if (slewing_cached_ && std::chrono::steady_clock::now() < slew_force_until_) {
            return true;
        }
        // Check manual axis motion.
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (!slewing_cached_) {
            return false;
        }
        // Poll TheSkyX.
        try {
            auto& protocol = BisqueProtocolWrapper::instance();
            bool complete = protocol.is_slew_complete();
            if (complete) {
                slewing_cached_ = false;
            }
            return !complete;
        } catch (...) {
            return false;
        }
    }

    bool get_tracking_locked() const {
        try {
            auto& protocol = BisqueProtocolWrapper::instance();
            return protocol.is_tracking();
        } catch (...) {
            return false;
        }
    }

    void wait_for_slew_complete_locked(std::unique_lock<std::mutex>& lock) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > 120) {
                throw AlpacaException("Slew timed out after 120 seconds");
            }
            try {
                auto& protocol = BisqueProtocolWrapper::instance();
                if (protocol.is_slew_complete()) {
                    slewing_cached_ = false;
                    if (slew_settle_time_seconds_ > 0) {
                        lock.unlock();
                        std::this_thread::sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
                        lock.lock();
                    }
                    return;
                }
            } catch (...) {
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            lock.lock();
        }
    }

    int device_number_;
    ConnectionInfo connection_info_;
    mutable std::mutex mutex_;
    bool connected_ = false;

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    mutable bool target_set_ = false;

    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;

    mutable double cached_ra_hours_ = 0.0;
    mutable double cached_dec_degrees_ = 0.0;
    mutable double cached_alt_degrees_ = 0.0;
    mutable double cached_az_degrees_ = 0.0;
    mutable bool equatorial_cache_valid_ = false;
    mutable bool altaz_cache_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_equatorial_update_;
    mutable std::chrono::steady_clock::time_point last_altaz_update_;

    double site_latitude_ = 0.0;
    double site_longitude_ = 0.0;
    double site_elevation_m_ = 0.0;
    bool site_info_valid_ = false;

    mutable bool parked_ = false;
    mutable bool at_home_ = false;
    mutable bool homing_ = false;
    mutable bool slewing_cached_ = false;
    mutable std::chrono::steady_clock::time_point slew_force_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    mutable bool pulse_guiding_ = false;

    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;

    GuideRate guide_rate_{};
};

std::unique_ptr<TelescopeDriver> create_bisque_telescope(
    int device_number,
    const ConnectionInfo& connection_info) {
    return create_bisque_telescope_with_site(device_number, connection_info,
                                             std::nullopt, std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_bisque_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m) {
    return std::make_unique<BisqueTelescopeDriver>(
        device_number, connection_info,
        site_latitude_deg, site_longitude_deg, site_elevation_m);
}

} // namespace alpacacore::vendor::bisque

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
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>

namespace alpacacore::vendor::skywatcher {

namespace {

constexpr double kHoursToDegrees = 15.0;
// Axis counts value that represents the home position (pointing at the pole,
// counterweight side down). The controller's position register is initialized
// to this offset so signed axis angles fit in the unsigned 24-bit counter.
constexpr uint32_t kHomeCounts = 0x800000;
constexpr uint32_t kCountsMask = 0xFFFFFF;
constexpr double kSiderealDegPerSec = 360.0 / 86164.0905;
constexpr double kDefaultGuideRateDegPerSec = 0.5 * kSiderealDegPerSec;
// ASCOM DriveRates: 0 = Sidereal, 1 = Lunar, 2 = Solar (3 = King unsupported).
// Standard drive rates (INDI TRACKRATE_* constants), arcsec/s over 3600.
constexpr double kLunarDegPerSec = 14.511415 / 3600.0;
// RightAscensionRate is in seconds of RA per SIDEREAL second (ASCOM):
// 1 s RA = 15 arcsec, scaled sidereal->SI by 86164.1/86400.
constexpr double kRaRateSecondsToDegPerSec = 15.0 / (0.9972695663 * 3600.0);
// Dec rates within this factor of the slow-mode floor are duty-cycled rather
// than run continuously (":I" already pinned at 0xFFFFFF cannot go slower).
constexpr double kSlowModeFloorPad = 1.02;
constexpr double kSolarDegPerSec = 15.0 / 3600.0;
// ~800x sidereal, the classic Sky-Watcher maximum slew rate.
constexpr double kMaxMoveAxisRateDegPerSec = 800.0 * kSiderealDegPerSec;
// Above ~128x sidereal the controller needs high-speed (fast) mode.
constexpr double kFastModeThresholdDegPerSec = 128.0 * kSiderealDegPerSec;
constexpr auto kPositionCacheTtl = std::chrono::seconds(2);
// Longest a comms fault may serve last-known position/slewing state.
constexpr auto kStaleCacheLimit = std::chrono::seconds(10);
constexpr auto kPulseGuideCompletionDelay = std::chrono::milliseconds(1000);
constexpr auto kAxisStopTimeout = std::chrono::seconds(5);
// Below this rate an in-place ":I" pulse adjustment is unreliable (and a
// non-positive rate needs a direction change ":I" cannot deliver).
constexpr double kMinInPlacePulseRateDegPerSec = 0.05 * kSiderealDegPerSec;
// AutoHome (home index sensor) constants — SynScan/EQMod ":q"/":W" extended
// commands. Indexer reads: 0 = armed below the index, 0xFFFFFF = armed above,
// anything else = the count at which the sensor edge latched.
constexpr uint32_t kFeatureInquiry = 0x000001;
constexpr uint32_t kIndexerInquiry = 0x000000;
constexpr uint32_t kIndexerReset = 0x000008;
constexpr uint32_t kIndexerAbove = 0xFFFFFF;
// Hunt speeds (EQMod uses 800x for the coarse pass, 400x for the detect pass).
constexpr double kAutoHomeCoarseRateDegPerSec = 800.0 * kSiderealDegPerSec;
constexpr double kAutoHomeDetectRateDegPerSec = 400.0 * kSiderealDegPerSec;

double wrap_degrees(double deg) {
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

double wrap_hours(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }
    return wrapped;
}

// Wrap an hour angle into [-12, +12).
double wrap_hour_angle(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < -12.0) {
        wrapped += 24.0;
    }
    if (wrapped >= 12.0) {
        wrapped -= 24.0;
    }
    return wrapped;
}

double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time, double longitude_degrees) {
    using namespace std::chrono;
    // Sub-second resolution matters: whole-second truncation stepped LST (and
    // so RA) in 15 arcsec jumps, +-0.1 RA-s/s of jitter over a 10 s window.
    double days_since_epoch = duration<double>(utc_time.time_since_epoch()).count() / 86400.0;
    double jd = 2440587.5 + days_since_epoch;
    double t = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - (t * t * t) / 38710000.0;
    double lst = gmst + longitude_degrees;
    lst = std::fmod(lst, 360.0);
    if (lst < 0.0) {
        lst += 360.0;
    }
    return lst / kHoursToDegrees;
}

// Signed axis angle in degrees from the raw 24-bit counter.
double counts_to_degrees(uint32_t counts, uint32_t cpr) {
    int32_t delta = static_cast<int32_t>(counts) - static_cast<int32_t>(kHomeCounts);
    return static_cast<double>(delta) * 360.0 / static_cast<double>(cpr);
}

uint32_t degrees_to_counts(double degrees, uint32_t cpr) {
    int32_t delta = static_cast<int32_t>(std::lround(degrees / 360.0 * static_cast<double>(cpr)));
    return (kHomeCounts + static_cast<uint32_t>(delta)) & kCountsMask;
}

}  // namespace

class SkyWatcherTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    SkyWatcherTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                              std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                              std::optional<double> site_elevation_m)
        : AsyncConnectable("SkyWatcher"),
          device_number_(device_number),
          connection_info_(connection_info),
          site_latitude_(site_latitude_deg.value_or(0.0)),
          site_longitude_(site_longitude_deg.value_or(0.0)),
          site_elevation_m_(site_elevation_m.value_or(0.0)) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~SkyWatcherTelescopeDriver() override {
        // Base contract: block/join the connection task FIRST, then cancel and
        // join the slew/pulse task threads, all before members are destroyed.
        shutdown_connection();
        cancel_async_tasks();
        if (connected_) {
            try {
                // Qualified: virtual dispatch is already gone in a destructor.
                SkyWatcherTelescopeDriver::set_connected(false);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Destructor: nothing useful to do with a disconnect failure.
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Sky-Watcher Wave Mount"; }

    DeviceType get_device_type() const override { return DeviceType::Telescope; }

    std::string get_unique_id() const override { return "SkyWatcher_" + std::to_string(device_number_); }

    std::string get_description() const override { return "Sky-Watcher Motor Controller Mount Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore SkyWatcher Driver v0.1"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Motor board firmware captured at connect; surfaced in the web UI only,
    // under its own narrow mutex so a configureddevices poll never blocks on
    // the coarse mutex_ held across the multi-second connect.
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
        if (!connected) {
            // Join background task threads BEFORE taking mutex_ (they take it).
            cancel_async_tasks();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }

        auto& protocol = SkyWatcherProtocolWrapper::instance();
        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to Sky-Watcher motor controller");
            }
            connected_ = true;
            reset_runtime_state_locked();

            try {
                std::string version = protocol.get_motor_board_version();
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = version;
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // TODO: Confirm ":e" reliability on Wave 100i over both transports.
            }

            axis_params_[0] = protocol.get_axis_parameters(kAxisRa);
            axis_params_[1] = protocol.get_axis_parameters(kAxisDec);

            // Feature inquiry (":q" data 0x000001): bit 0x04 = home index
            // sensor. Wave 100i reports it on both axes (0x100C); older
            // boards reject ":q" entirely, so failure just disables AutoHome.
            has_home_indexer_ = false;
            try {
                uint32_t ra_features = protocol.get_feature(kAxisRa, kFeatureInquiry);
                uint32_t dec_features = protocol.get_feature(kAxisDec, kFeatureInquiry);
                has_home_indexer_ = (ra_features & 0x04) && (dec_features & 0x04);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // ":q" unsupported on this motor board.
            }

            // First power-up: position registers default to the home offset but
            // the controller reports "not initialized" and rejects motion until
            // ":F" is sent. Only stamp the home position when uninitialized so a
            // reconnect never clobbers an aligned session.
            for (int axis = kAxisRa; axis <= kAxisDec; ++axis) {
                AxisStatus status = protocol.inquire_status(axis);
                if (!status.init_done) {
                    protocol.set_position(axis, kHomeCounts);
                    protocol.initialization_done(axis);
                }
            }

            // Warm the position cache so first property reads stay inside
            // ConformU fast-response targets.
            try {
                refresh_position_cache_locked(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Cache warm-up only; reads will retry on demand.
            }
        } else {
            // Best effort: never leave an axis moving on disconnect.
            try {
                protocol.stop_motion(kAxisRa);
                protocol.stop_motion(kAxisDec);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Best effort; status polling still reports the true state.
            }
            protocol.disconnect();
            connected_ = false;
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_.clear();
            }
            reset_runtime_state_locked();
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
        SkyWatcherProtocolWrapper::instance().send_raw_command(std::string(command) + "\r");
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        std::string reply = SkyWatcherProtocolWrapper::instance().send_raw_command(std::string(command) + "\r");
        return !reply.empty() && reply[0] == '=';
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return SkyWatcherProtocolWrapper::instance().send_raw_command(std::string(command) + "\r");
    }

    AlignmentMode get_alignment_mode() const override {
        // The Wave is a strain-wave EQ mount; it points and flips like a GEM
        // (two pier sides), so GermanPolar matches client expectations.
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        auto [alt, az] = compute_alt_az_locked();
        (void)az;
        return alt;
    }

    double get_aperture_diameter() const override { return aperture_diameter_m_; }

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

    double get_aperture_area() const override { return aperture_area_m2_; }

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
        refresh_position_cache_locked(false);
        auto [alt, az] = compute_alt_az_locked();
        (void)alt;
        return az;
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

    bool get_can_set_declination_rate() const override { return true; }
    bool get_can_set_guide_rates() const override { return true; }
    bool get_can_set_park() const override { return true; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return true; }
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
        refresh_position_cache_locked(false);
        auto [ra, dec] = compute_ra_dec_locked();
        (void)ra;
        return std::clamp(dec, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return dec_rate_arcsec_per_sec_;
    }

    // DeclinationRate (arcsec/s): while tracking, the Dec axis runs a
    // speed-mode motion at the offset rate; rates below the slow-mode floor
    // (~0.26 arcsec/s, ":I" clamps at 0xFFFFFF) are produced by duty-cycling.
    // Issue #214 re-attempt: the previously-suspected hardware anomalies were
    // bench-disproven (axis tracks 5-320 as/s within 0.2%; no ":I" read
    // glitches) — both were artifacts of the pre-#216 refinement-goto races.
    void set_declination_rate(double rate) override {
        reap_dec_duty_task();
        bool need_duty = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            if (tracking_rate_ != 0) {
                throw AlpacaException("DeclinationRate can only be set at the Sidereal drive rate",
                                      AlpacaError::InvalidOperation);
            }
            dec_rate_arcsec_per_sec_ = rate;
            refresh_position_cache_locked(true);  // fresh model anchor
            if (tracking_) {
                apply_dec_rate_offset_locked(lock);
            }
            need_duty = dec_duty_rate_deg_s_ != 0.0;
        }
        if (need_duty) {
            start_dec_duty_thread();
        }
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return tracking_;
    }

    void set_tracking(bool tracking) override {
        bool need_duty = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            if (tracking) {
                check_not_parked_locked("Tracking");
            }
            set_tracking_locked(lock, tracking);
            need_duty = tracking_ && dec_duty_rate_deg_s_ != 0.0;
        }
        if (need_duty) {
            start_dec_duty_thread();
        }
    }

    // (Re)start the duty-cycle worker for a sub-floor DeclinationRate. Call
    // with no mutexes held. The whole reap+create sequence is serialized by
    // duty_lifecycle_mutex_ so two concurrent setters can never reassign a
    // still-joinable std::thread (std::terminate). The join itself must NOT
    // happen under task_mutex_ — the worker's task_wait_for reacquires it on
    // wake, so joining while holding it deadlocks; the lifecycle mutex is
    // never taken by the worker, only by setters.
    void start_dec_duty_thread() {
        std::lock_guard<std::mutex> lifecycle(duty_lifecycle_mutex_);
        reap_dec_duty_locked_lifecycle();
        std::lock_guard<std::mutex> tlock(task_mutex_);
        dec_duty_thread_ = std::thread([this]() { dec_duty_loop(); });
    }

    double get_focal_length() const override { return focal_length_m_; }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override { return guide_rate_; }

    void set_guide_rate(const GuideRate& rate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double ra_fraction = rate.ra / kSiderealDegPerSec;
        double dec_fraction = rate.dec / kSiderealDegPerSec;
        if (ra_fraction < 0.0 || ra_fraction > 1.0 || dec_fraction < 0.0 || dec_fraction > 1.0) {
            throw AlpacaException("Guide rate must be between 0 and 1x sidereal", AlpacaError::InvalidValue);
        }
        guide_rate_ = rate;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        auto [ra, dec] = compute_ra_dec_locked();
        (void)dec;
        return ra;
    }

    double get_right_ascension_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ra_rate_sec_per_sidereal_sec_;
    }

    // RightAscensionRate (seconds of RA per sidereal second): folded into the
    // RA drive rate. Positive rate = RA increasing = axis advancing SLOWER
    // (RA = LST - HA), so the offset is SUBTRACTED; a large offset reverses
    // the axis, which needs a stop-and-restart (":I" cannot change direction).
    void set_right_ascension_rate(double rate) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        if (tracking_rate_ != 0) {
            throw AlpacaException("RightAscensionRate can only be set at the Sidereal drive rate",
                                  AlpacaError::InvalidOperation);
        }
        double previous = effective_ra_rate_locked();
        ra_rate_sec_per_sidereal_sec_ = rate;
        refresh_position_cache_locked(true);  // fresh model anchor
        if (tracking_) {
            apply_ra_tracking_rate_locked(lock, previous);
        }
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        // ASCOM convention derived from the dec-axis branch: the branch chosen
        // for HA >= 0 targets is pierEast (0), the mirror branch pierWest (1).
        return cached_dec_axis_deg_ >= 0.0 ? 0 : 1;
    }

    void set_side_of_pier(int side) override {
        (void)side;
        throw AlpacaException("Pier side not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        (void)dec;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double lst = compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_);
        double ha = wrap_hour_angle(lst - ra);
        return ha >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        // Coordinates are derived from local sidereal time — topocentric
        // apparent (JNow), not J2000.
        return EquatorialSystem::Topocentric;
    }

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
        return compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_);
    }

    double get_site_elevation() const override { return site_elevation_m_; }

    void set_site_elevation(double elevation) override {
        if (elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters", AlpacaError::InvalidValue);
        }
        site_elevation_m_ = elevation;
    }

    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return site_latitude_;
    }

    void set_site_latitude(double latitude) override {
        if (latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        site_latitude_ = latitude;
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return site_longitude_;
    }

    void set_site_longitude(double longitude) override {
        if (longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        site_longitude_ = longitude;
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

    int get_tracking_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracking_rate_;
    }

    void set_tracking_rate(int rate) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        if (rate < 0 || rate > 2) {
            throw AlpacaException("Unsupported tracking rate", AlpacaError::InvalidValue);
        }
        double previous = effective_ra_rate_locked();
        tracking_rate_ = rate;
        // ASCOM contract: changing the drive rate zeroes the rate offsets.
        ra_rate_sec_per_sidereal_sec_ = 0.0;
        dec_rate_arcsec_per_sec_ = 0.0;
        dec_duty_rate_deg_s_ = 0.0;
        if (tracking_) {
            apply_ra_tracking_rate_locked(lock, previous);
            apply_dec_rate_offset_locked(lock);
        }
    }

    std::vector<int> get_tracking_rates() const override { return {0, 1, 2}; }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // The motor controller has no clock; the host clock is authoritative.
        return std::chrono::system_clock::now() + utc_offset_;
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        utc_offset_ = utc - std::chrono::system_clock::now();
    }

    // FindHome is an asynchronous initiator (ITelescopeV4). The Wave has no
    // home sensor, but the controller's power-on index (counts 0x800000,
    // counterweight down pointing at the pole) IS the home position, so homing
    // is a goto to axis angles 0,0. AtHome flips true (and Slewing false) in
    // the same locked step when the goto lands.
    void find_home() override {
        reap_slew_task();
        reap_pulse_task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("FindHome");
            if (homing_) {
                return;  // already homing
            }
            if (at_home_ && !get_hardware_slewing_locked()) {
                return;  // already at home
            }
            invalidate_position_cache_locked();
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            restore_tracking_after_slew_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            homing_ = true;
        }

        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (slew_task_thread_.joinable()) {
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            slew_task_thread_.join();
            slew_task_cancel_.store(false);
        }
        slew_task_thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                homing_ = false;
                return;
            }
            try {
                if (has_home_indexer_) {
                    // True AutoHome: hunt the home index sensors and re-anchor
                    // the count frame to the physical home mark.
                    run_autohome(lock);
                    set_tracking_locked(lock, false);
                    refresh_position_cache_locked(true);
                    at_home_ = true;
                } else {
                    // No sensors: goto the power-on index (counts kHomeCounts).
                    dispatch_goto_locked(lock, 0.0, 0.0);
                    wait_for_slew_complete(lock);
                    set_tracking_locked(lock, false);
                    // Verify we actually landed at home (an AbortSlew mid-home
                    // stops the axes wherever they are) before claiming AtHome.
                    refresh_position_cache_locked(true);
                    at_home_ = std::abs(cached_ra_axis_deg_) < 0.1 && std::abs(cached_dec_axis_deg_) < 0.1;
                }
                homing_ = false;
            } catch (const std::exception& ex) {
                homing_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", std::string("FindHome failed: ") + ex.what());
            } catch (...) {
                homing_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", "FindHome failed with unknown exception");
            }
        });
    }

    // Park is an asynchronous initiator (ITelescopeV4): dispatch the park slew
    // in the background and return inside the STANDARD response target; AtPark
    // turns true when the slew completes and tracking is stopped.
    void park() override {
        reap_slew_task();
        reap_pulse_task();
        double target_ra_axis = 0.0;
        double target_dec_axis = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // calling Park twice is harmless
            }
            if (!park_position_set_) {
                // Default park = the mount's home position (pole, weights down).
                park_ra_axis_deg_ = 0.0;
                park_dec_axis_deg_ = 0.0;
                park_position_set_ = true;
            }
            target_ra_axis = park_ra_axis_deg_;
            target_dec_axis = park_dec_axis_deg_;
            invalidate_position_cache_locked();
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            restore_tracking_after_slew_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parking_ = true;
        }

        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (slew_task_thread_.joinable()) {
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            slew_task_thread_.join();
            slew_task_cancel_.store(false);
        }
        slew_task_thread_ = std::thread([this, target_ra_axis, target_dec_axis]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                parking_ = false;
                return;
            }
            try {
                dispatch_goto_locked(lock, target_ra_axis, target_dec_axis);
                wait_for_slew_complete(lock);
                set_tracking_locked(lock, false);
                // AtPark and Slewing flip in the same locked step: no window
                // where a poller can see Slewing false with AtPark false.
                parked_ = true;
                parking_ = false;
                at_home_ = target_ra_axis == 0.0 && target_dec_axis == 0.0;
            } catch (const std::exception& ex) {
                parking_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", std::string("Park failed: ") + ex.what());
            } catch (...) {
                parking_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", "Park failed with unknown exception");
            }
        });
    }

    void pulse_guide(int direction, int duration) override {
        int axis = -1;
        bool ra_rate_adjust = false;
        bool ra_pulse_restart = false;  // pulse rate <= 0: axis must reverse
        double dec_rate_deg_per_sec = 0.0;
        double ra_pulse_rate_deg_per_sec = 0.0;
        double ra_restore_rate_deg_per_sec = kSiderealDegPerSec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            if (duration < 0) {
                throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
            }
            if (direction < 0 || direction > 3) {
                throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
            }

            const auto now = std::chrono::steady_clock::now();

            // Reads are live (dead-reckoned) during pulses — no frozen-target
            // accumulation, and pulses never rewrite the slew Target
            // properties. The pier-side sign for Dec needs a fresh position.
            refresh_position_cache_locked(false);

            if (direction == 0 || direction == 1) {
                // North/South: DEC axis speed-mode nudge. Freeze the RA value —
                // the RA axis keeps tracking, so only DEC accumulates.
                axis = kAxisDec;
                dec_rate_deg_per_sec = direction == 0 ? guide_rate_.dec : -guide_rate_.dec;
                // dec = 90 - a2 on the east-pointing branch (a2 >= 0): guide
                // North (+Dec) is NEGATIVE axis motion there (same sign rule
                // as the DeclinationRate offset, confirmed by ConformU 4.5
                // measured-rate tests).
                if (cached_dec_axis_deg_ >= 0.0) {
                    dec_rate_deg_per_sec = -dec_rate_deg_per_sec;
                }
            } else {
                // East/West: adjust the RA tracking rate for the pulse window
                // (slow speed mode allows a live step-period change). East
                // slows apparent RA drive, west speeds it.
                axis = kAxisRa;
                ra_rate_adjust = tracking_;
                double adjust = direction == 2 ? -guide_rate_.ra : guide_rate_.ra;
                ra_restore_rate_deg_per_sec = effective_ra_rate_locked();
                ra_pulse_rate_deg_per_sec = ra_restore_rate_deg_per_sec + adjust;
                // In-place ":I" cannot change direction. A non-positive pulse
                // rate (guide rate near 1x sidereal under Lunar/Solar drive)
                // needs a full stop-and-reverse, and a full restart to resume.
                ra_pulse_restart = ra_pulse_rate_deg_per_sec <= kMinInPlacePulseRateDegPerSec;
            }

            // No read freeze during the pulse: ConformU 4.5 measures the
            // physical pulse displacement (Dec moved, RA unchanged), and the
            // dead-reckoned live reads report exactly that.

            pulse_guiding_active_ = true;
            pulse_guide_end_time_ = now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay;
            invalidate_position_cache_locked();
        }

        // Stop-the-pulse timer thread — joinable member thread, never detached.
        reap_pulse_task();
        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (pulse_task_thread_.joinable()) {
            pulse_task_cancel_.store(true);
            task_cv_.notify_all();
            pulse_task_thread_.join();
            pulse_task_cancel_.store(false);
        }
        const bool restore_tracking = ra_rate_adjust;
        const double dec_rate = dec_rate_deg_per_sec;
        const double ra_pulse_rate = ra_pulse_rate_deg_per_sec;
        const bool pulse_restart = ra_pulse_restart;
        pulse_task_thread_ = std::thread([this, axis, duration, restore_tracking, direction, dec_rate, ra_pulse_rate,
                                          ra_restore_rate_deg_per_sec, pulse_restart]() {
            // PulseGuide is an asynchronous initiator (ITelescopeV4): the axis
            // dispatch (which can stop-and-wait a ramping axis, plus UDP
            // retries) runs here so pulse_guide() returns inside the STANDARD
            // response target. IsPulseGuiding is already true.
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                auto& proto = SkyWatcherProtocolWrapper::instance();
                if (axis == kAxisDec) {
                    start_speed_motion_locked(lock, kAxisDec, dec_rate);
                } else if (restore_tracking && !pulse_restart) {
                    proto.set_step_period(kAxisRa, tracking_step_period_for(ra_pulse_rate));
                    cmd_axis_rate_deg_s_[0] = ra_pulse_rate;
                } else if (restore_tracking) {
                    // Pulse rate is non-positive (direction reversal): a live
                    // ":I" write cannot reverse the axis — stop and restart in
                    // the pulse direction instead.
                    start_speed_motion_locked(lock, kAxisRa, ra_pulse_rate);
                } else {
                    // Not tracking: nudge the RA axis directly like DEC.
                    double rate = direction == 2 ? -guide_rate_.ra : guide_rate_.ra;
                    start_speed_motion_locked(lock, kAxisRa, rate);
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SkyWatcher", std::string("PulseGuide dispatch failed: ") + e.what());
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_active_ = false;
                return;
            } catch (...) {
                ALPACA_LOG_WARN("SkyWatcher", "PulseGuide dispatch failed with unknown exception");
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_active_ = false;
                return;
            }
            auto stop_axis = [this, axis, restore_tracking, ra_restore_rate_deg_per_sec, pulse_restart]() {
                auto& proto = SkyWatcherProtocolWrapper::instance();
                if (restore_tracking && !pulse_restart) {
                    // RA pulse over a live tracking axis: restore the drive
                    // step period; the axis never stopped.
                    proto.set_step_period(kAxisRa, tracking_step_period_for(ra_restore_rate_deg_per_sec));
                    std::lock_guard<std::mutex> lock(mutex_);
                    cmd_axis_rate_deg_s_[0] = ra_restore_rate_deg_per_sec;
                } else if (restore_tracking) {
                    // Reversed pulse: full stop-and-restart back to the drive rate.
                    std::unique_lock<std::mutex> lock(mutex_);
                    start_speed_motion_locked(lock, kAxisRa, ra_restore_rate_deg_per_sec);
                } else {
                    proto.stop_motion(axis);
                    // Zero the dead-reckoning rate: this direct stop bypasses
                    // stop_axis_and_wait_locked, and a stale rate kept the
                    // reads drifting after the pulse ended (ConformU 4.5 pulse
                    // displacement tests read phantom motion).
                    std::unique_lock<std::mutex> lock(mutex_);
                    cmd_axis_rate_deg_s_[axis - 1] = 0.0;
                    invalidate_position_cache_locked();
                    // A Dec pulse pre-empted any DeclinationRate offset
                    // motion: re-apply it so guiding corrections don't
                    // silently cancel comet/satellite tracking.
                    if (axis == kAxisDec && tracking_ && dec_rate_arcsec_per_sec_ != 0.0) {
                        try {
                            apply_dec_rate_offset_locked(lock);
                        } catch (const std::exception& e) {
                            ALPACA_LOG_WARN("SkyWatcher",
                                            std::string("Pulse end: failed to restore Dec rate offset: ") + e.what());
                        }
                    }
                }
            };
            if (!task_wait_for(std::chrono::milliseconds(duration), pulse_task_cancel_)) {
                // Cancelled by a reaper (a new pulse/slew/park/home/moveaxis/
                // abort/disconnect). DO NOT touch the hardware here: the
                // reaper stops or re-commands the axes itself, and a stop or
                // step-period restore landing mid-goto corrupted the next
                // operation (ConformU: "declination axis did not move" on the
                // first pulse after a slew).
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_active_ = false;
                return;
            }
            // The stop/restore MUST land or the axis runs away at guide rate.
            constexpr int kStopAttempts = 3;
            bool stopped = false;
            std::string last_error;
            for (int attempt = 0; attempt < kStopAttempts && !stopped; ++attempt) {
                try {
                    stop_axis();
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
            if (!stopped) {
                ALPACA_LOG_ERROR("SkyWatcher", "PulseGuide STOP FAILED after " + std::to_string(kStopAttempts) +
                                                   " attempts on axis " + std::to_string(axis) +
                                                   " — axis may still be moving (mount runaway risk): " + last_error);
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_active_ = false;
                if (connected_) {
                    manual_axis_slewing_[axis - 1] = true;
                }
            }
        });
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(true);
        park_ra_axis_deg_ = cached_ra_axis_deg_;
        park_dec_axis_deg_ = cached_dec_axis_deg_;
        park_position_set_ = true;
    }

    void slew_to_coordinates(double ra, double dec) override {
        reap_slew_task();  // also clears a leftover AbortSlew cancellation
        reap_pulse_task();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinates");
        validate_ra_dec(ra, dec, "SlewToCoordinates");
        goto_in_progress_ = true;
        try {
            do_slew_to_ra_dec_locked(lock, ra, dec);
            wait_for_slew_complete(lock);
            refine_goto_landing(lock, ra, dec);
        } catch (...) {
            goto_in_progress_ = false;
            throw;
        }
        goto_in_progress_ = false;
        restore_tracking_after_slew_locked(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        // Cancel + join any previous slew or pulse task first (without mutex_):
        // a stale pulse timer firing mid-goto corrupts the slew.
        reap_slew_task();
        reap_pulse_task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            invalidate_position_cache_locked();
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_set_ = true;
            restore_tracking_after_slew_ = tracking_;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
        }

        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (slew_task_thread_.joinable()) {
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            slew_task_thread_.join();
            slew_task_cancel_.store(false);
        }
        slew_task_thread_ = std::thread([this, ra, dec]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                return;
            }
            goto_in_progress_ = true;
            try {
                dispatch_predicted_goto_locked(lock, ra, dec);
                // Poll for completion so tracking restarts after the goto —
                // releasing the mutex between polls so GETs stay responsive.
                wait_for_slew_complete(lock);
                refine_goto_landing(lock, ra, dec);
                goto_in_progress_ = false;
                restore_tracking_after_slew_locked(lock);
            } catch (const std::exception& ex) {
                goto_in_progress_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", std::string("Async slew failed: ") + ex.what());
            } catch (...) {
                goto_in_progress_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", "Async slew failed with unknown exception");
            }
        });
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
        reap_pulse_task();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        validate_ra_dec(ra, dec, "SyncToCoordinates");

        auto& protocol = SkyWatcherProtocolWrapper::instance();

        // ":E" requires the motors fully stopped — pause tracking around the
        // position write, then resume. This is the mount's native sync (the
        // controller's own position register moves), never a driver offset.
        //
        // ORDER MATTERS: the axis frame must be computed AFTER the axis has
        // stopped, aimed at the moment tracking RESUMES — the sky keeps
        // moving while the counts are frozen, and a frame computed before the
        // stop is stale by the whole pause (stop ramp + writes + restart,
        // ~1-3 s = 15-45 arcsec of RA; seen by ConformU as a constant
        // ~79 arcsec return error together with the old post-sync freeze).
        const bool was_tracking = tracking_;
        if (was_tracking) {
            const uint64_t gen = ++motion_generation_;
            if (!stop_axis_and_wait_locked(lock, kAxisRa, gen)) {
                throw AlpacaException("Sync superseded by a concurrent motion command");
            }
        }
        // Aim the frame at the expected restart moment (two ":E" writes plus
        // the tracking start sequence).
        constexpr double kSyncRestartLatencySeconds = 0.25;
        auto [axis1, axis2] = ra_dec_to_axis_degrees_locked(
            ra, dec, was_tracking ? kSyncRestartLatencySeconds * kLstHoursPerSecond : 0.0);
        protocol.set_position(kAxisRa, degrees_to_counts(axis1, axis_params_[0].counts_per_revolution));
        protocol.set_position(kAxisDec, degrees_to_counts(axis2, axis_params_[1].counts_per_revolution));
        if (was_tracking) {
            set_tracking_locked(lock, true);
        }

        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        invalidate_position_cache_locked();
        // No post-sync read freeze: live reads land on the synced frame.
    }

    void sync_to_target() override {
        if (!target_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        sync_to_coordinates(target_ra_hours_, target_dec_degrees_);
    }

    void unpark() override {
        // Cancel an in-flight park first (without mutex_ -- the park task
        // takes it): Unpark during a park must win, not race the task's
        // parked_ = true assignment.
        bool was_parking = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            was_parking = parking_;
        }
        if (was_parking) {
            reap_slew_task();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (was_parking) {
            // The park slew may still be moving the axes; stop them.
            try {
                auto& protocol = SkyWatcherProtocolWrapper::instance();
                protocol.stop_motion(kAxisRa);
                protocol.stop_motion(kAxisDec);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Best effort; status polling still reports the true state.
            }
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
        }
        parking_ = false;
        parked_ = false;
    }

    bool get_can_move_axis(int axis) const override { return axis == 0 || axis == 1; }

    void move_axis(int axis, double rate) override {
        reap_pulse_task();
        // Join any previous stop-completion task first, WITHOUT mutex_ held
        // (the task takes mutex_).
        reap_stop_task();
        bool need_stop_task = false;
        uint64_t stop_task_generation = 0;
        int channel = kAxisRa;
        {
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

            channel = axis == 0 ? kAxisRa : kAxisDec;
            constexpr double kStopEpsilon = 1e-9;
            const bool moving = std::abs(rate) > kStopEpsilon;
            if (moving) {
                parked_ = false;
                at_home_ = false;
                // Command the motion BEFORE publishing the Slewing flag: if the
                // transport throws (seen as UDP timeouts over a flaky Wi-Fi
                // link), a pre-set flag is never cleared and Slewing wedges
                // true forever (ConformU Wi-Fi finding).
                start_speed_motion_locked(lock, channel, rate);
                manual_axis_slewing_[axis] = true;
            } else if (manual_axis_slewing_[axis]) {
                // MoveAxis(axis, 0) on a moving axis is an asynchronous
                // initiator: issue the stop and return inside the STANDARD
                // response target. A background task keeps Slewing true until
                // the axis reports fully stopped (the deceleration ramp can
                // exceed 1 s), then restores the previous tracking state per
                // ASCOM.
                SkyWatcherProtocolWrapper::instance().stop_motion(channel);
                cmd_axis_rate_deg_s_[axis] = 0.0;
                // The stop is a motion command: bump and capture the
                // generation so the background restore-tracking task can tell
                // whether a newer motion command took over while it polled
                // (PR #216 round-5 finding — the restore otherwise re-starts
                // tracking a concurrent SetTracking(false) just stopped).
                stop_task_generation = ++motion_generation_;
                need_stop_task = true;
            }
            // MoveAxis(axis, 0) on an axis with no manual motion is a no-op:
            // Slewing must read false immediately, and tracking (if running on
            // the RA axis) continues untouched per the ASCOM restore-tracking
            // semantics. Raising the flag here and clearing it via the status
            // poll failed ConformU over Wi-Fi, where the poll round-trip
            // outlasted the checker's window.
            invalidate_position_cache_locked();
        }
        if (!need_stop_task) {
            return;
        }

        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (stop_task_thread_.joinable()) {
            stop_task_cancel_.store(true);
            task_cv_.notify_all();
            stop_task_thread_.join();
            stop_task_cancel_.store(false);
        }
        stop_task_thread_ = std::thread([this, channel, axis, stop_task_generation]() {
            auto& protocol = SkyWatcherProtocolWrapper::instance();
            auto deadline = std::chrono::steady_clock::now() + kAxisStopTimeout;
            bool stopped = false;
            while (std::chrono::steady_clock::now() < deadline) {
                try {
                    if (!protocol.inquire_status(channel).running) {
                        stopped = true;
                        break;
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Transient poll failure; keep trying until the deadline.
                }
                if (!task_wait_for(std::chrono::milliseconds(50), stop_task_cancel_)) {
                    return;  // cancelled by disconnect/destruction
                }
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || stop_task_cancel_.load()) {
                return;
            }
            manual_axis_slewing_[axis] = false;
            if (!stopped) {
                ALPACA_LOG_WARN("SkyWatcher", "MoveAxis stop: axis " + std::to_string(channel) +
                                                  " still reported running at timeout");
            }
            // ASCOM: MoveAxis(axis, 0) restores the previous tracking state —
            // but only if no newer motion command superseded this stop while
            // the task polled (generation guard, same as every other path).
            if (channel == kAxisRa && tracking_ && motion_generation_ == stop_task_generation) {
                try {
                    set_tracking_locked(lock, true);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("SkyWatcher",
                                    std::string("MoveAxis stop: failed to restore tracking: ") + e.what());
                }
            } else if (channel == kAxisDec && tracking_ && dec_rate_arcsec_per_sec_ != 0.0 &&
                       motion_generation_ == stop_task_generation) {
                // Same restore contract for Dec: a manual nudge must not
                // silently cancel an active DeclinationRate offset.
                try {
                    apply_dec_rate_offset_locked(lock);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("SkyWatcher",
                                    std::string("MoveAxis stop: failed to restore Dec rate offset: ") + e.what());
                }
            }
        });
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            // Tertiary axis unsupported: empty range set per ASCOM semantics.
            return {};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        reap_pulse_task();
        // Cancel an in-flight async slew/refinement (the task joins later via
        // reap; the flag makes its waits and the refine loop exit promptly —
        // without this, the refinement re-slews after the abort's stop).
        slew_task_cancel_.store(true);
        task_cv_.notify_all();
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("AbortSlew");
        // AbortSlew is itself a motion command: bump the generation so any
        // stop-wait sleeping in an unlock window observes the supersession
        // and its dispatch aborts BEFORE sending new motor commands (the
        // cancel flag alone only catches it after the re-dispatch).
        ++motion_generation_;
        auto& protocol = SkyWatcherProtocolWrapper::instance();
        // Instant stop (":L") rather than the ramped ":K": AbortSlew's contract
        // is to stop NOW, and the ramp-down from an 800x slew otherwise leaves
        // the axes reporting a GOTO in progress for over a second, which the
        // next ConformU test observes as Slewing stuck true.
        protocol.instant_stop(kAxisRa);
        protocol.instant_stop(kAxisDec);
        cmd_axis_rate_deg_s_[0] = 0.0;
        cmd_axis_rate_deg_s_[1] = 0.0;
        goto_in_progress_ = false;
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        restore_tracking_after_slew_ = false;
        tracking_ = false;
        invalidate_position_cache_locked();
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
            throw AlpacaException("Not connected to Sky-Watcher mount", AlpacaError::NotConnected);
        }
    }

    void check_not_parked_locked(const char* operation) const {
        if (parked_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    static void validate_ra_dec(double ra, double dec, const char* context) {
        if (ra < 0.0 || ra >= 24.0) {
            throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
        }
        if (dec < -90.0 || dec > 90.0) {
            throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
        }
    }

    void reset_runtime_state_locked() {
        target_set_ = false;
        parked_ = false;
        at_home_ = false;
        tracking_ = false;
        restore_tracking_after_slew_ = false;
        pulse_guiding_active_ = false;
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parking_ = false;
        homing_ = false;
        goto_in_progress_ = false;
        tracking_rate_ = 0;
        ra_rate_sec_per_sidereal_sec_ = 0.0;
        dec_rate_arcsec_per_sec_ = 0.0;
        dec_duty_rate_deg_s_ = 0.0;
        dec_offset_running_ = false;
        cmd_axis_rate_deg_s_[0] = 0.0;
        cmd_axis_rate_deg_s_[1] = 0.0;
        position_cache_valid_ = false;
    }

    bool hemisphere_south_locked() const { return site_latitude_ < 0.0; }

    // ── Pointing model ──────────────────────────────────────────────────────
    // Home (counts == kHomeCounts on both axes): counterweight down, OTA at
    // the visible celestial pole. Axis angles are signed degrees from home.
    //   Branch A (dec axis angle >= 0): dec = 90 - a2, HA hours = a1 / 15.
    //   Branch B (dec axis angle <  0): dec = 90 + a2, HA hours = a1 / 15 - 12.
    // TODO: Validate physical rotation signs on Wave 100i hardware — the
    // positive-count direction of each axis relative to the sky is a wiring
    // convention this math assumes; flip kRaAxisSign/kDecAxisSign if slews
    // mirror. Southern hemisphere handling (dec mirrored, RA direction
    // reversed) is likewise unvalidated.

    std::pair<double, double> compute_ra_dec_locked() const {
        // Dead-reckon between hardware reads: while an axis runs at a
        // commanded speed rate, extrapolate the cached angle by rate x
        // elapsed. Raw counts quantize at ~0.31 arcsec and the cache is up to
        // kPositionCacheTtl stale -- reporting the commanded model keeps RA
        // steady under tracking (no LST-vs-stale-HA sawtooth) and resolves
        // sub-count offset rates. Goto/stop paths zero the commanded rates,
        // so a slewing or idle axis reports the raw cached angle.
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_position_update_).count();
        // Offsets hold the model anchor for the whole offset session; without
        // offsets the cache re-anchors within seconds, so clamp tight.
        dt = std::clamp(dt, 0.0, rate_offsets_active_locked() && tracking_ ? 3600.0 : 5.0);
        double a1 = cached_ra_axis_deg_ + cmd_axis_rate_deg_s_[0] * dt;
        double a2 = cached_dec_axis_deg_ + cmd_axis_rate_deg_s_[1] * dt;
        double dec = 0.0;
        double ha_hours = 0.0;
        if (a2 >= 0.0) {
            dec = 90.0 - a2;
            ha_hours = a1 / kHoursToDegrees;
        } else {
            dec = 90.0 + a2;
            ha_hours = a1 / kHoursToDegrees - 12.0;
        }
        if (hemisphere_south_locked()) {
            dec = -dec;
        }
        double lst = compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_);
        double ra = wrap_hours(lst - ha_hours);
        return {ra, std::clamp(dec, -90.0, 90.0)};
    }

    std::pair<double, double> ra_dec_to_axis_degrees_locked(double ra, double dec,
                                                            double lst_advance_hours = 0.0) const {
        double lst =
            compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_) + lst_advance_hours;
        double ha = wrap_hour_angle(lst - ra);
        double dec_mech = hemisphere_south_locked() ? -dec : dec;
        double a1 = 0.0;
        double a2 = 0.0;
        if (ha >= 0.0) {
            // pierEast branch
            a2 = 90.0 - dec_mech;
            a1 = ha * kHoursToDegrees;
        } else {
            // pierWest branch
            a2 = -(90.0 - dec_mech);
            a1 = (ha + 12.0) * kHoursToDegrees;
        }
        return {a1, a2};
    }

    std::pair<double, double> compute_alt_az_locked() const {
        auto [ra, dec] = compute_ra_dec_locked();
        double lst = compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_);
        double ha_rad = wrap_hour_angle(lst - ra) * kHoursToDegrees * std::numbers::pi / 180.0;
        double dec_rad = dec * std::numbers::pi / 180.0;
        double lat_rad = site_latitude_ * std::numbers::pi / 180.0;
        double sin_alt =
            std::sin(dec_rad) * std::sin(lat_rad) + std::cos(dec_rad) * std::cos(lat_rad) * std::cos(ha_rad);
        sin_alt = std::clamp(sin_alt, -1.0, 1.0);
        double alt_rad = std::asin(sin_alt);
        double cos_az =
            (std::sin(dec_rad) - std::sin(alt_rad) * std::sin(lat_rad)) / (std::cos(alt_rad) * std::cos(lat_rad));
        cos_az = std::clamp(cos_az, -1.0, 1.0);
        double az_deg = std::acos(cos_az) * 180.0 / std::numbers::pi;
        if (std::sin(ha_rad) > 0.0) {
            az_deg = 360.0 - az_deg;
        }
        return {alt_rad * 180.0 / std::numbers::pi, wrap_degrees(az_deg)};
    }

    // ── Position cache ──────────────────────────────────────────────────────

    void invalidate_position_cache_locked() const { position_cache_valid_ = false; }

    bool rate_offsets_active_locked() const {
        return ra_rate_sec_per_sidereal_sec_ != 0.0 || dec_rate_arcsec_per_sec_ != 0.0;
    }

    void refresh_position_cache_locked(bool force) const {
        auto now = std::chrono::steady_clock::now();
        if (!force && position_cache_valid_ && (now - last_position_update_) < kPositionCacheTtl) {
            return;
        }
        // While rate offsets run, serve the dead-reckoned model instead of
        // re-anchoring on hardware counts: duty-cycled Dec bursts and the
        // offset RA rate make raw reads jitter around the commanded average.
        // Offset entry points and motion commands force a fresh anchor, and
        // the hold is bounded: past kOffsetModelHold the model would pin at
        // the dt clamp and the reported position would silently freeze, so
        // fall through and take a fresh hardware anchor instead.
        constexpr auto kOffsetModelHold = std::chrono::minutes(30);
        if (!force && position_cache_valid_ && rate_offsets_active_locked() && tracking_ &&
            (now - last_position_update_) < kOffsetModelHold) {
            return;
        }
        auto& protocol = SkyWatcherProtocolWrapper::instance();
        try {
            uint32_t ra_counts = protocol.inquire_position(kAxisRa);
            uint32_t dec_counts = protocol.inquire_position(kAxisDec);
            cached_ra_axis_deg_ = counts_to_degrees(ra_counts, axis_params_[0].counts_per_revolution);
            cached_dec_axis_deg_ = counts_to_degrees(dec_counts, axis_params_[1].counts_per_revolution);
            position_cache_valid_ = true;
            last_position_update_ = now;
        } catch (...) {
            if (!position_cache_valid_) {
                throw;
            }
            // Serve last-known values only briefly: a sustained comms fault
            // must surface as an error, not as a frozen position.
            if ((now - last_position_update_) > kStaleCacheLimit) {
                position_cache_valid_ = false;
                throw;
            }
        }
    }

    // ── Motion primitives ───────────────────────────────────────────────────

    uint32_t step_period_for_locked(int channel, double rate_deg_per_sec, bool fast) const {
        const AxisParameters& params = axis_params_[channel - 1];
        double counts_per_sec = std::abs(rate_deg_per_sec) * params.counts_per_revolution / 360.0;
        if (counts_per_sec <= 0.0) {
            return kCountsMask;
        }
        double preset = static_cast<double>(params.timer_frequency) / counts_per_sec;
        if (fast) {
            preset *= static_cast<double>(params.high_speed_ratio);
        }
        preset = std::clamp(preset, 1.0, static_cast<double>(kCountsMask));
        return static_cast<uint32_t>(std::lround(preset));
    }

    uint32_t tracking_step_period_for(double rate_deg_per_sec) const {
        // Callers hold mutex_ or run from the pulse task after setup under it;
        // axis_params_ is immutable after connect.
        const AxisParameters& params = axis_params_[0];
        double counts_per_sec = std::abs(rate_deg_per_sec) * params.counts_per_revolution / 360.0;
        double preset = static_cast<double>(params.timer_frequency) / counts_per_sec;
        preset = std::clamp(preset, 1.0, static_cast<double>(kCountsMask));
        return static_cast<uint32_t>(std::lround(preset));
    }

    // Direction char for ":G": '0' = increasing counts, '1' = decreasing.
    // Positive axis rates (increasing axis angle) map to increasing counts.
    static char direction_char(double signed_rate) { return signed_rate >= 0.0 ? '0' : '1'; }

    // Stop one axis and poll until the controller reports it stationary,
    // RELEASING the mutex around every sleep (issue #212): the ramp-down can
    // take over a second, and holding mutex_ through it blocked every
    // concurrent Alpaca GET. The motion generation guards the unlock windows:
    // if another command claims the axes while we slept, the wait is no
    // longer ours to finish.
    // Returns true when the axis stopped and the caller's motion command is
    // still the current one; false when the wait was superseded (a newer
    // motion command bumped the generation — AbortSlew included — or the
    // slew task was cancelled) — the caller MUST NOT issue further motor
    // commands for its now-stale operation (PR #216 review: an AbortSlew
    // landing in the unlock window otherwise saw its goto re-dispatched).
    [[nodiscard]] bool stop_axis_and_wait_locked(std::unique_lock<std::mutex>& lock, int channel, uint64_t gen) {
        // Never emit a stop for a STALE generation (PR #216 round-6): when a
        // dispatch's first axis wait was superseded, a newer command may have
        // legitimately started motion on the second axis — a stale stop here
        // would silently kill it while its bookkeeping says it is running.
        // The newer generation owns the axes now, whatever their state.
        if (motion_generation_ != gen) {
            return false;
        }
        auto& protocol = SkyWatcherProtocolWrapper::instance();
        cmd_axis_rate_deg_s_[channel - 1] = 0.0;
        protocol.stop_motion(channel);
        auto deadline = std::chrono::steady_clock::now() + kAxisStopTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            // Generation only: slew_task_cancel_ stays stale-true between an
            // AbortSlew and the next reap, and must not poison unrelated
            // commands (ConformU: Tracking Write failed "Motion superseded").
            // AbortSlew bumps the generation, which is the supersession signal.
            if (motion_generation_ != gen) {
                return false;
            }
            AxisStatus status = protocol.inquire_status(channel);
            if (!status.running) {
                return true;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            lock.lock();
            check_connected();
        }
        throw AlpacaException("Timed out waiting for axis " + std::to_string(channel) + " to stop");
    }

    // Start speed-mode motion at a signed rate on one axis (assumes the caller
    // wants the axis re-commanded from stopped).
    void start_speed_motion_locked(std::unique_lock<std::mutex>& lock, int channel, double signed_rate_deg_per_sec) {
        auto& protocol = SkyWatcherProtocolWrapper::instance();
        const uint64_t gen = ++motion_generation_;
        if (!stop_axis_and_wait_locked(lock, channel, gen)) {
            throw AlpacaException("Motion superseded before dispatch");
        }
        const bool fast = std::abs(signed_rate_deg_per_sec) > kFastModeThresholdDegPerSec;
        // Motion mode: '1' = speed slow, '3' = speed fast.
        double rate = signed_rate_deg_per_sec;
        if (channel == kAxisRa && hemisphere_south_locked()) {
            // TODO: Validate southern hemisphere RA direction on hardware.
            rate = -rate;
        }
        protocol.set_motion_mode(channel, fast ? '3' : '1', direction_char(rate));
        protocol.set_step_period(channel, step_period_for_locked(channel, rate, fast));
        protocol.start_motion(channel);
        cmd_axis_rate_deg_s_[channel - 1] = rate;
    }

    // Base drive rate for the selected ASCOM DriveRate.
    double base_tracking_rate_locked() const {
        switch (tracking_rate_) {
            case 1:
                return kLunarDegPerSec;
            case 2:
                return kSolarDegPerSec;
            default:
                return kSiderealDegPerSec;
        }
    }

    // RA drive rate with the RightAscensionRate offset folded in. Positive
    // offset = RA increasing = axis SLOWER (RA = LST - HA) -> subtract.
    double effective_ra_rate_locked() const {
        return base_tracking_rate_locked() - ra_rate_sec_per_sidereal_sec_ * kRaRateSecondsToDegPerSec;
    }

    // Slowest achievable slow-mode rate (":I" clamps at 0xFFFFFF): ~0.26
    // arcsec/s on the Wave 100i. Sub-floor Dec offsets are duty-cycled.
    double slow_mode_floor_rate_locked(int channel) const {
        const AxisParameters& params = axis_params_[channel - 1];
        return static_cast<double>(params.timer_frequency) * 360.0 /
               (static_cast<double>(params.counts_per_revolution) * static_cast<double>(kCountsMask));
    }

    // Start/stop/duty the Dec-axis offset motion for DeclinationRate. Sign:
    // dec = 90 - a2 on the east-pointing branch (a2 >= 0) -> +Dec is NEGATIVE
    // axis motion there (ConformU 4.5 measured-rate confirmed).
    // TODO(#214 follow-up): the sign is evaluated at (re)apply time and held;
    // a session whose dec axis crosses the branch boundary (a2 through 0)
    // between apply events keeps the stale sign until the next goto, pulse,
    // tracking toggle, or rate write re-applies it. Long unattended sessions
    // near the pole should re-set DeclinationRate after a meridian flip.
    void apply_dec_rate_offset_locked(std::unique_lock<std::mutex>& lock) {
        double rate = dec_rate_arcsec_per_sec_ / 3600.0;
        if (rate == 0.0) {
            dec_duty_rate_deg_s_ = 0.0;
            if (dec_offset_running_) {
                const uint64_t gen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, gen));
                dec_offset_running_ = false;
            }
            return;
        }
        refresh_position_cache_locked(false);
        if (cached_dec_axis_deg_ >= 0.0) {
            rate = -rate;
        }
        // Rates within kSlowModeFloorPad of the floor still duty-cycle: an
        // ":I" value pinned at 0xFFFFFF cannot resolve them continuously.
        double floor_rate = slow_mode_floor_rate_locked(kAxisDec) * kSlowModeFloorPad;
        if (std::abs(rate) >= floor_rate) {
            dec_duty_rate_deg_s_ = 0.0;
            start_speed_motion_locked(lock, kAxisDec, rate);
        } else {
            if (dec_offset_running_) {
                const uint64_t gen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, gen));
            }
            dec_duty_rate_deg_s_ = rate;
            cmd_axis_rate_deg_s_[1] = rate;  // dead-reckon the requested average
        }
        dec_offset_running_ = true;
    }

    // Duty-cycle worker for sub-floor Dec rates: floor-rate bursts sized to
    // the requested average. Exits when the duty rate returns to zero; idles
    // while tracking is off or a slew/park/home owns the axes.
    void dec_duty_loop() {
        constexpr auto kDutyPeriod = std::chrono::milliseconds(3000);
        while (!dec_duty_cancel_.load()) {
            double rate = 0.0;
            double floor_rate = 0.0;
            bool go = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rate = dec_duty_rate_deg_s_;
                if (rate == 0.0) {
                    break;
                }
                floor_rate = slow_mode_floor_rate_locked(kAxisDec);
                go = connected_ && tracking_ && !parking_ && !homing_ && !goto_in_progress_ && !slewing_cached_ &&
                     !pulse_guiding_active_ && !manual_axis_slewing_[0] && !manual_axis_slewing_[1];
            }
            if (!go) {
                if (!task_wait_for(std::chrono::milliseconds(500), dec_duty_cancel_)) {
                    break;
                }
                continue;
            }
            // ~140 ms stop-landing overrun measured on hardware; shorten the
            // wait so the physical on-duration matches the duty fraction.
            // The RAW floor is correct here (the burst physically runs at
            // it); rates inside the kSlowModeFloorPad margin just compute an
            // on-time near/above the period and the clamp makes them
            // effectively continuous.
            auto on_time = std::chrono::milliseconds(
                std::clamp(static_cast<int>(3000.0 * std::abs(rate) / floor_rate) - 140, 50, 3000));
            uint64_t burst_gen = 0;
            bool burst_started = false;
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                if (dec_duty_rate_deg_s_ != rate) continue;
                start_speed_motion_locked(lock, kAxisDec, rate > 0.0 ? floor_rate : -floor_rate);
                burst_gen = motion_generation_;  // owned by THIS burst
                burst_started = true;
                cmd_axis_rate_deg_s_[1] = rate;
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Superseded or transport hiccup; next cycle re-evaluates.
            }
            if (!task_wait_for(on_time, dec_duty_cancel_)) break;
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                // Only stop the motion this burst itself started: a fresher
                // generation means a pulse/MoveAxis/goto took the axis while
                // the burst timer ran — the newer command owns it now, and
                // stop_axis_and_wait_locked abandons on the stale generation.
                if (burst_started && dec_duty_rate_deg_s_ == rate && motion_generation_ == burst_gen &&
                    !goto_in_progress_ && !parking_ && !homing_ && !pulse_guiding_active_ && !manual_axis_slewing_[0] &&
                    !manual_axis_slewing_[1]) {
                    static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, burst_gen));
                    cmd_axis_rate_deg_s_[1] = rate;  // still the average rate
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            if (!task_wait_for(kDutyPeriod - on_time, dec_duty_cancel_)) break;
        }
        // Never leave the axis creeping on exit. A zeroed duty rate means
        // whoever cleared it already stopped the axis; only a cancel with the
        // rate still set (disconnect mid-burst) needs the safety stop.
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            if (dec_duty_rate_deg_s_ == 0.0) {
                return;
            }
            if (connected_) {
                const uint64_t gen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, gen));
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    void reap_dec_duty_task() {
        std::lock_guard<std::mutex> lifecycle(duty_lifecycle_mutex_);
        reap_dec_duty_locked_lifecycle();
    }

    // Requires duty_lifecycle_mutex_. Moves the thread slot out under
    // task_mutex_ and joins with only the lifecycle mutex held.
    void reap_dec_duty_locked_lifecycle() {
        dec_duty_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(dec_duty_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        dec_duty_cancel_.store(false);
    }

    // Re-command the RA axis after a rate change. In-place step-period writes
    // are only legal while the direction is unchanged; a sign flip (or a
    // stopped/reversed axis) needs a full stop-and-restart.
    void apply_ra_tracking_rate_locked(std::unique_lock<std::mutex>& lock, double previous_effective) {
        double eff = effective_ra_rate_locked();
        if (eff != 0.0 && previous_effective != 0.0 && (eff > 0.0) == (previous_effective > 0.0)) {
            auto& protocol = SkyWatcherProtocolWrapper::instance();
            protocol.set_step_period(kAxisRa, tracking_step_period_for(eff));
            cmd_axis_rate_deg_s_[0] = eff;  // keep dead reckoning on the new rate
        } else {
            start_speed_motion_locked(lock, kAxisRa, eff);
        }
    }

    void set_tracking_locked(std::unique_lock<std::mutex>& lock, bool tracking) {
        if (tracking) {
            // Track: RA axis in the direction of increasing hour angle
            // (positive axis angle by this driver's convention) at the
            // selected drive rate plus any RightAscensionRate offset.
            start_speed_motion_locked(lock, kAxisRa, effective_ra_rate_locked());
            apply_dec_rate_offset_locked(lock);
        } else {
            const uint64_t gen = ++motion_generation_;
            if (!stop_axis_and_wait_locked(lock, kAxisRa, gen)) {
                // A newer motion command took the axes while the mutex was
                // released: it owns the tracking state now — do not stomp it.
                throw AlpacaException("Tracking change superseded by a concurrent motion command");
            }
            if (dec_offset_running_) {
                const uint64_t dgen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, dgen));
                dec_offset_running_ = false;
            }
            // Let the duty worker exit while tracking is off; re-enabling
            // tracking recomputes the duty rate and restarts it.
            dec_duty_rate_deg_s_ = 0.0;
        }
        tracking_ = tracking;
        invalidate_position_cache_locked();
    }

    void dispatch_goto_locked(std::unique_lock<std::mutex>& lock, double target_ra_axis_deg,
                              double target_dec_axis_deg) {
        auto& protocol = SkyWatcherProtocolWrapper::instance();
        const uint64_t gen = ++motion_generation_;
        cmd_axis_rate_deg_s_[0] = 0.0;
        cmd_axis_rate_deg_s_[1] = 0.0;
        // A single generation spans BOTH stop-waits and the goto commands
        // below: if either wait is superseded (AbortSlew bumps the
        // generation too), the whole dispatch aborts before any new motor
        // command is sent.
        // Evaluate BOTH waits unconditionally (round-4 finding: a short-
        // circuit skipped the second axis entirely) — while the stale-
        // generation gate inside the wait ensures a superseded dispatch
        // never emits stops that could clobber the superseding command's
        // fresh motion (round-6 finding).
        const bool ra_stopped = stop_axis_and_wait_locked(lock, kAxisRa, gen);
        const bool dec_stopped = stop_axis_and_wait_locked(lock, kAxisDec, gen);
        if (!ra_stopped || !dec_stopped) {
            throw AlpacaException("Slew superseded before dispatch");
        }
        refresh_position_cache_locked(true);

        struct AxisGoto {
            int channel;
            double current_deg;
            double target_deg;
        };
        const AxisGoto plans[2] = {
            {kAxisRa, cached_ra_axis_deg_, target_ra_axis_deg},
            {kAxisDec, cached_dec_axis_deg_, target_dec_axis_deg},
        };
        for (const auto& plan : plans) {
            double delta = plan.target_deg - plan.current_deg;
            // Motion mode '0' = fast GOTO (the controller manages ramp and the
            // brake point); direction from the signed move.
            protocol.set_motion_mode(plan.channel, '0', direction_char(delta));
            protocol.set_goto_target(
                plan.channel,
                degrees_to_counts(plan.target_deg,
                                  axis_params_[static_cast<std::size_t>(plan.channel - 1)].counts_per_revolution));
        }
        protocol.start_motion(kAxisRa);
        protocol.start_motion(kAxisDec);
    }

    // LST advances 24 sidereal hours per sidereal day of SI seconds.
    static constexpr double kLstHoursPerSecond = 24.0 / 86164.0905;
    static constexpr double kGotoRampSeconds = 2.5;
    // After the goto lands, the RA axis sits stopped while tracking restarts
    // (~0.7 s); aim that far ahead so the drift-back lands ON target.
    static constexpr double kTrackingResumeSeconds = 0.7;
    // Landing deadband per axis (~8 arcsec; ConformU checks RA to 10 arcsec).
    static constexpr double kLandingDeadbandDeg = 8.0 / 3600.0;

    // First goto aimed at the ARRIVAL-time sky position: estimate the slew
    // duration from the distance and advance the LST used for the axis
    // target. An uncompensated goto lands east by the slew duration
    // (~3 arcmin of RA for a long slew on the Wave 100i).
    void dispatch_predicted_goto_locked(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        refresh_position_cache_locked(true);
        auto [p1, p2] = ra_dec_to_axis_degrees_locked(ra, dec);
        double dist = std::max(std::abs(p1 - cached_ra_axis_deg_), std::abs(p2 - cached_dec_axis_deg_));
        double est_seconds = dist / kMaxMoveAxisRateDegPerSec + kGotoRampSeconds + kTrackingResumeSeconds;
        auto [t1, t2] = ra_dec_to_axis_degrees_locked(ra, dec, est_seconds * kLstHoursPerSecond);
        dispatch_goto_locked(lock, t1, t2);
    }

    // After the first goto lands, close the residual (prediction error) with
    // short re-gotos until inside the deadband. Slewing is held true across
    // the inter-goto gaps via slew_force_until_.
    void refine_goto_landing(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        for (int iter = 0; iter < 3; ++iter) {
            if (slew_task_cancel_.load()) {
                break;  // AbortSlew/unpark/disconnect cancelled the slew
            }
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            refresh_position_cache_locked(true);
            // Judge the landing against the target ADVANCED by the resume
            // window -- the mount deliberately lands ahead (see above).
            auto [n1, n2] = ra_dec_to_axis_degrees_locked(ra, dec, kTrackingResumeSeconds * kLstHoursPerSecond);
            if (std::abs(n1 - cached_ra_axis_deg_) <= kLandingDeadbandDeg &&
                std::abs(n2 - cached_dec_axis_deg_) <= kLandingDeadbandDeg) {
                break;
            }
            slewing_cached_ = true;
            dispatch_predicted_goto_locked(lock, ra, dec);
            wait_for_slew_complete(lock);
        }
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        slewing_cached_ = false;
    }

    void do_slew_to_ra_dec_locked(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        invalidate_position_cache_locked();
        slewing_cached_ = true;
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        restore_tracking_after_slew_ = tracking_;
        try {
            dispatch_predicted_goto_locked(lock, ra, dec);
        } catch (...) {
            // Dispatch failed before the mount started moving: clear the
            // pre-published slew state so Slewing cannot wedge true.
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            restore_tracking_after_slew_ = false;
            throw;
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_set_ = true;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parked_ = false;
        at_home_ = false;
    }

    // Some controllers stop tracking during a GOTO and do not resume; always
    // re-issue tracking after a completed slew when it was on (project lesson).
    void restore_tracking_after_slew_locked(std::unique_lock<std::mutex>& lock) {
        if (restore_tracking_after_slew_) {
            restore_tracking_after_slew_ = false;
            try {
                set_tracking_locked(lock, true);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SkyWatcher", std::string("Failed to restore tracking after slew: ") + e.what());
            }
        }
    }

    bool get_slewing_locked() const {
        // A park in progress reports Slewing until AtPark flips in the same
        // locked step -- ConformU polls Slewing for park completion, and a
        // fast (localhost) poller caught the gap between the slew ending and
        // parked_ being set, declaring the park failed.
        if (parking_ || homing_ || goto_in_progress_) {
            return true;
        }
        return get_hardware_slewing_locked();
    }

    // Hardware/manual slewing state WITHOUT the parking_ override. The park
    // task's own wait_for_slew_complete must poll this variant: polling
    // get_slewing_locked() while parking_ is set can never see "stopped" and
    // times out at 180s (AtPark stays false -- ConformU Park failure).
    bool get_hardware_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (std::chrono::steady_clock::now() < slew_force_until_) {
            return true;
        }
        bool was_slewing = slewing_cached_;
        try {
            auto& protocol = SkyWatcherProtocolWrapper::instance();
            AxisStatus ra = protocol.inquire_status(kAxisRa);
            AxisStatus dec = protocol.inquire_status(kAxisDec);
            // Trust the controller's status register: a GOTO is in progress
            // while either axis is running in GOTO mode. A tracking axis
            // (speed mode) is NOT slewing.
            slewing_cached_ = (ra.running && !ra.speed_mode) || (dec.running && !dec.speed_mode);
            last_slewing_poll_ = std::chrono::steady_clock::now();
        } catch (...) {
            // Keep last known state across a transient poll failure, but a
            // sustained fault must surface, not report frozen Slewing forever.
            if ((std::chrono::steady_clock::now() - last_slewing_poll_) > kStaleCacheLimit) {
                throw;
            }
        }
        if (was_slewing && !slewing_cached_) {
            invalidate_position_cache_locked();
        }
        return slewing_cached_;
    }

    // ── AutoHome (home index sensors) ───────────────────────────────────────
    // Port of the SynScan/EQMod AutoHome procedure (indi-eqmod eqmodbase.cpp).
    // The Wave's home index sensor latches the axis count when the axis sweeps
    // past the physical home mark. Reading the indexer (":q" data 0x000000)
    // returns 0 (armed, currently below the index), 0xFFFFFF (armed, above),
    // or the latched count; ":W" data 0x000008 re-arms it. The procedure hunts
    // the sensor edge on both axes, always makes the final approach from below
    // (consistent direction kills backlash), then re-stamps the position
    // registers to kHomeCounts at the sensed mark — re-anchoring the count
    // frame to the physical home regardless of where the mount was powered on.
    // TODO: Validate AutoHome direction conventions in the southern hemisphere
    // (start_speed_motion_locked flips the RA sign there).

    void autohome_sleep(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds d) const {
        lock.unlock();
        std::this_thread::sleep_for(d);
        lock.lock();
        check_connected();
        if (slew_task_cancel_.load()) {
            throw AlpacaException("AutoHome cancelled");
        }
    }

    void autohome_wait_axes_stopped(std::unique_lock<std::mutex>& lock) const {
        auto& proto = SkyWatcherProtocolWrapper::instance();
        const auto start = std::chrono::steady_clock::now();
        while (proto.inquire_status(kAxisRa).running || proto.inquire_status(kAxisDec).running) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(300)) {
                throw AlpacaException("AutoHome: axes did not stop");
            }
            autohome_sleep(lock, std::chrono::milliseconds(250));
        }
    }

    void run_autohome(std::unique_lock<std::mutex>& lock) {
        auto& proto = SkyWatcherProtocolWrapper::instance();
        const int axes[2] = {kAxisRa, kAxisDec};
        auto read_idx = [&](int i) { return proto.get_feature(axes[i], kIndexerInquiry); };
        auto reset_idx = [&](int i) { proto.set_feature(axes[i], kIndexerReset); };
        auto axis_deg = [&](int i) { return i == 0 ? cached_ra_axis_deg_ : cached_dec_axis_deg_; };
        auto idx_to_deg = [&](int i, uint32_t counts) {
            return counts_to_degrees(counts, axis_params_[i].counts_per_revolution);
        };

        // Phase 1: stop everything, arm the indexers, pick directions from the
        // armed reading (0 = below the index -> move down first, away from it),
        // and step 5 degrees off so the edge is approached cleanly.
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 1: arming home indexers");
        {
            const uint64_t gen = ++motion_generation_;
            // Both waits evaluated unconditionally (see dispatch_goto_locked).
            const bool ra_stopped = stop_axis_and_wait_locked(lock, kAxisRa, gen);
            const bool dec_stopped = stop_axis_and_wait_locked(lock, kAxisDec, gen);
            if (!ra_stopped || !dec_stopped) {
                throw AlpacaException("AutoHome cancelled");
            }
        }
        tracking_ = false;
        bool up[2];
        for (int i = 0; i < 2; ++i) {
            reset_idx(i);
            up[i] = read_idx(i) == 0;
        }
        refresh_position_cache_locked(true);
        dispatch_goto_locked(lock, axis_deg(0) + (up[0] ? -5.0 : 5.0), axis_deg(1) + (up[1] ? -5.0 : 5.0));
        autohome_wait_axes_stopped(lock);

        // Phase 2: if the 5-degree step swept PAST the index (latched), move a
        // further 5 degrees in the same direction, then re-arm and re-read the
        // side (0 now means above -> hunt downward).
        bool swept[2];
        for (int i = 0; i < 2; ++i) {
            uint32_t v = read_idx(i);
            swept[i] = v != 0 && v != kIndexerAbove;
        }
        if (swept[0] || swept[1]) {
            ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 2: stepping past a latched index");
            refresh_position_cache_locked(true);
            dispatch_goto_locked(lock, axis_deg(0) + (swept[0] ? (up[0] ? -5.0 : 5.0) : 0.0),
                                 axis_deg(1) + (swept[1] ? (up[1] ? -5.0 : 5.0) : 0.0));
            autohome_wait_axes_stopped(lock);
            for (int i = 0; i < 2; ++i) {
                if (swept[i]) {
                    reset_idx(i);
                    up[i] = read_idx(i) != 0;
                }
            }
        }

        // Phase 3: any axis above the index hunts downward at the coarse rate
        // until the indexer reports the below side, runs 3 s further, stops,
        // and re-arms — every axis now sits below its index.
        if (!up[0] || !up[1]) {
            ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 3: coarse hunt below the index");
            bool hunting[2] = {!up[0], !up[1]};
            for (int i = 0; i < 2; ++i) {
                if (hunting[i]) {
                    start_speed_motion_locked(lock, axes[i], -kAutoHomeCoarseRateDegPerSec);
                }
            }
            std::chrono::steady_clock::time_point edge_at[2];
            bool edge[2] = {false, false};
            const auto start = std::chrono::steady_clock::now();
            while (hunting[0] || hunting[1]) {
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(300)) {
                    throw AlpacaException("AutoHome: coarse hunt timed out");
                }
                for (int i = 0; i < 2; ++i) {
                    if (!hunting[i]) continue;
                    if (!edge[i] && read_idx(i) != kIndexerAbove) {
                        edge[i] = true;
                        edge_at[i] = std::chrono::steady_clock::now();
                    }
                    if (edge[i] && std::chrono::steady_clock::now() - edge_at[i] > std::chrono::seconds(3)) {
                        {
                            const uint64_t gen = ++motion_generation_;
                            if (!stop_axis_and_wait_locked(lock, axes[i], gen)) {
                                throw AlpacaException("AutoHome cancelled");
                            }
                        }
                        reset_idx(i);
                        up[i] = true;
                        hunting[i] = false;
                    }
                }
                autohome_sleep(lock, std::chrono::milliseconds(200));
            }
        }

        // Phase 4: sweep upward at the detect rate; the indexer latches the
        // exact count of the home mark as each axis crosses it.
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 4: detecting the home index");
        uint32_t home_idx[2] = {0, 0};
        bool latched[2] = {false, false};
        start_speed_motion_locked(lock, kAxisRa, kAutoHomeDetectRateDegPerSec);
        start_speed_motion_locked(lock, kAxisDec, kAutoHomeDetectRateDegPerSec);
        const auto detect_start = std::chrono::steady_clock::now();
        while (!latched[0] || !latched[1]) {
            if (std::chrono::steady_clock::now() - detect_start > std::chrono::seconds(300)) {
                proto.stop_motion(kAxisRa);
                proto.stop_motion(kAxisDec);
                throw AlpacaException("AutoHome: index detect timed out");
            }
            for (int i = 0; i < 2; ++i) {
                if (latched[i]) continue;
                uint32_t v = read_idx(i);
                if (v != 0) {
                    home_idx[i] = v;
                    latched[i] = true;
                    {
                        const uint64_t gen = ++motion_generation_;
                        if (!stop_axis_and_wait_locked(lock, axes[i], gen)) {
                            throw AlpacaException("AutoHome cancelled");
                        }
                    }
                }
            }
            autohome_sleep(lock, std::chrono::milliseconds(150));
        }
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome: index latched at RA=" + std::to_string(home_idx[0]) +
                                          " Dec=" + std::to_string(home_idx[1]));

        // Phase 5+6: back 10 degrees below the latched mark, then approach it
        // from below and stamp the position registers to the home offset.
        dispatch_goto_locked(lock, idx_to_deg(0, home_idx[0]) - 10.0, idx_to_deg(1, home_idx[1]) - 10.0);
        autohome_wait_axes_stopped(lock);
        dispatch_goto_locked(lock, idx_to_deg(0, home_idx[0]), idx_to_deg(1, home_idx[1]));
        autohome_wait_axes_stopped(lock);
        proto.set_position(kAxisRa, kHomeCounts);
        proto.set_position(kAxisDec, kHomeCounts);
        invalidate_position_cache_locked();
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome: complete, count frame re-anchored to home");
    }

    // Poll for slew completion, RELEASING the mutex around every sleep so a
    // sync slew/park doesn't block all GETs (project reference pattern).
    void wait_for_slew_complete(std::unique_lock<std::mutex>& lock) const {
        const auto timeout = std::chrono::seconds(180);
        auto start = std::chrono::steady_clock::now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            std::this_thread::sleep_for(d);
            lock.lock();
            check_connected();
        };
        while (true) {
            if (slew_task_cancel_.load()) {
                // A reap (unpark cancelling an in-flight park, or a newer async
                // slew) wants this waiter gone; abandon the wait promptly so
                // the join is bounded.
                throw AlpacaException("Slew wait cancelled");
            }
            bool slewing = get_hardware_slewing_locked();
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
        invalidate_position_cache_locked();
        // No post-slew position freeze: gotos are LST-compensated and refined
        // (see goto helpers), so live reads land on target -- freezing them
        // corrupted ConformU's rate-offset endpoint measurements instead.
        if (slew_settle_time_seconds_ > 0) {
            sleep_unlocked(std::chrono::seconds(slew_settle_time_seconds_));
        }
    }

    // ── Background task threads (async slew, pulse stop) ────────────────────

    bool task_wait_for(std::chrono::milliseconds d, std::atomic<bool>& cancel) const {
        std::unique_lock<std::mutex> tlock(task_mutex_);
        task_cv_.wait_for(tlock, d, [&] { return cancel.load(); });
        return !cancel.load();
    }

    // A slew/park/home task that dies CANCELLED may have launched its goto
    // before the cancel landed (abort in the pre-dispatch window): the goto
    // must not keep running. Best effort — an aborting reaper re-commands or
    // has already stopped the axes; running this before the reaper's join
    // returns keeps the two orderings consistent.
    void stop_axes_if_cancelled_locked() {
        if (!slew_task_cancel_.load() || !connected_) {
            return;
        }
        try {
            auto& protocol = SkyWatcherProtocolWrapper::instance();
            protocol.instant_stop(kAxisRa);
            protocol.instant_stop(kAxisDec);
            cmd_axis_rate_deg_s_[0] = 0.0;
            cmd_axis_rate_deg_s_[1] = 0.0;
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Best effort; the axes were already stopped by the aborter.
        }
    }

    void cancel_async_tasks() {
        slew_task_cancel_.store(true);
        pulse_task_cancel_.store(true);
        stop_task_cancel_.store(true);
        dec_duty_cancel_.store(true);
        task_cv_.notify_all();
        std::thread slew_thread;
        std::thread pulse_thread;
        std::thread stop_thread;
        std::thread duty_thread;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            slew_thread = std::move(slew_task_thread_);
            pulse_thread = std::move(pulse_task_thread_);
            stop_thread = std::move(stop_task_thread_);
            duty_thread = std::move(dec_duty_thread_);
        }
        if (slew_thread.joinable()) {
            slew_thread.join();
        }
        if (pulse_thread.joinable()) {
            pulse_thread.join();
        }
        if (stop_thread.joinable()) {
            stop_thread.join();
        }
        if (duty_thread.joinable()) {
            duty_thread.join();
        }
        dec_duty_cancel_.store(false);
    }

    void reap_stop_task() {
        stop_task_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(stop_task_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        stop_task_cancel_.store(false);
    }

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

    // ── State ───────────────────────────────────────────────────────────────

    int device_number_;
    ConnectionInfo connection_info_;
    mutable std::mutex mutex_;
    bool connected_ = false;

    AxisParameters axis_params_[2]{};

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    mutable bool target_set_ = false;

    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;

    double site_latitude_;
    double site_longitude_;
    double site_elevation_m_;
    std::chrono::system_clock::duration utc_offset_{};

    mutable double cached_ra_axis_deg_ = 0.0;
    mutable double cached_dec_axis_deg_ = 0.0;
    mutable bool position_cache_valid_ = false;
    mutable std::chrono::steady_clock::time_point last_position_update_{};

    bool tracking_ = false;
    bool restore_tracking_after_slew_ = false;
    mutable bool parked_ = false;
    mutable bool at_home_ = false;
    mutable bool slewing_cached_ = false;
    mutable std::chrono::steady_clock::time_point last_slewing_poll_ = std::chrono::steady_clock::now();
    mutable std::chrono::steady_clock::time_point slew_force_until_ = std::chrono::steady_clock::time_point::min();
    mutable bool manual_axis_slewing_[2] = {false, false};

    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;
    GuideRate guide_rate_{};
    mutable bool pulse_guiding_active_ = false;
    mutable std::chrono::steady_clock::time_point pulse_guide_end_time_{};

    mutable bool parking_ = false;
    mutable bool homing_ = false;
    // True from goto dispatch until the landing refinement finishes: Slewing
    // must not flicker false mid-refinement (the timed slew_force_until_
    // expired during a slow refine iteration, ConformU proceeded, and the
    // next refinement goto fought its pulse-guide test for the motors).
    mutable bool goto_in_progress_ = false;
    bool has_home_indexer_ = false;
    int tracking_rate_ = 0;                      // ASCOM DriveRate (0/1/2)
    double ra_rate_sec_per_sidereal_sec_ = 0.0;  // RightAscensionRate
    double dec_rate_arcsec_per_sec_ = 0.0;       // DeclinationRate
    double dec_duty_rate_deg_s_ = 0.0;           // sub-floor Dec rate (duty-cycled)
    bool dec_offset_running_ = false;
    std::thread dec_duty_thread_;
    std::atomic<bool> dec_duty_cancel_{false};
    std::mutex duty_lifecycle_mutex_;                     // serializes duty-worker reap+create
    mutable double cmd_axis_rate_deg_s_[2] = {0.0, 0.0};  // dead-reckoning rates
    uint64_t motion_generation_ = 0;                      // bumped by every motion command; guards unlocked stop-waits
    bool park_position_set_ = false;
    double park_ra_axis_deg_ = 0.0;
    double park_dec_axis_deg_ = 0.0;

    // Web-UI firmware copy under its own narrow mutex (never mutex_).
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;

    // Background task threads; task_mutex_ only guards handles + cv, never
    // held across protocol I/O.
    mutable std::mutex task_mutex_;
    mutable std::condition_variable task_cv_;
    std::thread slew_task_thread_;
    std::thread pulse_task_thread_;
    std::thread stop_task_thread_;
    mutable std::atomic<bool> slew_task_cancel_{false};
    mutable std::atomic<bool> pulse_task_cancel_{false};
    mutable std::atomic<bool> stop_task_cancel_{false};
};

std::unique_ptr<TelescopeDriver> create_skywatcher_telescope(int device_number, const ConnectionInfo& connection_info,
                                                             std::optional<double> site_latitude_deg,
                                                             std::optional<double> site_longitude_deg,
                                                             std::optional<double> site_elevation_m) {
    return std::make_unique<SkyWatcherTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                       site_longitude_deg, site_elevation_m);
}

std::unique_ptr<TelescopeDriver> create_skywatcher_telescope_auto(int device_number, int mount_index,
                                                                  std::optional<double> site_latitude_deg,
                                                                  std::optional<double> site_longitude_deg,
                                                                  std::optional<double> site_elevation_m) {
    auto ports = enumerate_skywatcher_ports();
    if (!ports.empty()) {
        if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
            throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                                  std::to_string(ports.size()) + " mount(s))");
        }
        const auto& port = ports[static_cast<std::size_t>(mount_index)];
        ALPACA_LOG_INFO("SkyWatcher",
                        "Auto-detected mount on " + port.port_path + " (MC fw " + port.firmware_version + ")");
        ConnectionInfo conn;
        conn.type = ConnectionType::Serial;
        conn.port_path = port.port_path;
        return create_skywatcher_telescope(device_number, conn, site_latitude_deg, site_longitude_deg,
                                           site_elevation_m);
    }

    auto hosts = discover_skywatcher_hosts();
    if (hosts.empty()) {
        throw AlpacaException(
            "No Sky-Watcher motor controller found on any serial port or via "
            "Wi-Fi discovery (UDP 11880).");
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(hosts.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                              std::to_string(hosts.size()) + " mount(s))");
    }
    const auto& host = hosts[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("SkyWatcher", "Auto-detected mount at " + host.host + " (MC fw " + host.firmware_version + ")");
    ConnectionInfo conn;
    conn.type = ConnectionType::Network;
    conn.host = host.host;
    conn.udp_port = host.udp_port;
    return create_skywatcher_telescope(device_number, conn, site_latitude_deg, site_longitude_deg, site_elevation_m);
}

}  // namespace alpacacore::vendor::skywatcher

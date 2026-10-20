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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/park_slew.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>

namespace alpacacore::vendor::celestron {

namespace {

constexpr double kHoursToDegrees = 15.0;
// Decision 0010, Celestron adoption. A poll body samples the mount every kStatusPollPeriod (start to start; ticks
// missed behind a slow link are skipped, never made up): the slewing flag and the equatorial pair on every tick, the
// slower-moving fields (alt/az, pier side, tracking mode) on every kSlowPollEvery-th. A full frame is up to five
// handset round trips, so the cadence stays slow enough to leave the line to the control path. Alt/az, pier side and
// tracking mode move only with a motion the driver commands (and the control path publishes a full frame when that
// motion ends) or with a keypad press.
// kSnapshotMaxAge is the FRESHNESS bound: a frame older than it reads `stale` and is still served. It is not the
// bound at which a getter refuses: that is the link-loss policy of decision 0009 (util::kLinkStalenessBound).
constexpr auto kStatusPollPeriod = std::chrono::milliseconds(1000);
constexpr int kSlowPollEvery = 4;
constexpr auto kSnapshotMaxAge = std::chrono::milliseconds(3000);
static_assert(kSnapshotMaxAge >= 2 * kStatusPollPeriod, "two poll periods, or a healthy link reads stale");
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");

// One polled frame of operational state (decision 0010). Small and copyable: the snapshot copies it under its mutex.
struct PolledFrame {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
    bool equatorial_known = false;
    double az_degrees = 0.0;
    double alt_degrees = 0.0;
    bool altaz_known = false;
    bool slewing = false;
    bool slewing_known = false;  // false until a query (or a control-path write) has answered
    int side_of_pier = -1;       // ASCOM value, -1 (pierUnknown) when the handset's state maps to neither side
    bool pier_known = false;     // the handset has answered the pier query at least once
    int tracking_mode = -1;      // handset tracking mode, -1 unknown
};
constexpr int kLinkFailureThreshold = 3;
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
// The poll's own retry of an unread site. A failed read ends the tick without a sample (one exchange timeout at
// most), and a full tick comes every 4 s, so a 2 s retry would skip the sample of every full tick on a link whose
// location never reads; 30 s leaves one tick in about eight.
constexpr auto kSitePollRetryDelay = std::chrono::seconds(30);
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

class CelestronTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    CelestronTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                             std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                             std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                             CelestronAlignmentSetting alignment,
                             util::ConnectionResolver<ConnectionInfo> connection_resolver = {},
                             util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("Celestron"),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
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
          parked_(false),
          at_home_(false),
          mount_model_id_(-1),
          use_precise_commands_(true),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)),
          alignment_setting_(alignment),
          clock_(clock) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~CelestronTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        stop_status_poller();
        // Cancel + join the background slew/pulse task threads before any
        // member they touch is destroyed. Must run WITHOUT mutex_ held (the
        // slew task tail re-acquires mutex_).
        cancel_async_tasks();
        if (connected_) {
            // Qualify the call so it binds statically (we want this class's
            // implementation, not virtual dispatch from a destructor), and
            // swallow any throw so it cannot escape the noexcept destructor.
            try {
                CelestronTelescopeDriver::set_connected(false);
            } catch (...) {
                // Best-effort disconnect on destruction; log and continue so
                // nothing escapes the noexcept destructor.
                ALPACA_LOG_WARN("Celestron", "Error disconnecting mount during destruction");
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
        return connected_ && !link_lost_.load();
    }

    // Reads only the published copy under its own narrow mutex, never the coarse
    // mutex_ that set_connected() holds across the connect, so a configureddevices
    // poll does not wait on the mount connection.
    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        return link_fault_text_;
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_, std::defer_lock);
        // Decision 0009 point 4: a lost link is relinked only by the client's Connected=true, which tears the dead
        // session down (slew bodies included) and connects afresh.
        bool relink = false;
        if (connected) {
            std::lock_guard<std::mutex> guard(mutex_);
            relink = connected_ && link_lost_.load();
        }
        if (!connected || relink) {
            ilock.lock();
            // Cancel + join the status poll and the background slew/pulse task
            // threads BEFORE taking mutex_: the task threads take mutex_, so
            // joining under the lock would deadlock, and leaving the threads
            // running across the protocol disconnect would race the shared wrapper.
            stop_status_poller();
            cancel_async_tasks();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (relink) {
            teardown_session_locked();
        }
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
        ++motion_generation_;

        auto& protocol = CelestronProtocolWrapper::instance();
        if (connected) {
            // An auto-detected mount resolves its port here, not in the factory (#659).
            util::connect_resolved(
                connection_info_, connection_resolved_, connection_resolver_,
                [&protocol](const ConnectionInfo& info) {
                    if (!protocol.connect(info)) {
                        // Refused TCP connect or vanished serial node: stale, re-scan.
                        if (info.type == ConnectionType::Network || util::device_node_missing(info.port_path)) {
                            throw util::StaleEndpoint("Failed to connect to Celestron mount");
                        }
                        throw AlpacaException("Failed to connect to Celestron mount");
                    }
                },
                "Celestron");
            link_lost_ = false;
            connected_ = true;
            mount_firmware_version_ = "";
            mount_model_id_ = -1;
            mount_model_name_ = "";
            site_info_valid_ = false;
            timezone_offset_valid_ = false;
            target_ra_set_ = false;
            target_dec_set_ = false;
            parked_ = false;
            parking_ = false;
            at_home_ = false;
            homing_ = false;
            pulse_guide_active_.fill(false);
            pulse_guide_end_time_.fill(util::TaskClock::clock::time_point::min());
            snapshot_.reset();
            set_slewing_locked(false);
            slew_force_until_ = util::TaskClock::clock::time_point::min();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            utc_offset_.clear();
            client_disagreement_warned_ = false;
            position_link_health_.reset();
            publish_link_fault_locked();
            last_site_info_attempt_ = util::TaskClock::clock::time_point::min();
            clear_motion_errors_locked();

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
            // Publish the firmware to the web-UI getter under its OWN narrow mutex
            // so a configureddevices poll never blocks on the coarse mutex_ that
            // this connect sequence holds for several seconds.
            {
                // Gate on the version string directly (not hc_available_) so the
                // UI display isn't coupled to HC-command support: suppress the
                // empty / "0.0" sentinel the same way SynScan suppresses "0.0.0".
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = (!mount_firmware_version_.empty() && mount_firmware_version_ != "0.0")
                                      ? mount_firmware_version_
                                      : std::string();
            }
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
            // Seed the snapshot so the first property reads are served from memory, status flags included, and
            // read the clock once, so no getter reaches the mount (ConformU holds the first reads to its
            // fast-response target).
            seed_snapshot_locked();
            ensure_utc_cached_locked();
            try {
                LocationInfo info = protocol.get_location();
                site_latitude_cached_ = info.latitude_degrees;
                site_longitude_cached_ = info.longitude_degrees;
                site_info_valid_ = true;
            } catch (...) {
            }
        } else {
            teardown_session_locked();
        }
        if (connected) {
            lock.unlock();  // the poll slot starts without mutex_ held (AsyncOperation rule 10)
            start_status_poller();
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return alignment_mode_locked();
    }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return served_altaz_locked().second;
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
        check_connected();
        // AtHome is affected only by FindHome failures. A failed goto or Park
        // still surfaces through Slewing without making this independent flag
        // unreadable.
        if (!last_home_error_.empty()) {
            throw AlpacaException(last_home_error_);
        }
        // The status poll checks a FindHome for completion; the getter serves its result.
        return at_home_;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return parked_;
    }

    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return served_altaz_locked().first;
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
        const auto now = clock_.now();
        bool active = false;
        for (std::size_t axis = 0; axis < pulse_guide_active_.size(); ++axis) {
            if (pulse_guide_active_[axis] && now >= pulse_guide_end_time_[axis]) {
                pulse_guide_active_[axis] = false;
            }
            active = active || pulse_guide_active_[axis];
        }
        return active;
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
        if (!position_link_health_.faulted() && target_ra_set_ && target_dec_set_ &&
            clock_.now() < position_override_until_ && !get_slewing_locked()) {
            return std::clamp(target_dec_degrees_, -90.0, 90.0);
        }
        const auto now = clock_.now();
        double dec_value;
        if (!position_link_health_.faulted() && pg_hold_dec_valid_ && now < pg_hold_dec_until_) {
            dec_value = pg_hold_dec_degrees_;
        } else {
            dec_value = served_equatorial_locked().second;
        }
        if (!position_link_health_.faulted() && pg_dec_correction_valid_ && now < pg_dec_correction_until_) {
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
            const AlignmentMode alignment = alignment_mode_locked();
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                ensure_site_info_cached_locked();
            }
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                throw AlpacaException("Set SiteLatitude before enabling equatorial tracking", AlpacaError::ValueNotSet);
            }
            const int mode = tracking_mode_for_alignment_locked(alignment);
            protocol.set_tracking_mode(mode);
            set_tracking_mode_cached_locked(mode);
        } else {
            protocol.set_tracking_mode(0);
            set_tracking_mode_cached_locked(0);
        }
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_autoguider_port_) {
            throw AlpacaException("Guide rates not supported", AlpacaError::PropertyNotImplemented);
        }
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        if (!has_autoguider_port_) {
            throw AlpacaException("Guide rates not supported", AlpacaError::PropertyNotImplemented);
        }
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate must be a finite number", AlpacaError::InvalidValue);
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
        if (!position_link_health_.faulted() && target_ra_set_ && target_dec_set_ &&
            clock_.now() < position_override_until_ && !get_slewing_locked()) {
            return target_ra_hours_;
        }
        const auto now = clock_.now();
        double ra_value;
        if (!position_link_health_.faulted() && pg_hold_ra_valid_ && now < pg_hold_ra_until_) {
            ra_value = pg_hold_ra_hours_;
        } else {
            ra_value = served_equatorial_locked().first;
        }
        if (!position_link_health_.faulted() && pg_ra_correction_valid_ && now < pg_ra_correction_until_) {
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
        const auto frame = served_frame_locked().value;
        if (!frame.pier_known) {
            throw AlpacaException("Celestron mount pier side is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return frame.side_of_pier;
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
        return compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_cached_);
    }

    double get_site_elevation() const override {
        return site_elevation_m_;
    }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters",
                                  AlpacaError::InvalidValue);
        }
        site_elevation_m_ = elevation;
    }

    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_info_valid_) {
            throw AlpacaException("SiteLatitude not available — location not set on mount",
                                  AlpacaError::ValueNotSet);
        }
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
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
            throw AlpacaException("SiteLongitude not available — location not set on mount",
                                  AlpacaError::ValueNotSet);
        }
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
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
        // open-astro#575: surface a stored async-slew failure as an error
        // instead of a silent false, until AbortSlew or a new slew initiator
        // clears it (see last_slew_error_).
        if (!last_slew_error_.empty()) {
            throw AlpacaException(last_slew_error_);
        }
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
            throw AlpacaException("TargetDeclination must be in range -90 to 90 degrees",
                                  AlpacaError::InvalidValue);
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
            throw AlpacaException("TargetRightAscension must be in range 0 to <24 hours",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_ra_hours_ = ra;
        target_ra_set_ = true;
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
        ensure_utc_cached_locked();
        return current_utc_time_locked();
    }

private:
    // The mount's clock, read once at connect so no getter reaches the mount for it.
    void ensure_utc_cached_locked() const {
        if (!utc_offset_.valid()) {
            std::chrono::system_clock::time_point seeded;
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
                seeded = local_time - minutes{total_offset};
            } catch (...) {
                seeded = std::chrono::system_clock::now();
            }
            utc_offset_.set(seeded, clock_.now());
        }
    }

public:
    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        set_utc_date_locked(utc);
    }

private:
    // Body of set_utc_date with mutex_ already held — also called from the
    // connect path (sync_mount_time_locked), which holds mutex_; calling the
    // public locking method there would self-deadlock on the non-recursive mutex.
    void set_utc_date_locked(std::chrono::system_clock::time_point utc) {
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

        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        CelestronProtocolWrapper::instance().set_time(info);
        timezone_offset_minutes_ = tz_info.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = tz_info.dst;
        utc_offset_.set(utc, clock_.now());
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("Celestron", client_minus_host, client_disagreement_warned_);
    }

public:
    void find_home() override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("FindHome");
        }
        cancel_async_tasks();
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        clear_pulse_guiding_locked();
        check_not_parked_locked("FindHome");
        ++motion_generation_;
        auto& protocol = CelestronProtocolWrapper::instance();

        // MC_LEVEL_START (0x0B) — moves each axis to its hardware home switch.
        // Asynchronous: returns immediately, client polls Slewing/AtHome.
        try {
            protocol.level_start(0);
            protocol.level_start(1);
        } catch (const std::exception& ex) {
            fail_home_locked(std::string("FindHome dispatch failed: ") + ex.what());
            throw AlpacaException(last_slew_error_);
        } catch (...) {
            fail_home_locked("FindHome dispatch failed: unknown exception");
            throw AlpacaException(last_slew_error_);
        }

        homing_ = true;
        at_home_ = false;
        set_slewing_locked(true);
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that calls FindHome after a failed GOTO must not be told the OLD
        // goto failed while it's homing.
        clear_motion_errors_locked();
    }

    // Park is an asynchronous initiator (ITelescopeV4; ConformU 4.5 times it
    // against the 1 s STANDARD target): the park slew is dispatched in the
    // slew task thread and this call returns at once. Slewing reports true
    // until the mount reaches the park position and tracking is stopped, at
    // which point AtPark flips true in the same locked step (no window where
    // a poller sees Slewing false with AtPark false). Issue #208.
    void park() override {
        // Serialize against other initiators (SlewToCoordinatesAsync): the
        // check-then-reap-then-spawn sequence must not interleave with
        // another initiator's, or a park could be cancelled and restarted
        // (or a slew could clobber a park) in the gap.
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            // Check BEFORE cancelling: cancelling the slew slot would end a park in
            // flight (its body clears parking_), so a second Park would
            // restart the slew instead of being the documented no-op.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // calling Park twice (or while parking) is harmless
            }
        }
        double park_ra = 0.0;
        double park_dec = 0.0;
        // Cancel + join any previous slew task first. Must run without mutex_
        // held: the task takes mutex_.
        const bool displaced = slew_.running();
        cancel_async_tasks();
        bool prev_slewing = false;
        bool prev_homing = false;
        bool prev_at_home = false;
        bool prev_manual[2] = {false, false};
        auto prev_override = util::TaskClock::clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            clear_pulse_guiding_locked();
            if (parked_ || parking_) {
                return;  // ASCOM: Park on a parked (or parking) mount is harmless.
            }
            if (!park_position_set_) {
                std::tie(park_ra_hours_, park_dec_degrees_) = measure_equatorial_locked();
                park_position_set_ = true;
            }
            park_ra = park_ra_hours_;
            park_dec = park_dec_degrees_;
            validate_ra_dec(park_ra, park_dec, "Park");
            // Run the slew-safety gate synchronously so an unaligned mount
            // still throws from Park (as the blocking implementation did)
            // rather than silently never reaching AtPark.
            check_slew_safety_locked("Park");
            // Snapshot what the block below overwrites, for a refused start.
            prev_slewing = slewing_cached_;
            prev_homing = homing_;
            prev_at_home = at_home_;
            prev_manual[0] = manual_axis_slewing_[0];
            prev_manual[1] = manual_axis_slewing_[1];
            prev_override = position_override_until_;
            // Publish the slewing state before the task starts so a poller
            // never sees Slewing false between Park returning and dispatch.
            set_slewing_locked(true);
            slew_force_until_ = clock_.now() + std::chrono::seconds(8);
            position_override_until_ = util::TaskClock::clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            homing_ = false;
            at_home_ = false;
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that calls Park after a failed GOTO must not be told the OLD
            // goto failed while it's parking.
            clear_motion_errors_locked();
            parking_ = true;
            ++motion_generation_;
        }

        start_slew_body(
            [this, park_ra, park_dec](util::OperationContext& ctx) {
                std::unique_lock<std::mutex> lock(mutex_);
                util::ParkSlewHooks hooks;
                hooks.still_parking = [this] { return connected_ && parking_; };
                hooks.abandon = [this] { parking_ = false; };
                hooks.dispatch = [&] { do_slew_to_coordinates_locked(park_ra, park_dec); };
                hooks.poll_slewing = [this] { return poll_hardware_slewing_locked(); };
                hooks.stop_tracking = [this] {
                    CelestronProtocolWrapper::instance().set_tracking_mode(0);
                    set_tracking_mode_cached_locked(0);
                };
                hooks.fail = [this, &ctx](const std::string& message) { fail_park_locked(message, ctx); };
                hooks.complete = [this] {
                    set_slewing_locked(false);
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
                    flip_in_progress_ = false;
                    // AtPark and Slewing flip in the same locked step, beside the position the mount stopped at.
                    publish_measurement_locked();
                    parked_ = true;
                    parking_ = false;
                };
                util::run_park_slew(ctx, lock, clock_, slew_settle_time_seconds_, hooks);
            },
            displaced,
            [&] {
                parking_ = false;
                set_slewing_locked(prev_slewing);
                homing_ = prev_homing;
                at_home_ = prev_at_home;
                manual_axis_slewing_[0] = prev_manual[0];
                manual_axis_slewing_[1] = prev_manual[1];
                position_override_until_ = prev_override;
                slew_force_until_ = util::TaskClock::clock::time_point::min();
            });
    }

    // Stop a park slew: cancel the GOTO, then both axes to rate 0. Each stop
    // is tried on its own so one failure does not skip the others. Returns
    // the first failure's message, or an empty string when every stop was
    // answered (#742). mutex_ must be held.
    std::string stop_park_slew_locked() const { return util::stop_goto_and_axes(CelestronProtocolWrapper::instance()); }

    void clear_motion_errors_locked() {
        last_slew_error_.clear();
        last_home_error_.clear();
        slew_status_poll_failures_ = 0;
        home_status_poll_failures_ = 0;
    }

    // Park task failure path: stop the hardware, then drop the parking state so
    // the caller can retry. A successful stop publishes Slewing=false; a failed
    // stop leaves a persistent Slewing error because the mount may still move.
    // mutex_ must be held.
    void fail_park_locked(const std::string& message, util::OperationContext& ctx) {
        const std::string stop_error = stop_park_slew_locked();
        if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
            return;
        }
        parking_ = false;
        last_home_error_.clear();
        last_slew_error_ = message;
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ += "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron",
                             "stop after park failure failed: " + stop_error + "; the mount may still be moving");
        }
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        flip_in_progress_ = false;
        ALPACA_LOG_WARN("Celestron", message);
    }

    void fail_slew_locked(const std::string& message, util::OperationContext& ctx) {
        const std::string stop_error = stop_park_slew_locked();
        if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
            return;
        }
        last_home_error_.clear();
        last_slew_error_ = message;
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ += "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron",
                             "stop after slew failure failed: " + stop_error + "; the mount may still be moving");
        }
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        drop_poll_in_flight_locked();
        flip_in_progress_ = false;
        ALPACA_LOG_WARN("Celestron", message);
    }

    void fail_slew_poll_locked(const std::string& message) const {
        const std::string stop_error = stop_park_slew_locked();
        if (!connected_) {
            return;
        }
        last_home_error_.clear();
        last_slew_error_ = message;
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ += "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron", "stop after Slewing poll failure failed: " + stop_error +
                                              "; the mount may still be moving");
        }
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        drop_poll_in_flight_locked();
        flip_in_progress_ = false;
        ALPACA_LOG_WARN("Celestron", message);
    }

    void fail_home_locked(const std::string& message) const {
        const std::string stop_error = stop_park_slew_locked();
        homing_ = false;
        at_home_ = false;
        last_slew_error_ = message;
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ += "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron",
                             "stop after FindHome failure failed: " + stop_error + "; the mount may still be moving");
        }
        last_home_error_ = last_slew_error_;
        ALPACA_LOG_WARN("Celestron", message);
    }

    void pulse_guide(int direction, int duration) override {
        if (!has_autoguider_port_) {
            throw AlpacaException("PulseGuide not supported — no autoguider port detected",
                                  AlpacaError::MethodNotImplemented);
        }
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
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
        const auto now = clock_.now();
        const auto hold_end = now + std::chrono::milliseconds(duration) +
                              kPulseGuideCompletionDelay + kPulseGuideHoldGrace;
        const auto correction_end = now + std::chrono::milliseconds(duration) +
                                    kPulseGuideCompletionDelay + kPulseGuideCorrectionGrace;

        const auto [served_ra, served_dec] = served_equatorial_locked();

        if (is_dec_axis) {
            pg_hold_ra_hours_ = served_ra;
            pg_hold_ra_valid_ = !pulse_guide_active_[0] || now >= pulse_guide_end_time_[0];
            pg_hold_ra_until_ = hold_end;
            pg_hold_dec_valid_ = false;

            double expected_delta = guide_rate_.dec * duration_sec;
            if (direction == 1) expected_delta = -expected_delta;
            pg_dec_baseline_degrees_ = served_dec;
            pg_dec_expected_delta_degrees_ = expected_delta;
            pg_dec_correction_valid_ = true;
            pg_dec_correction_until_ = correction_end;
        } else {
            pg_hold_dec_degrees_ = served_dec;
            pg_hold_dec_valid_ = !pulse_guide_active_[1] || now >= pulse_guide_end_time_[1];
            pg_hold_dec_until_ = hold_end;
            pg_hold_ra_valid_ = false;

            double expected_delta = (guide_rate_.ra * duration_sec) / 15.0;
            if (direction == 3) expected_delta = -expected_delta;
            pg_ra_baseline_hours_ = served_ra;
            pg_ra_expected_delta_hours_ = expected_delta;
            pg_ra_correction_valid_ = true;
            pg_ra_correction_until_ = correction_end;
        }

        // The pulse body never takes mutex_, so joining it here is safe.
        pulse_ops_[axis].cancel_all_and_join();
        pulse_guide_active_[static_cast<std::size_t>(axis)] = false;
        protocol.pulse_guide_axis(axis, velocity, first_chunk_cs);

        pulse_guide_active_[static_cast<std::size_t>(axis)] = true;
        pulse_guide_end_time_[static_cast<std::size_t>(axis)] =
            now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay;
        drop_poll_in_flight_locked();

        // Chain remaining chunks on the axis's own slot (MC_AUX_GUIDE max =
        // 255cs = 2550ms): one slot per axis, so a pulse on RA never supersedes
        // one on Dec. Cancelled + joined on disconnect and in the destructor.
        // The body never takes mutex_, so starting and joining it while mutex_
        // is held (as here) cannot deadlock.
        int remaining_cs = total_cs - first_chunk_cs;
        if (remaining_cs > 0) {
            pulse_ops_[axis].start([axis, velocity, remaining_cs, first_chunk_cs](util::OperationContext& ctx) {
                if (!ctx.wait_for(std::chrono::milliseconds(first_chunk_cs * 10))) {
                    return;
                }
                int left = remaining_cs;
                auto& proto = CelestronProtocolWrapper::instance();
                while (left > 0) {
                    int chunk = std::min(left, 255);
                    try {
                        proto.pulse_guide_axis(axis, velocity, chunk);
                    } catch (const std::exception& e) {
                        // Each MC_AUX_GUIDE chunk is hardware-timed, so a
                        // failed re-arm just ends the pulse early (no runaway).
                        ALPACA_LOG_WARN("Celestron", std::string("Pulse-guide chunk chain aborted: ") + e.what());
                        break;
                    } catch (...) {
                        ALPACA_LOG_WARN("Celestron", "Pulse-guide chunk chain aborted");
                        break;
                    }
                    left -= chunk;
                    if (left > 0 && !ctx.wait_for(std::chrono::milliseconds(chunk * 10))) {
                        return;
                    }
                }
            });
        }
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        std::tie(park_ra_hours_, park_dec_degrees_) = measure_equatorial_locked();
        park_position_set_ = true;
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinates");
            validate_ra_dec(ra, dec, "SlewToCoordinates");
            check_slew_safety_locked("SlewToCoordinates");
            check_tracking_for_slew_locked("SlewToCoordinates");
        }
        cancel_async_tasks();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        clear_pulse_guiding_locked();
        check_not_parked_locked("SlewToCoordinates");
        do_slew_to_coordinates_locked(ra, dec);
        const uint64_t owner_generation = ++motion_generation_;
        ilock.unlock();
        if (!wait_for_slew_complete(lock, owner_generation)) {
            // Another client's AbortSlew / Park / MoveAxis / FindHome / slew took the
            // mount: skip the completion tail so it cannot write T2 over their motion.
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
        restore_tracking_after_slew_locked();
        learn_ra_offset_locked(ra);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see park()
        {
            // Gate BEFORE cancelling: cancelling the slew slot would end a park in
            // flight (clearing parking_) and let this slew clobber it.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            check_tracking_for_slew_locked("SlewToCoordinatesAsync");
        }
        uint32_t ra_raw = 0;
        uint32_t dec_raw = 0;
        bool precise = false;
        bool use_passthrough = false;
        bool do_flip = false;
        uint32_t flip_ra = 0, flip_dec = 0;
        // Cancel + join any previous slew task BEFORE publishing the new slew
        // state below (its tail would otherwise clear the new slew's flags).
        // Must run without mutex_ held: the previous task's tail takes mutex_.
        const bool displaced = slew_.running();
        cancel_async_tasks();
        bool prev_slewing = false;
        bool prev_at_home = false;
        bool prev_manual[2] = {false, false};
        auto prev_override = util::TaskClock::clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            clear_pulse_guiding_locked();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            check_slew_safety_locked("SlewToCoordinatesAsync");
            slew_aborted_ = false;
            ++motion_generation_;

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

            drop_poll_in_flight_locked();
            prev_slewing = slewing_cached_;
            prev_at_home = at_home_;
            prev_manual[0] = manual_axis_slewing_[0];
            prev_manual[1] = manual_axis_slewing_[1];
            prev_override = position_override_until_;
            set_slewing_locked(true);
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that retries a rejected goto must not be told the OLD goto
            // failed.
            clear_motion_errors_locked();
            slew_force_until_ = clock_.now() + std::chrono::seconds(8);
            position_override_until_ = util::TaskClock::clock::time_point::min();
            flip_in_progress_ = do_flip;
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
        }

        double slew_target_ra = ra;
        start_slew_body(
            [this, ra_raw, dec_raw, precise, use_passthrough, do_flip, flip_ra, flip_dec,
             slew_target_ra](util::OperationContext& ctx) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
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
                        // open-astro#575: a reap by a newer initiator or AbortSlew is not a
                        // failure -- that operation owns clearing/replacing last_slew_error_.
                        if (ctx.stop_reason() == util::StopReason::None) {
                            fail_slew_locked(std::string("SlewToCoordinatesAsync dispatch failed: ") + ex.what(), ctx);
                        }
                        return;
                    } catch (...) {
                        if (ctx.stop_reason() == util::StopReason::None) {
                            fail_slew_locked("SlewToCoordinatesAsync dispatch failed: unknown exception", ctx);
                        }
                        return;
                    }
                }
                // Poll for slew completion WITHOUT holding the mutex so that
                // HTTP endpoints (Slewing, RightAscension, etc.) remain responsive.
                auto timeout = std::chrono::seconds(120);
                auto start = clock_.now();
                auto start_grace = std::chrono::seconds(2);
                bool saw_slewing = false;
                int completion_poll_failures = 0;
                bool use_axis_poll = do_flip || use_passthrough;
                auto& protocol = CelestronProtocolWrapper::instance();
                while (true) {
                    if (!ctx.wait_for(std::chrono::milliseconds(250))) {
                        // Cancelled (AbortSlew, disconnect / destruction, or a superseding motion):
                        // exit without touching driver state.
                        return;
                    }
                    bool still_slewing = false;
                    try {
                        if (hc_available_ && !use_axis_poll) {
                            still_slewing = protocol.is_goto_in_progress();
                        } else {
                            still_slewing = !protocol.is_slew_done(0) || !protocol.is_slew_done(1);
                        }
                    } catch (const std::exception& ex) {
                        if (ctx.stop_reason() == util::StopReason::None) {
                            const int failures = ++completion_poll_failures;
                            if (failures < kLinkFailureThreshold) {
                                ALPACA_LOG_WARN("Celestron",
                                                "Slew completion poll failed (" + std::to_string(failures) + "/" +
                                                    std::to_string(kLinkFailureThreshold) + "): " + ex.what());
                                continue;
                            }
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                                fail_slew_locked("Slew completion polling failed after " + std::to_string(failures) +
                                                     " consecutive failures: " + ex.what(),
                                                 ctx);
                            }
                        }
                        return;
                    } catch (...) {
                        if (ctx.stop_reason() == util::StopReason::None) {
                            const int failures = ++completion_poll_failures;
                            if (failures < kLinkFailureThreshold) {
                                ALPACA_LOG_WARN("Celestron", "Slew completion poll failed (" +
                                                                 std::to_string(failures) + "/" +
                                                                 std::to_string(kLinkFailureThreshold) +
                                                                 "): " + "unknown exception");
                                continue;
                            }
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                                fail_slew_locked("Slew completion polling failed after " + std::to_string(failures) +
                                                     " consecutive failures: unknown exception",
                                                 ctx);
                            }
                        }
                        return;
                    }
                    completion_poll_failures = 0;
                    if (still_slewing) {
                        saw_slewing = true;
                    }
                    if (!still_slewing) {
                        if (!saw_slewing && (clock_.now() - start) < start_grace) {
                            continue;
                        }
                        break;
                    }
                    if (clock_.now() - start > timeout) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                            fail_slew_locked("Slew timed out after 120s", ctx);
                        }
                        return;
                    }
                }
                if (slew_settle_time_seconds_ > 0) {
                    // Settle without holding the mutex (a settle of many seconds
                    // must not block GETs); bail out if cancelled meanwhile.
                    if (!ctx.wait_for(std::chrono::seconds(slew_settle_time_seconds_))) {
                        return;
                    }
                }
                // The whole async tail is guarded: a disconnect racing this thread
                // makes restore_tracking_after_slew_locked()/learn_ra_offset_locked()
                // throw NotConnected, and an exception escaping the thread lambda
                // is std::terminate.
                try {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                        return;
                    }
                    restore_tracking_after_slew_locked();
                    learn_ra_offset_locked(slew_target_ra);
                    set_slewing_locked(false);
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
                    flip_in_progress_ = false;
                    publish_measurement_locked();
                } catch (const std::exception& ex) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                        fail_slew_locked(std::string("Slew completion handling failed: ") + ex.what(), ctx);
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                        fail_slew_locked("Slew completion handling failed: unknown exception", ctx);
                    }
                }
            },
            displaced,
            [&] {
                set_slewing_locked(prev_slewing);
                at_home_ = prev_at_home;
                manual_axis_slewing_[0] = prev_manual[0];
                manual_axis_slewing_[1] = prev_manual[1];
                position_override_until_ = prev_override;
                slew_force_until_ = util::TaskClock::clock::time_point::min();
                flip_in_progress_ = false;
            });
    }

    void slew_to_target() override {
        // Its async sibling below has always required the pair; this one did
        // not, so SlewToTarget with nothing set slewed to whatever was in the
        // members -- 0h/0deg on a fresh connect. Found while splitting the
        // flag for open-astro#346: with one flag the omission was invisible,
        // since any target write at all made the check pass.
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
        target_ra_set_ = true;
        target_dec_set_ = true;
        // The mount now reports the synced position: write it through, and drop the poll in flight.
        snapshot_.write([ra, dec](PolledFrame& frame) {
            frame.ra_hours = ra;
            frame.dec_degrees = dec;
            frame.equatorial_known = true;
        });
        sync_completed_this_session_ = true;
        skip_next_ra_learn_ = true;
        ALPACA_LOG_WARN("Celestron",
                        "SyncToCoordinates completed — slew gate now OPEN for this session, "
                        "RA offset learning suspended for next slew");
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
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        bool was_parking = false;
        std::string stop_error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            parked_ = false;
            was_parking = parking_;
            if (was_parking) {
                // Unpark during a park wins the race: stop the park slew and
                // drop the parking state; the task below is then joined. A
                // failed stop leaves Slewing to report the stored error, since
                // the park slew may still be running.
                parking_ = false;
                stop_error = stop_park_slew_locked();
                if (stop_error.empty()) {
                    set_slewing_locked(false);
                }
                slew_force_until_ = util::TaskClock::clock::time_point::min();
                position_override_until_ = util::TaskClock::clock::time_point::min();
                flip_in_progress_ = false;
            }
            if (stop_error.empty()) {
                clear_motion_errors_locked();
            } else {
                last_home_error_.clear();
                last_slew_error_ = "Unpark could not stop the park slew: " + stop_error;
            }
        }
        if (was_parking) {
            slew_.cancel_all_and_join();  // without mutex_ held
        }
        if (!stop_error.empty()) {
            throw AlpacaException("Unpark could not stop the park slew: " + stop_error, AlpacaError::DriverException);
        }
    }

    bool get_can_move_axis(int axis) const override {
        if (axis < 0 || axis > 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("MoveAxis axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(rate)) {
            throw AlpacaException("MoveAxis rate must be finite", AlpacaError::InvalidValue);
        }
        if (std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("MoveAxis rate exceeds supported range", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("MoveAxis");
        }
        slew_.cancel_all_and_join();
        pulse_ops_[axis].cancel_all_and_join();
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("MoveAxis");
        ++motion_generation_;

        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;
        set_slewing_locked(false);
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        flip_in_progress_ = false;
        manual_axis_slewing_[axis] = moving;
        pulse_guide_active_[static_cast<std::size_t>(axis)] = false;
        pulse_guide_end_time_[static_cast<std::size_t>(axis)] = util::TaskClock::clock::time_point::min();
        if (moving) {
            parked_ = false;
            at_home_ = false;
        }
        CelestronProtocolWrapper::instance().move_axis_variable_rate(axis, moving ? rate : 0.0);
        clear_motion_errors_locked();
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            return {};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_fully_parked_locked("AbortSlew");  // AbortSlew may cancel a park in flight
            slew_.cancel();
        }
        try {
            // Stop both pulse chains before stopping hardware, so neither can
            // re-arm a guide pulse after the stop commands.
            pulse_ops_[0].cancel_all_and_join();
            pulse_ops_[1].cancel_all_and_join();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                check_connected();
                check_not_fully_parked_locked("AbortSlew");
                // The slew cancellation above prevents delayed dispatches. The
                // wrapper serializes transactions, so a GOTO already in flight
                // must finish (or time out) before these stops can reach the mount.
                auto& protocol = CelestronProtocolWrapper::instance();
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
                try_stop([&protocol]() { protocol.cancel_goto(); });
                try_stop([&protocol]() { protocol.move_axis_fixed_rate(0, 0); });
                try_stop([&protocol]() { protocol.move_axis_fixed_rate(1, 0); });
                if (!stop_error.empty()) {
                    last_home_error_.clear();
                    last_slew_error_ = "AbortSlew stop failed: " + stop_error;
                    // The mount may still be moving: leave Slewing and the slew state as they were.
                    throw AlpacaException("AbortSlew stop failed: " + stop_error, AlpacaError::DriverException);
                }
                // AbortSlew clears a stored motion error only after all stop
                // commands are acknowledged.
                clear_motion_errors_locked();
                pulse_guide_active_.fill(false);
                pulse_guide_end_time_.fill(util::TaskClock::clock::time_point::min());
                homing_ = false;
                parking_ = false;  // an aborted park never reaches AtPark
                set_slewing_locked(false);
                slew_aborted_ = true;
                ++motion_generation_;
                flip_in_progress_ = false;
                slew_force_until_ = util::TaskClock::clock::time_point::min();
                position_override_until_ = util::TaskClock::clock::time_point::min();
                manual_axis_slewing_[0] = false;
                manual_axis_slewing_[1] = false;
            }
        } catch (...) {
            slew_.cancel_all_and_join();
            throw;
        }
        slew_.cancel_all_and_join();
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
    // The disconnect body, also the first half of a relink. Caller holds mutex_.
    void teardown_session_locked() {
        CelestronProtocolWrapper::instance().disconnect();
        connected_ = false;
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        utc_offset_.clear();  // decision 0014: the written UTCDate lives until the next connect or disconnect
        target_ra_set_ = false;
        target_dec_set_ = false;
        parked_ = false;
        parking_ = false;
        at_home_ = false;
        homing_ = false;
        pulse_guide_active_.fill(false);
        pulse_guide_end_time_.fill(util::TaskClock::clock::time_point::min());
        set_slewing_locked(false);
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        sync_completed_this_session_ = false;
        skip_next_ra_learn_ = false;
        flip_in_progress_ = false;
        clear_motion_errors_locked();
        position_link_health_.reset();
        publish_link_fault_locked();
        snapshot_.reset();
        link_lost_ = false;
    }

    void check_connected() const {
        if (!connected_ || link_lost_.load()) {
            throw AlpacaException("Not connected to Celestron mount", AlpacaError::NotConnected);
        }
    }

    // ── Background bodies (async slew / park, pulse-guide chunk chain) ──
    // Each runs on a util::AsyncOperation slot (decision 0006): never
    // detached, cancelled and joined in the destructor and on disconnect so a
    // wakeup can never touch a destroyed or disconnected driver.

    // Cancel and join every slot. Must be called WITHOUT mutex_ held (the
    // slew body re-acquires mutex_ before finishing).
    void cancel_async_tasks() {
        slew_.cancel_all_and_join();
        pulse_ops_[0].cancel_all_and_join();
        pulse_ops_[1].cancel_all_and_join();
    }

    // Call without mutex_ held. Starts `body` on the slew slot. The initiator
    // cancelled and joined the previous body and published its flags before
    // this; when the slot refuses (no thread), `rollback_locked` undoes the
    // flags and the exception goes to the caller. `displaced` says a body was
    // in flight and has been cancelled: its motion may still be running with
    // nobody left to stop it, so the mount is stopped and the AbortSlew state
    // published (decision 0006, "Thread refusal"). A refusal that replaced
    // nothing cancels nothing.
    template <typename Rollback>
    void start_slew_body(std::function<void(util::OperationContext&)> body, bool displaced,
                         Rollback&& rollback_locked) {
        try {
            slew_.start(std::move(body));
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            rollback_locked();
            if (displaced && connected_) {
                stop_after_refused_start_locked();
            }
            throw;
        }
    }

    // AbortSlew's stop and published state, for a refused start that cancelled
    // a body in flight. A failed stop leaves Slewing as it was: the mount may
    // still be moving.
    void stop_after_refused_start_locked() {
        const std::string stop_error = stop_park_slew_locked();
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            ALPACA_LOG_ERROR("Celestron",
                             "stop after refused slew start failed: " + stop_error + "; the mount may still be moving");
        }
        homing_ = false;
        parking_ = false;
        slew_aborted_ = true;
        ++motion_generation_;
        flip_in_progress_ = false;
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
    }

public:
    void set_slew_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        slew_.set_spawn_for_testing(std::move(spawn));
    }

private:
    void clear_pulse_guiding_locked() {
        pulse_guide_active_.fill(false);
        pulse_guide_end_time_.fill(util::TaskClock::clock::time_point::min());
    }

    // A park in flight (parking_) gates the same members as a completed park:
    // otherwise a slew/MoveAxis/sync issued right after the async Park would
    // silently clobber it (and MoveAxis would jog an axis mid-GOTO). Only
    // AbortSlew and Unpark are allowed through, and both cancel the park.
    void check_not_parked_locked(const char* operation) const {
        if (parked_ || parking_) {
            throw AlpacaException(std::string(operation) + " is not allowed while " + (parked_ ? "parked" : "parking"),
                                  AlpacaError::InvalidWhileParked);
        }
    }

    void check_not_fully_parked_locked(const char* operation) const {
        if (parked_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    // ── Snapshot-served state (decision 0010, Celestron adoption) ──────────────

    struct SamplePlan {
        bool hc = true;            // the hand controller answers the HC commands
        bool precise = true;       // precise (24-bit) HC commands
        bool read_slewing = true;  // false while the control path owns the Slewing value
        bool hc_slewing = true;    // Slewing from the HC goto flag; false: the two MC axis flags
        bool full = false;         // alt/az, pier side and tracking mode too
    };

    // One mount sample, taken without mutex_ (the protocol wrapper serialises each exchange).
    struct Sample {
        std::optional<bool> slewing;
        double ra_hours = 0.0;
        double dec_degrees = 0.0;
        std::optional<std::pair<double, double>> altaz;  // {azimuth, altitude}
        std::optional<int> pier;                         // ASCOM value
        std::optional<int> tracking_mode;
    };

    // Throws what the protocol throws for the slewing flag or the equatorial pair; the slow fields keep their last
    // value when the mount does not answer them.
    Sample sample_raw(const SamplePlan& plan) const {
        auto& protocol = CelestronProtocolWrapper::instance();
        Sample sample;
        if (plan.read_slewing) {
            if (plan.hc_slewing) {
                sample.slewing = protocol.is_goto_in_progress();
            } else {
                const bool ra_done = protocol.is_slew_done(0);
                const bool dec_done = protocol.is_slew_done(1);
                sample.slewing = !ra_done || !dec_done;
            }
        }
        if (plan.hc) {
            const int bits = plan.precise ? 24 : 16;
            const auto raw = protocol.get_ra_dec_raw(plan.precise);
            sample.ra_hours = decode_ra_hours(raw.first, bits);
            sample.dec_degrees = decode_angle(raw.second, bits);
        } else {
            const uint32_t ra_raw = protocol.get_mc_position(0);
            const uint32_t dec_raw = protocol.get_mc_position(1);
            sample.ra_hours = decode_ra_hours(ra_raw, 24);
            sample.dec_degrees = decode_angle(dec_raw, 24);
        }
        if (plan.full) {
            // The slow fields stop at the first failure: on a dead link each would wait out its own response
            // timeout, and a Disconnect waiting on this tick would pass the ~5 s client budget.
            bool slow_ok = true;
            try {
                const int bits = plan.precise ? 24 : 16;
                const auto raw = protocol.get_alt_az_raw(plan.precise);
                sample.altaz = {wrap_degrees(decode_angle(raw.first, bits)), decode_angle(raw.second, bits)};
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
                slow_ok = false;
            }
            if (plan.hc && slow_ok) {
                try {
                    const char side = protocol.get_pier_side();
                    sample.pier = side == 'W' ? 0 : (side == 'E' ? 1 : -1);  // ASCOM: 0 = pierEast, 1 = pierWest
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
                    slow_ok = false;
                }
            }
            if (slow_ok) {
                try {
                    sample.tracking_mode = protocol.get_tracking_mode();
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
                }
            }
        }
        return sample;
    }

    // The frame a sample makes, over the previous one (a field the sample lacks keeps its value).
    static PolledFrame decode_sample(const Sample& sample, const PolledFrame& previous) {
        PolledFrame frame = previous;
        if (sample.slewing) {
            frame.slewing = *sample.slewing;
            frame.slewing_known = true;
        }
        frame.ra_hours = sample.ra_hours;
        frame.dec_degrees = sample.dec_degrees;
        frame.equatorial_known = true;
        if (sample.altaz) {
            frame.az_degrees = sample.altaz->first;
            frame.alt_degrees = sample.altaz->second;
            frame.altaz_known = true;
        }
        if (sample.pier) {
            frame.side_of_pier = *sample.pier;  // -1 (pierUnknown) is an answer too
            frame.pier_known = true;
        }
        if (sample.tracking_mode) {
            frame.tracking_mode = *sample.tracking_mode;
        }
        return frame;
    }

    SamplePlan sample_plan_locked(bool full) const {
        SamplePlan plan;
        plan.hc = hc_available_;
        plan.precise = use_precise_commands_;
        plan.hc_slewing = hc_available_ && !flip_in_progress_;
        plan.full = full;
        return plan;
    }

    // The frame a getter serves, or the refusal that applies: a lost link (NotConnected), a latched fault
    // (DriverException "communications compromised", Connected stays true, decision 0009 point 1), no frame yet.
    // A frame past kSnapshotMaxAge is served as it is: stale is not lost.
    util::StateSnapshot<PolledFrame>::Reading served_frame_locked() const {
        check_connected();
        if (position_link_health_.faulted()) {
            throw_position_link_fault_locked();
        }
        auto reading = snapshot_.read();
        if (!reading) {
            throw AlpacaException(
                "Celestron mount state is not available yet: the first status poll has not been answered",
                AlpacaError::DriverException);
        }
        return *reading;
    }

    // {RA hours, Dec degrees}
    std::pair<double, double> served_equatorial_locked() const {
        const auto frame = served_frame_locked();
        if (!frame.value.equatorial_known) {
            throw AlpacaException("Celestron mount position is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return {frame.value.ra_hours, frame.value.dec_degrees};
    }

    // {azimuth, altitude}
    std::pair<double, double> served_altaz_locked() const {
        const auto frame = served_frame_locked();
        if (!frame.value.altaz_known) {
            throw AlpacaException("Celestron mount alt/az is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return {frame.value.az_degrees, frame.value.alt_degrees};
    }

    // The handset tracking mode. `allow_read` (the control path) asks the mount once when no poll has answered the
    // tracking query yet; the getter refuses instead.
    int tracking_mode_locked(bool allow_read) const {
        const auto frame = served_frame_locked();
        if (frame.value.tracking_mode >= 0) {
            return frame.value.tracking_mode;
        }
        if (!allow_read) {
            throw AlpacaException("Celestron mount tracking state is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        const int mode = CelestronProtocolWrapper::instance().get_tracking_mode();
        snapshot_.write([mode](PolledFrame& held) { held.tracking_mode = mode; });
        return mode;
    }

    // The age of the state DeviceState serves: how long ago the poll measured it, on the task clock. Takes only the
    // snapshot's own mutex.
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        const auto reading = snapshot_.read();
        if (!reading) {
            return std::nullopt;
        }
        return clock_.now() - reading->measured_at;
    }

    // A write that changed what the mount's position means (a slew start, a pulse, a sync): drops the poll in
    // flight so its older sample cannot overwrite what the control path knows. The next poll publishes.
    void drop_poll_in_flight_locked() const {
        snapshot_.write([](PolledFrame&) {});
    }

    // The tracking mode the control path just commanded, written through so the next read sees it before the
    // next poll runs.
    void set_tracking_mode_cached_locked(int mode) const {
        snapshot_.write([mode](PolledFrame& frame) { frame.tracking_mode = mode; });
    }

    // The slewing value the control path just decided (a slew started or ended, an abort), written through.
    void set_slewing_locked(bool slewing) const {
        slewing_cached_ = slewing;
        snapshot_.write([slewing](PolledFrame& frame) {
            frame.slewing = slewing;
            frame.slewing_known = true;
        });
    }

    // A re-read of the mount's slewing flag (the slew-complete and Park loops). Writes only a change: every write
    // drops the poll tick in flight, so a same-value write on each re-read would drop nearly every tick of a GOTO
    // and freeze the position.
    void note_slewing_read_locked(bool slewing) const {
        const auto held = snapshot_.read();
        if (held && held->value.slewing_known && held->value.slewing == slewing) {
            slewing_cached_ = slewing;
            return;
        }
        set_slewing_locked(slewing);
    }

    // Where the control path ends a motion (slew landed, park complete, home done): reads the mount now and
    // publishes a full frame, so Slewing false is never served beside a mid-motion position. A failed read leaves
    // the position unknown, which the getters refuse until the next poll answers.
    void publish_measurement_locked() const {
        const auto token = snapshot_.begin_poll();
        try {
            SamplePlan plan = sample_plan_locked(true);
            plan.read_slewing = false;
            const Sample sample = sample_raw(plan);
            const auto previous = snapshot_.read();
            snapshot_.publish(token, decode_sample(sample, previous ? previous->value : PolledFrame{}));
            drop_poll_in_flight_locked();  // a poll that sampled before this read must not replace it
            note_position_reply_locked();
        } catch (const std::exception& e) {
            snapshot_.write([](PolledFrame& frame) {
                frame.equatorial_known = false;
                frame.altaz_known = false;
            });
            note_position_failure_locked(e);
        }
    }

    // The first frame, at the end of a connect with mutex_ held, so the getters have an answer the moment Connect
    // returns. Best effort: a mount that does not answer the slewing query still gives its position, and one that
    // answers nothing leaves a frame with nothing known; the poll fills it in.
    // The seed's failures do not feed position_link_health_: the status poll that follows judges the link.
    void seed_snapshot_locked() {
        const auto token = snapshot_.begin_poll();
        PolledFrame frame;
        SamplePlan plan = sample_plan_locked(true);
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                frame = decode_sample(sample_raw(plan), frame);
                break;
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): retried without the slewing query
            }
            plan.read_slewing = false;
        }
        snapshot_.publish(token, frame);
    }

    // Control path: one direct read of the equatorial pair, for an operation that needs the mount's position now
    // (SetPark, Park without a park position, the RA offset learning). {RA hours, Dec degrees}.
    std::pair<double, double> measure_equatorial_locked() const {
        try {
            const auto sample = sample_raw([this] {
                SamplePlan plan = sample_plan_locked(false);
                plan.read_slewing = false;
                return plan;
            }());
            note_position_reply_locked();
            return {sample.ra_hours, sample.dec_degrees};
        } catch (const std::exception& e) {
            note_position_failure_locked(e);
            if (position_link_health_.faulted()) {
                throw_position_link_fault_locked();
            }
            throw;
        }
    }

    // Decision 0009 point 2(b): a fault that has stood past the bound is a LOST link. Connected reads false, the
    // getters and setters throw NotConnected, DeviceState is empty, the transport is closed; only the client's
    // Connected=true relinks. Caller holds mutex_. True when the link was just declared lost.
    bool declare_link_lost_if_stale_locked() {
        if (!position_link_health_.fault_stale(clock_.now())) {
            return false;
        }
        ALPACA_LOG_ERROR(
            "Celestron",
            "Link to the mount lost after " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
                " s without a reply; reconnect to resume (" + position_link_health_.fault() + ")");
        link_lost_ = true;
        CelestronProtocolWrapper::instance().disconnect();
        return true;
    }

    // True when the unread site (a connect that could not reach it) is due another attempt of the poll
    // (kSitePollRetryDelay). Like the sample, its exchange runs without mutex_.
    bool site_read_due_locked() {
        const auto now = clock_.now();
        if (last_site_info_attempt_ != util::TaskClock::clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSitePollRetryDelay) {
            return false;
        }
        last_site_info_attempt_ = now;
        return true;
    }

    // One poll tick. The site read and the sample exchanges run WITHOUT mutex_, so a getter never waits behind a
    // slow link and the snapshot mutex is never held across I/O; the frame is decoded and published under mutex_.
    // The exception is a FindHome in flight: its completion check (check_homing_complete_locked) runs under mutex_,
    // so on a dead link during FindHome each tick holds mutex_ for one exchange timeout and the getters wait. Returns
    // false when the connection is gone or lost and the poll body should end. `ctx` is null for the test seam's direct
    // tick, which also ignores the pause.
    bool poll_status_once(util::OperationContext* ctx, int tick) {
        const auto stopped = [ctx] { return ctx != nullptr && ctx->stop_reason() != util::StopReason::None; };
        // Held for the whole tick of the poll body, so pausing it can wait out a tick already in flight.
        std::unique_lock<std::mutex> tick_lock(status_poll_tick_mutex_, std::defer_lock);
        if (ctx != nullptr) {
            tick_lock.lock();
        }
        if (ctx != nullptr && status_poll_paused_for_testing_.load()) {
            return true;
        }
        SamplePlan plan;
        bool read_site = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || link_lost_.load() || stopped()) {
                return false;
            }
            if (homing_) {
                check_homing_complete_locked();  // a FindHome in flight is completed here, not by a getter
                if (home_status_poll_failures_ != 0) {
                    return true;  // it timed out: no second timeout in this tick (Disconnect waits for the tick)
                }
            }
            plan = sample_plan_locked(tick % kSlowPollEvery == 0);
            // The control path owns the Slewing value while it moves the mount itself.
            plan.read_slewing = !(parking_ || slew_.running() || homing_ || manual_axis_slewing_[0] ||
                                  manual_axis_slewing_[1] || clock_.now() < slew_force_until_);
            read_site = !site_info_valid_ && plan.full && site_read_due_locked();
        }
        if (read_site) {
            std::optional<LocationInfo> site;
            try {
                site = CelestronProtocolWrapper::instance().get_location();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): retried after kSiteInfoRetryDelay
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || stopped()) {
                return false;
            }
            if (!site) {
                return true;  // it timed out: no second timeout in this tick; the next tick samples
            }
            // A client's SiteLatitude/SiteLongitude write that landed during the read wins over the older value.
            if (!site_info_valid_) {
                site_latitude_cached_ = site->latitude_degrees;
                site_longitude_cached_ = site->longitude_degrees;
                site_info_valid_ = true;
            }
        }
        const auto token = snapshot_.begin_poll();
        Sample sample;
        try {
            sample = sample_raw(plan);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || stopped()) {
                return false;
            }
            note_position_failure_locked(e);
            return !declare_link_lost_if_stale_locked();
        }
        run_status_poll_after_sample_hook();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || link_lost_.load() || stopped()) {
            return false;
        }
        const auto previous = snapshot_.read();
        snapshot_.publish(token, decode_sample(sample, previous ? previous->value : PolledFrame{}));
        note_position_reply_locked();  // a dropped frame (a write landed since the sample) was still a reply
        return true;
    }

    void run_status_poll_after_sample_hook() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
            hook = status_poll_after_sample_hook_;
        }
        if (hook) {
            hook();
        }
    }

    void run_status_poller(util::OperationContext& ctx) {
        for (int tick = 0;; ++tick) {
            const auto started = clock_.now();
            if (!poll_status_once(&ctx, tick)) {
                return;
            }
            // Start to start: wait for the next tick, skipping any a slow poll ran over (no catch-up).
            const auto spent = clock_.now() - started;
            const auto ticks = spent / kStatusPollPeriod + 1;
            if (!ctx.wait_for(ticks * kStatusPollPeriod - spent)) {
                return;
            }
        }
    }

    // Call without mutex_ held: the poll body takes mutex_.
    void stop_status_poller() { status_poll_.cancel_all_and_join(); }

    // Call without mutex_ held (AsyncOperation rule 10), after a connect.
    void start_status_poller() {
        try {
            status_poll_.start([this](util::OperationContext& ctx) { run_status_poller(ctx); });
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR("Celestron", std::string("Status poll did not start: ") + e.what());
        }
    }

public:
    // Returns once no poll-body tick is in flight, so a case that pauses the poll then counts mount transactions
    // without a tick started before the pause adding to them. The pause survives a relink: set_connected(true) does
    // not clear it, so a case that reconnects unpauses before it expects the body to tick.
    void set_status_poll_paused_for_testing(bool paused) {
        status_poll_paused_for_testing_ = paused;
        std::lock_guard<std::mutex> drain(status_poll_tick_mutex_);
    }

    bool poll_status_now_for_testing() { return poll_status_once(nullptr, 0); }

    void set_status_poll_after_sample_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
        status_poll_after_sample_hook_ = std::move(hook);
    }

private:
    void note_position_reply_locked() const {
        if (position_link_health_.on_reply()) {
            ALPACA_LOG_INFO("Celestron", "Position link recovered; mount readback is available again");
        }
        publish_link_fault_locked();
    }

    // Copies the latched fault text to the narrow-mutex copy get_link_fault() reads.
    // Call under mutex_ after every change to position_link_health_.
    void publish_link_fault_locked() const {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        link_fault_text_ = position_link_health_.fault();
    }

    void note_position_failure_locked(const std::exception& e) const {
        if (auto fault = position_link_health_.note_failure(e.what(), kLinkFailureThreshold, clock_.now())) {
            ALPACA_LOG_ERROR("Celestron", "Position link faulted: " + *fault);
        } else {
            ALPACA_LOG_WARN("Celestron", "Position read failed (" +
                                             std::to_string(position_link_health_.consecutive_failures()) +
                                             " consecutive failures): " + e.what());
        }
        publish_link_fault_locked();
    }

    [[noreturn]] void throw_position_link_fault_locked() const {
        throw AlpacaException("Celestron mount communications compromised: " + position_link_health_.fault(),
                              AlpacaError::DriverException);
    }

    // `fresh` is the control path (the slew-complete loops): it asks the mount. The getter serves the snapshot.
    bool get_slewing_locked(bool fresh = false) const {
        if (!last_slew_error_.empty()) {
            throw AlpacaException(last_slew_error_);
        }
        // A park in flight reports Slewing until the park task flips AtPark
        // (same locked step) — never Slewing false with AtPark false.
        if (parking_) {
            return true;
        }
        // The async body owns its completion polling; a getter serves its
        // published state instead of competing for a second wire read.
        if (slew_.running()) {
            return slewing_cached_;
        }
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1] || clock_.now() < slew_force_until_ || homing_) {
            return true;  // homing: the status poll checks for completion
        }
        if (fresh) {
            return poll_hardware_slewing_locked();
        }
        const auto reading = served_frame_locked();
        if (!reading.value.slewing_known) {
            // The connect-time query failed; no guess is served until a poll (or the control path) answers.
            throw AlpacaException("Celestron mount state is not available yet: the slewing flag has not been answered",
                                  AlpacaError::DriverException);
        }
        return reading.value.slewing;
    }

    bool poll_hardware_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (clock_.now() < slew_force_until_) {
            return true;
        }
        if (homing_) {
            check_homing_complete_locked();
            if (!last_slew_error_.empty()) {
                throw AlpacaException(last_slew_error_);
            }
            return homing_; // still homing = still slewing
        }
        bool was_slewing = slewing_cached_;
        try {
            auto& protocol = CelestronProtocolWrapper::instance();
            bool hardware_slewing = false;
            if (hc_available_ && !flip_in_progress_) {
                hardware_slewing = protocol.is_goto_in_progress();
            } else {
                bool ra_done = protocol.is_slew_done(0);
                bool dec_done = protocol.is_slew_done(1);
                hardware_slewing = !ra_done || !dec_done;
            }
            note_slewing_read_locked(hardware_slewing);
            slew_status_poll_failures_ = 0;
        } catch (const std::exception& ex) {
            const int failures = ++slew_status_poll_failures_;
            if (failures < kLinkFailureThreshold) {
                ALPACA_LOG_WARN("Celestron", "Slew status poll failed (" + std::to_string(failures) + "/" +
                                                 std::to_string(kLinkFailureThreshold) + "): " + ex.what());
                return slewing_cached_;
            }
            const std::string message = "Slew completion polling failed after " + std::to_string(failures) +
                                        " consecutive failures: " + ex.what();
            if (parking_) throw AlpacaException(message, AlpacaError::DriverException);
            if (slewing_cached_) fail_slew_poll_locked(message);
            throw AlpacaException(last_slew_error_.empty() ? message : last_slew_error_, AlpacaError::DriverException);
        } catch (...) {
            const int failures = ++slew_status_poll_failures_;
            if (failures < kLinkFailureThreshold) {
                ALPACA_LOG_WARN("Celestron", "Slew status poll failed (" + std::to_string(failures) + "/" +
                                                 std::to_string(kLinkFailureThreshold) + "): unknown exception");
                return slewing_cached_;
            }
            const std::string message = "Slew completion polling failed after " + std::to_string(failures) +
                                        " consecutive failures: unknown exception";
            if (parking_) throw AlpacaException(message, AlpacaError::DriverException);
            if (slewing_cached_) fail_slew_poll_locked(message);
            throw AlpacaException(last_slew_error_.empty() ? message : last_slew_error_, AlpacaError::DriverException);
        }
        if (was_slewing && !slewing_cached_) {
            flip_in_progress_ = false;
            publish_measurement_locked();
        }
        return slewing_cached_;
    }

    void check_homing_complete_locked() const {
        try {
            auto& protocol = CelestronProtocolWrapper::instance();
            bool ra_done = protocol.is_level_done(0);
            bool dec_done = protocol.is_level_done(1);
            home_status_poll_failures_ = 0;
            if (ra_done && dec_done) {
                homing_ = false;
                at_home_ = true;
                set_slewing_locked(false);
                publish_measurement_locked();
            }
        } catch (const std::exception& ex) {
            const int failures = ++home_status_poll_failures_;
            if (failures < kLinkFailureThreshold) {
                ALPACA_LOG_WARN("Celestron", "FindHome status poll failed (" + std::to_string(failures) + "/" +
                                                 std::to_string(kLinkFailureThreshold) + "): " + ex.what());
                return;
            }
            fail_home_locked("FindHome completion polling failed after " + std::to_string(failures) +
                             " consecutive failures: " + ex.what());
        } catch (...) {
            const int failures = ++home_status_poll_failures_;
            if (failures < kLinkFailureThreshold) {
                ALPACA_LOG_WARN("Celestron", "FindHome status poll failed (" + std::to_string(failures) + "/" +
                                                 std::to_string(kLinkFailureThreshold) + "): unknown exception");
                return;
            }
            fail_home_locked("FindHome completion polling failed after " + std::to_string(failures) +
                             " consecutive failures: unknown exception");
        }
    }

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). Read as the Tracking getter reads it.
    void check_tracking_for_slew_locked(const char* who) const {
        if (!get_tracking_locked(true)) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // The Tracking getter reads the snapshot only. The control path (a slew's Tracking gate, the tracking
    // restore) asks the mount once when no poll has answered the tracking query yet.
    bool get_tracking_locked(bool allow_read = false) const { return tracking_mode_locked(allow_read) != 0; }

    AlignmentMode alignment_mode_locked() const {
        switch (mount_model_id_) {
            case 5:   // CGE
            case 6:   // Advanced GT
            case 13:  // CGE Pro
            case 14:  // CGEM DX
            case 20:  // Advanced VX
            case 23:  // CGX
            case 24:  // CGX-L
                return AlignmentMode::GermanPolar;
            case 1:   // NexStar GPS
            case 3:   // NexStar i-Series
            case 4:   // NexStar i-Series SE
            case 7:   // SLT
            case 9:   // CPC
            case 10:  // NexStar GT
            case 11:  // NexStar 4/5 SE
            case 12:  // NexStar 6/8 SE
            case 15:  // LCM
            case 16:  // Sky Prodigy
            case 17:  // CPC Deluxe
            case 18:  // GT 16
            case 19:  // StarSeeker
            case 21:  // Cosmos
            case 22:  // NexStar Evolution
            case 25:  // Astro Fi
                // Nominally Alt-Az, but a fork can sit on a wedge; the handset
                // does not say, so the device config's alignmentMode can (#860).
                return alignment_setting_ == CelestronAlignmentSetting::Equatorial ? AlignmentMode::Polar
                                                                                   : AlignmentMode::AltAz;
            default:
                throw AlpacaException(
                    "Celestron cannot determine AlignmentMode for mount model ID " + std::to_string(mount_model_id_),
                    AlpacaError::DriverException);
        }
    }

    int tracking_mode_for_alignment_locked(AlignmentMode alignment) const {
        if (alignment == AlignmentMode::AltAz) {
            return 1;
        }
        const bool south = site_latitude_cached_ < 0.0;
        if (mount_model_id_ == 5 || mount_model_id_ == 6) {
            // CGE / Advanced GT HC firmware 3.01-3.04 uses 1=EQ north and
            // 2=EQ south; later firmware uses the standard 2/3 assignments.
            if (mount_firmware_version_.empty() || mount_firmware_version_ == "0.0") {
                throw AlpacaException("Celestron handset firmware is unavailable; cannot select EQ tracking mode",
                                      AlpacaError::DriverException);
            }
            const std::string_view firmware = mount_firmware_version_;
            const auto separator = firmware.find('.');
            if (separator == std::string_view::npos) {
                throw AlpacaException("Cannot parse Celestron handset firmware; cannot select EQ tracking mode",
                                      AlpacaError::DriverException);
            }
            int major = 0;
            int minor = 0;
            const auto major_text = firmware.substr(0, separator);
            const auto minor_text = firmware.substr(separator + 1);
            const auto major_result = std::from_chars(major_text.data(), major_text.data() + major_text.size(), major);
            const auto minor_result = std::from_chars(minor_text.data(), minor_text.data() + minor_text.size(), minor);
            if (major_result.ec != std::errc{} || major_result.ptr != major_text.data() + major_text.size() ||
                minor_result.ec != std::errc{} || minor_result.ptr != minor_text.data() + minor_text.size()) {
                throw AlpacaException("Cannot parse Celestron handset firmware; cannot select EQ tracking mode",
                                      AlpacaError::DriverException);
            }
            if (major == 3 && minor >= 1 && minor <= 4) {
                return south ? 2 : 1;
            }
        }
        return south ? 3 : 2;
    }

    void ensure_site_info_cached_locked() const {
        if (!connected_) {
            return;
        }
        auto now = clock_.now();
        if (!site_info_valid_ && last_site_info_attempt_ != util::TaskClock::clock::time_point::min() &&
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
        if (!get_tracking_locked(true)) {
            return;
        }
        auto& protocol = CelestronProtocolWrapper::instance();
        int mode = tracking_mode_locked(true);
        if (mode <= 0) {
            mode = 2;  // EQ-North default
        }
        protocol.set_tracking_mode(mode);
        set_tracking_mode_cached_locked(mode);
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
        const double measured_ra = measure_equatorial_locked().first;
        double error = shortest_ra_delta_hours(target_ra, measured_ra);
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
            // ensure_site_info_cached_locked() sets site_info_valid_ on success;
            // this re-check detects a failed cache populate, not a dead condition.
            // cppcheck-suppress identicalInnerCondition
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

        drop_poll_in_flight_locked();
        set_slewing_locked(true);
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that retries a rejected goto (even via the blocking SlewToCoordinates)
        // must not be told the OLD goto failed.
        clear_motion_errors_locked();
        slew_force_until_ = clock_.now() + std::chrono::seconds(8);
        position_override_until_ = util::TaskClock::clock::time_point::min();

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
        target_ra_set_ = true;
        target_dec_set_ = true;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parked_ = false;
        at_home_ = false;
        homing_ = false;
    }

    // Poll for slew completion, RELEASING the driver mutex around every sleep
    // so a sync slew/park doesn't block all GETs and disconnect for up to
    // 120 s (Bisque's unlock/sleep/relock loop is the reference pattern).
    // `lock` must be held on entry; it is held again on return/throw. The
    // connection state is re-checked after each relock.
    // Returns false when a concurrent motion command bumped motion_generation_
    // past `owner_generation` while the lock was released (the slew was superseded).
    bool wait_for_slew_complete(std::unique_lock<std::mutex>& lock, uint64_t owner_generation) const {
        const auto timeout = std::chrono::seconds(120);
        auto start = clock_.now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            clock_.sleep_for(d);
            lock.lock();
            // The mount may have been disconnected while the lock was released.
            check_connected();
        };
        while (true) {
            if (motion_generation_ != owner_generation) {
                return false;
            }
            bool slewing = get_slewing_locked(true);
            if (slewing) {
                saw_slewing = true;
            }
            if (!slewing) {
                if (!saw_slewing && (clock_.now() - start) < start_grace) {
                    sleep_unlocked(std::chrono::milliseconds(200));
                    continue;
                }
                break;
            }
            if (clock_.now() - start > timeout) {
                throw AlpacaException("Slew timed out");
            }
            sleep_unlocked(std::chrono::milliseconds(250));
        }
        set_slewing_locked(false);
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        publish_measurement_locked();
        if (slew_settle_time_seconds_ > 0) {
            sleep_unlocked(std::chrono::seconds(slew_settle_time_seconds_));
        }
        return motion_generation_ == owner_generation;
    }

    void sync_mount_time_locked() {
        auto now_utc = std::chrono::system_clock::now();
        // mutex_ is already held here — call the _locked body, not the public
        // locking method (non-recursive mutex would self-deadlock).
        set_utc_date_locked(now_utc);
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

    static double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time,
                                                    double longitude_degrees) {
        using namespace std::chrono;
        double days_since_epoch = duration<double>(utc_time.time_since_epoch()).count() / 86400.0;
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
        if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
            throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
            throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
        }
    }

    int device_number_;
    ConnectionInfo connection_info_;
    // Set by the auto-detect factory; empty for an explicit port or host.
    // connection_resolved_ is true once a connect has run the resolver, so a
    // later connect retries that endpoint before scanning again (#659).
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    mutable std::mutex mutex_;
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect
    bool connected_;

    double target_ra_hours_;
    double target_dec_degrees_;
    double aperture_diameter_m_;
    double aperture_area_m2_;
    double focal_length_m_;
    mutable util::PolledLinkHealth position_link_health_;
    mutable std::mutex link_fault_mutex_;
    mutable std::string link_fault_text_;

    mutable double site_latitude_cached_;
    mutable double site_longitude_cached_;
    mutable bool site_info_valid_;
    mutable util::TaskClock::clock::time_point last_site_info_attempt_;
    double site_elevation_m_;
    mutable int timezone_offset_minutes_;
    mutable bool timezone_offset_valid_;
    mutable bool dst_observed_;
    // The client's UTCDate (or the mount/host seed) and the task-clock time since (decision 0014); guarded by mutex_.
    mutable util::UtcOffset utc_offset_;
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
    mutable bool parked_;
    mutable bool at_home_;
    mutable bool homing_ = false;
    mutable bool slewing_cached_ = false;
    // open-astro#575: an async slew that fails AFTER slew_to_coordinates_async()
    // returned used to be logged and forgotten, leaving Slewing read FALSE --
    // indistinguishable from a landed goto. Set (under mutex_) by the slew
    // task's catch block on a REAL failure (never on the task's own
    // cancellation -- a newer initiator or AbortSlew reaped it), cleared by
    // the next motion initiator, a successful AbortSlew, and connect/disconnect.
    // A failed AbortSlew replaces it with the failed-stop reason. Consulted by
    // get_slewing() before the cached bool.
    mutable std::string last_slew_error_;
    mutable std::string last_home_error_;  // FindHome's completion error, not every motion error.
    mutable int slew_status_poll_failures_ = 0;
    mutable int home_status_poll_failures_ = 0;
    mutable util::TaskClock::clock::time_point slew_force_until_;
    mutable util::TaskClock::clock::time_point position_override_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    std::string mount_firmware_version_;
    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so the
    // get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;
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
    const CelestronAlignmentSetting alignment_setting_;  // fork geometry from the device config (#860)
    GuideRate guide_rate_{};
    mutable std::array<bool, 2> pulse_guide_active_{false, false};
    mutable std::array<util::TaskClock::clock::time_point, 2> pulse_guide_end_time_{
        util::TaskClock::clock::time_point::min(), util::TaskClock::clock::time_point::min()};
    // open-astro#326: atomic, not a plain bool. set_connected() writes this
    // during the AUX-bus probe while get_can_pulse_guide(),
    // get_can_set_guide_rates(), get_guide_rate()/set_guide_rate() and
    // pulse_guide() all read it WITHOUT taking mutex_ -- a genuine data race
    // that ThreadSanitizer reports, found when the [stress] migration made the
    // Celestron operate callback reach pulse_guide() for the first time (until
    // then the callback aborted at the first throw and never got past
    // get_tracking()).
    //
    // Atomic rather than moving the reads under mutex_ deliberately: these are
    // capability getters a polling client calls, and mutex_ is held across the
    // whole blocking connect handshake, so locking them would park that client
    // for the duration -- the issue #130 shape (alpaca-http-conformance.instructions.md
    // states the rule for get_connected()).
    std::atomic<bool> has_autoguider_port_{false};

    mutable double pg_hold_ra_hours_ = 0.0;
    mutable bool pg_hold_ra_valid_ = false;
    mutable util::TaskClock::clock::time_point pg_hold_ra_until_;
    mutable double pg_hold_dec_degrees_ = 0.0;
    mutable bool pg_hold_dec_valid_ = false;
    mutable util::TaskClock::clock::time_point pg_hold_dec_until_;

    mutable double pg_ra_baseline_hours_ = 0.0;
    mutable double pg_ra_expected_delta_hours_ = 0.0;
    mutable bool pg_ra_correction_valid_ = false;
    mutable util::TaskClock::clock::time_point pg_ra_correction_until_;
    mutable double pg_dec_baseline_degrees_ = 0.0;
    mutable double pg_dec_expected_delta_degrees_ = 0.0;
    mutable bool pg_dec_correction_valid_ = false;
    mutable util::TaskClock::clock::time_point pg_dec_correction_until_;
    mutable bool sync_completed_this_session_ = false;
    bool has_ra_switch_ = false;
    bool has_dec_switch_ = false;
    bool has_gps_ = false;
    bool has_focuser_ = false;
    std::vector<CelestronProtocolWrapper::BusDevice> detected_devices_;
    bool park_position_set_ = false;
    mutable bool parking_ = false;  // park task in flight (Slewing true, AtPark false)
    double park_ra_hours_ = 0.0;
    double park_dec_degrees_ = 0.0;
    double ra_slew_offset_hours_ = 0.0005; // ~27" initial seed for CGX-L fw 7.18 goto tracking deficit
    int ra_offset_samples_ = 0;
    bool slew_aborted_ = false;
    // Bumped under mutex_ by every motion initiator; a sync slew waiting with the lock
    // released compares it to learn it was superseded (Sky-Watcher shape, #832).
    uint64_t motion_generation_ = 0;
    bool skip_next_ra_learn_ = false;
    mutable bool flip_in_progress_ = false;

    // Serializes motion handoffs: sync/async slews, park, FindHome, PulseGuide,
    // MoveAxis, AbortSlew, Unpark, and disconnect. Never taken under mutex_; a
    // blocking slew releases it before waiting for motion completion.
    std::mutex initiator_mutex_;
    // Declared last, initialised last; must outlive the driver (decision 0005).
    util::TaskClock& clock_;
    // Declared after clock_ (the slots take it) and last, so they are destroyed
    // (and join) before any member their bodies touch. The slew slot carries
    // slew, Park and the completion tails; each axis has its own pulse slot and
    // generation, so a pulse on one axis never supersedes the other's.
    util::OperationGeneration slew_generation_;
    util::AsyncOperation slew_{"Celestron slew", slew_generation_, clock_};
    util::OperationGeneration pulse_generation_[2];
    util::AsyncOperation pulse_ops_[2] = {{"Celestron pulse guide (RA)", pulse_generation_[0], clock_},
                                          {"Celestron pulse guide (Dec)", pulse_generation_[1], clock_}};
    // Decision 0010: the polled frame, read by the getters and DeviceState, and the poll body that fills it. The
    // snapshot is declared before the slot, so the slot (whose body publishes into it) is destroyed first.
    mutable util::StateSnapshot<PolledFrame> snapshot_{clock_, kSnapshotMaxAge};
    util::OperationGeneration poll_generation_;
    util::AsyncOperation status_poll_{"Celestron status poll", poll_generation_, clock_};
    std::atomic<bool> link_lost_{false};  // decision 0009: set under mutex_ by the poll body, cleared by a connect
    std::atomic<bool> status_poll_paused_for_testing_{false};
    std::mutex status_poll_tick_mutex_;  // held across one poll-body tick
    mutable std::mutex status_poll_hook_mutex_;
    std::function<void()> status_poll_after_sample_hook_;
};

std::unique_ptr<TelescopeDriver> create_celestron_telescope(
    int device_number,
    const ConnectionInfo& connection_info) {
    return create_celestron_telescope_with_site(device_number, connection_info,
                                                std::nullopt, std::nullopt,
                                                std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_celestron_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, CelestronAlignmentSetting alignment, util::TaskClock& clock) {
    return std::make_unique<CelestronTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                      site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                      alignment, util::ConnectionResolver<ConnectionInfo>{}, clock);
}

void set_slew_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_slew_spawn_for_testing(std::move(spawn));
    }
}

void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_status_poll_paused_for_testing(paused);
    }
}

bool poll_status_now_for_testing(TelescopeDriver& driver) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        return celestron->poll_status_now_for_testing();
    }
    return false;
}

void set_status_poll_after_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_status_poll_after_sample_hook_for_testing(std::move(hook));
    }
}

std::unique_ptr<TelescopeDriver> create_celestron_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
    CelestronAlignmentSetting alignment, util::TaskClock& clock) {
    if (!connection_resolver) {
        throw AlpacaException("Celestron telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<CelestronTelescopeDriver>(device_number, ConnectionInfo{}, site_latitude_deg,
                                                      site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                      alignment, std::move(connection_resolver), clock);
}

ConnectionInfo resolve_celestron_serial_auto(int mount_index) {
    auto ports = enumerate_celestron_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("Celestron NexStar mount"));
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
    return conn;
}

std::unique_ptr<TelescopeDriver> create_celestron_telescope_auto(
    int device_number, int mount_index, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, CelestronAlignmentSetting alignment, util::TaskClock& clock) {
    // The serial scan runs at connect time (#659), not here.
    return create_celestron_telescope_deferred(
        device_number, [mount_index] { return resolve_celestron_serial_auto(mount_index); }, site_latitude_deg,
        site_longitude_deg, site_elevation_m, sync_time_on_connect, alignment, clock);
}

} // namespace alpacacore::vendor::celestron

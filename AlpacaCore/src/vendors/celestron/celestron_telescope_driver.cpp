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
#include <alpacacore/util/gps_status.h>
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
#include <stdexcept>
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
constexpr int kMaxPulseGuideMs = 25500;
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
          site_elevation_m_(site_elevation_m),
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
        std::lock_guard<std::mutex> lock(mutex_);  // the connect publishes the model name under it, at its end
        if (!mount_model_name_.empty()) {
            return "Celestron " + mount_model_name_;
        }
        return "Celestron NexStar Telescope";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string fallback_unique_id() const override { return "Celestron_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "Celestron NexStar Mount Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore Celestron NexStar Driver v0.1";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Handset firmware captured at connect; surfaced in the web UI only. Guarded
    // by its OWN narrow firmware_mutex_, so a configureddevices poll never
    // blocks on the mount connection.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    // Lock-free (decision 0016 rule 3): both flags are atomics.
    bool get_connected() const override { return connected_.load() && !link_lost_.load(); }

    // Reads only the published copy under its own narrow mutex, so a configureddevices poll does not wait on the
    // mount connection.
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
        // transition_mutex_ spans the whole call and no getter takes it: the connect handshake runs under it and the
        // wrapper's per-link lock, never under mutex_ (decision 0016). Lock order: transition_mutex_,
        // initiator_mutex_, the wrapper's lock, mutex_ (leaf).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        std::unique_lock<std::mutex> ilock(initiator_mutex_, std::defer_lock);
        // Decision 0009 point 4: a lost link is relinked only by the client's Connected=true, which tears the dead
        // session down (slew bodies included) and connects afresh.
        const bool relink = connected && connected_.load() && link_lost_.load();
        if (!connected || relink) {
            ilock.lock();
            // Cancel + join the status poll and the background slew/pulse task
            // threads BEFORE closing: the task threads take mutex_ and send on the
            // shared wrapper, so leaving them running across the protocol
            // disconnect would race it.
            stop_status_poller();
            cancel_async_tasks();
        }
        auto& protocol = CelestronProtocolWrapper::instance();
        if (relink) {
            clear_session_state();
            protocol.disconnect();
        }
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++motion_generation_;
        }
        if (!connected) {
            // State first, then the close: it waits for an exchange in flight, so none sees a closed link, and
            // no getter waits for it.
            clear_session_state();
            protocol.disconnect();
            return;
        }
        try {
            connect_session(protocol);
        } catch (...) {
            // A throw after the open closes it again; connected_ was never set.
            try {
                protocol.disconnect();
            } catch (...) {  // NOLINT(bugprone-empty-catch): the connect's own error is the one to report
            }
            clear_session_state();
            throw;
        }
        start_status_poller();
    }

    // "gps_status": whether the mount's GPS module holds a link, with the module's own latitude and longitude
    // (issue #1277). Listed only when the bus probe found the GPS module (has_gps_) at connect.
    static constexpr std::string_view kGpsStatusAction = "gps_status";

    std::vector<std::string> get_supported_actions() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || link_lost_.load() || !has_gps_) return {};
        return {std::string(kGpsStatusAction)};
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;  // gps_status takes none
        if (!util::is_gps_status_action(action_name)) {
            throw AlpacaException("Action not supported: " + std::string(action_name),
                                  AlpacaError::ActionNotImplemented);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (!has_gps_) {
                throw AlpacaException("Action not supported: " + std::string(action_name),
                                      AlpacaError::ActionNotImplemented);
            }
        }
        // The pass-through exchanges run outside mutex_ (the wrapper serialises the wire).
        const auto reading = CelestronProtocolWrapper::instance().get_gps_reading();
        if (!reading.linked) return util::format_gps_status(false, std::nullopt, std::nullopt);
        return util::format_gps_status(true, reading.latitude_degrees, reading.longitude_degrees);
    }

    bool can_action(std::string_view action_name) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_ && !link_lost_.load() && has_gps_ && util::is_gps_status_action(action_name);
    }

    std::string command_blind(std::string_view command, bool raw) override {
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
        check_connected();
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

    // The device exchange runs under the wrapper's per-link lock only; mutex_ is taken to read state and again to
    // publish. transition_mutex_ keeps two setters' wire order and published order the same.
    void set_tracking(bool tracking) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        int mode = 0;
        if (tracking) {
            AlignmentMode alignment = AlignmentMode::AltAz;
            bool read_site = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                check_connected();
                alignment = alignment_mode_locked();
                read_site = alignment != AlignmentMode::AltAz && !site_info_valid_ && site_info_attempt_due_locked();
            }
            if (read_site) {
                read_site_info_unlocked();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                throw AlpacaException("Set SiteLatitude before enabling equatorial tracking", AlpacaError::ValueNotSet);
            }
            mode = tracking_mode_for_alignment_locked(alignment);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
        }
        CelestronProtocolWrapper::instance().set_tracking_mode(mode);
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            set_tracking_mode_cached_locked(mode);
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
        std::lock_guard<std::mutex> transition(transition_mutex_);
        check_connected();
        double ra_percent = (rate.ra / kSiderealDegPerSec) * 100.0;
        double dec_percent = (rate.dec / kSiderealDegPerSec) * 100.0;
        auto& protocol = CelestronProtocolWrapper::instance();
        protocol.set_autoguide_rate(0, ra_percent);
        protocol.set_autoguide_rate(1, dec_percent);
        std::lock_guard<std::mutex> lock(mutex_);
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
        check_connected();
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
        validate_ra_dec(ra, dec, "DestinationSideOfPier");
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

    // The driver applies no refraction correction, so it neither knows nor computes the flag (ITelescopeV4).
    bool get_does_refraction() const override {
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    void set_does_refraction(bool) override {
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_elevation_m_) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return *site_elevation_m_;
    }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> transition(transition_mutex_);
        LocationInfo info = current_location();
        info.latitude_degrees = latitude;
        CelestronProtocolWrapper::instance().set_location(info);
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> transition(transition_mutex_);
        LocationInfo info = current_location();
        info.longitude_degrees = longitude;
        CelestronProtocolWrapper::instance().set_location(info);
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        ensure_utc_cached(lock);
        if (const auto utc = utc_offset_.current(clock_.now())) {
            return *utc;
        }
        throw AlpacaException("The mount clock could not be read", AlpacaError::InvalidOperation);
    }

private:
    // What a UTCDate write sends and what it publishes once the mount has taken it.
    struct TimeWrite {
        TimeInfo info;
        LocalTimeInfo tz;
        std::chrono::system_clock::time_point utc;
        std::chrono::system_clock::duration client_minus_host{};
    };

    static TimeWrite prepare_time_write(std::chrono::system_clock::time_point utc) {
        TimeWrite write;
        write.utc = utc;
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);
        write.tz = compute_local_timezone_info(utc_time_t);

        std::tm local_tm {};
        local_tm = *std::localtime(&utc_time_t);

        write.info.hour = local_tm.tm_hour;
        write.info.minute = local_tm.tm_min;
        write.info.second = local_tm.tm_sec;
        write.info.month = local_tm.tm_mon + 1;
        write.info.day = local_tm.tm_mday;
        write.info.year = (local_tm.tm_year + 1900) % 100;
        write.info.timezone_offset_minutes = write.tz.offset_minutes;
        write.info.dst_enabled = write.tz.dst;
        write.client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        return write;
    }

    void publish_time_write_locked(const TimeWrite& write) {
        timezone_offset_minutes_ = write.tz.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = write.tz.dst;
        utc_offset_.set(write.utc, clock_.now());
        mount_clock_unreadable_ = false;
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("Celestron", write.client_minus_host,
                                                      client_disagreement_warned_);
    }

    // The mount's clock as one read of it gave it.
    struct MountClock {
        int timezone_offset_minutes = 0;
        bool dst = false;
        std::chrono::system_clock::time_point utc;
    };

    // The mount's clock, or nothing when it does not answer. Touches no driver state: the caller has mutex_ free.
    static std::optional<MountClock> read_mount_clock() {
        try {
            TimeInfo info = CelestronProtocolWrapper::instance().get_time();
            using namespace std::chrono;
            int year = 2000 + info.year;
            sys_days date = sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(info.month)} /
                                     std::chrono::day{static_cast<unsigned>(info.day)}};
            auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
            int total_offset = info.timezone_offset_minutes + (info.dst_enabled ? 60 : 0);
            return MountClock{info.timezone_offset_minutes, info.dst_enabled, local_time - minutes{total_offset}};
        } catch (...) {
            return std::nullopt;
        }
    }

    // Seeds the clock from the read. A mount that did not answer leaves it unset and remembers that: the host clock
    // is not the mount's, so UTCDate throws InvalidOperation rather than answer with it, and no getter asks again.
    void publish_mount_clock_locked(const std::optional<MountClock>& mount_clock) const {
        if (!mount_clock) {
            mount_clock_unreadable_ = true;
            return;
        }
        timezone_offset_minutes_ = mount_clock->timezone_offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = mount_clock->dst;
        utc_offset_.set(mount_clock->utc, clock_.now());
    }

    // The mount's clock, read once at connect so no getter reaches the mount for it; a connect-time read that failed
    // is remembered (mount_clock_unreadable_), so the getter throws without I/O. A getter that finds the clock unset
    // for another reason reads it with mutex_ free, and a UTCDate write that landed meanwhile wins.
    void ensure_utc_cached(std::unique_lock<std::mutex>& lock) const {
        if (utc_offset_.valid() || mount_clock_unreadable_) {
            return;
        }
        const auto mount_clock = without_lock(lock, [] { return read_mount_clock(); });
        if (!utc_offset_.valid()) {
            publish_mount_clock_locked(mount_clock);
        }
    }

public:
    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        check_connected();
        const TimeWrite write = prepare_time_write(utc);
        CelestronProtocolWrapper::instance().set_time(write.info);
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            publish_time_write_locked(write);
        }
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
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        clear_pulse_guiding_locked();
        check_not_parked_locked("FindHome");
        ++motion_generation_;

        // MC_LEVEL_START (0x0B) — moves each axis to its hardware home switch.
        // Asynchronous: returns immediately, client polls Slewing/AtHome. The initiator lock held here keeps any
        // other initiator out while mutex_ is free for the exchange.
        try {
            without_lock(lock, [] {
                auto& protocol = CelestronProtocolWrapper::instance();
                protocol.level_start(0);
                protocol.level_start(1);
            });
        } catch (const std::exception& ex) {
            fail_home(lock, std::string("FindHome dispatch failed: ") + ex.what());
            throw AlpacaException(last_slew_error_);
        } catch (...) {
            fail_home(lock, "FindHome dispatch failed: unknown exception");
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
        uint64_t superseded = 0;
        uint64_t park_generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            clear_pulse_guiding_locked();
            if (parked_ || parking_) {
                return;  // ASCOM: Park on a parked (or parking) mount is harmless.
            }
            if (!park_position_set_) {
                std::tie(park_ra_hours_, park_dec_degrees_) = measure_equatorial(lock);
                park_position_set_ = true;
            }
            park_ra = park_ra_hours_;
            park_dec = park_dec_degrees_;
            validate_ra_dec(park_ra, park_dec, "Park");
            // Run the slew-safety gate synchronously so an unaligned mount
            // still throws from Park (as the blocking implementation did)
            // rather than silently never reaching AtPark.
            check_slew_safety(lock, "Park");
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
            superseded = motion_generation_;
            park_generation = ++motion_generation_;
        }

        // Every hook runs with mutex_ held and releases it around its own exchange; whatever takes the park over
        // (AbortSlew, Unpark, a disconnect) bumps motion_generation_, which `superseded` reads after each hook.
        start_slew_body(
            [this, park_ra, park_dec, park_generation](util::OperationContext& ctx) {
                std::unique_lock<std::mutex> lock(mutex_);
                util::ParkSlewHooks hooks;
                hooks.still_parking = [this] { return connected_ && parking_; };
                hooks.superseded = [this, park_generation] { return motion_generation_ != park_generation; };
                hooks.abandon = [this, park_generation] {
                    if (motion_generation_ == park_generation) {
                        parking_ = false;
                    }
                };
                hooks.dispatch = [&] { do_slew_to_coordinates(lock, park_ra, park_dec); };
                hooks.poll_slewing = [&] { return poll_hardware_slewing(lock); };
                hooks.stop_tracking = [&] {
                    without_lock(lock, [] { CelestronProtocolWrapper::instance().set_tracking_mode(0); });
                    set_tracking_mode_cached_locked(0);
                };
                hooks.fail = [&](const std::string& message) { fail_park(lock, message, ctx); };
                hooks.complete = [&] {
                    // The position is read first; AtPark and Slewing then flip in one locked step, beside the
                    // position the mount stopped at.
                    const Measurement landed = measure_full(lock);
                    if (motion_generation_ != park_generation) {
                        return;  // taken over while the mount was read: the new owner has the state
                    }
                    publish_measurement_locked(landed);
                    set_slewing_locked(false);
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
                    flip_in_progress_ = false;
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
                keep_pending_stop_locked(superseded);  // the park goto never went out
            });
    }

    // The stop exchanges: cancel the GOTO, then both axes to rate 0. Each stop is tried on its own so one failure
    // does not skip the others. Returns the first failure's message, or an empty string when every stop was
    // answered (#742). The wrapper's per-link lock serialises them, so the caller has mutex_ free (decision 0016).
    static std::string stop_park_slew_wire() { return util::stop_goto_and_axes(CelestronProtocolWrapper::instance()); }

    void clear_motion_errors_locked() {
        last_slew_error_.clear();
        last_home_error_.clear();
        slew_status_poll_failures_ = 0;
        home_status_poll_failures_ = 0;
    }

    // The stops with mutex_ released; `lock` is held again on return.
    static std::string stop_park_slew(std::unique_lock<std::mutex>& lock) {
        return without_lock(lock, [] { return stop_park_slew_wire(); });
    }

    // The stops of a failure found by the status poll or the slew-complete wait, which no initiator owns: they go out
    // only while no newer initiator has taken the mount, so a client that saw the failure and sent a new motion
    // never has it cut short by a stale stop. try_lock, never a blocking lock: set_connected joins the poll while it
    // holds initiator_mutex_. A changed generation means a newer motion owns the mount and the stale stop is
    // dropped (Superseded). A busy lock means an initiator is dispatching but may not have taken the mount (a sync,
    // a pulse guide, a motion command that fails validation bump no generation): the stop stays pending (Busy) and
    // the poll retries it. `generation` is the motion generation the failure was found under.
    enum class StopOutcome : std::uint8_t { Sent, Busy, Superseded };
    struct FailureStop {
        StopOutcome outcome = StopOutcome::Superseded;
        std::string error;  // Sent only
    };
    struct PendingStop {
        std::uint64_t generation = 0;
        std::string message;
        bool home = false;
    };

    FailureStop stop_park_slew_unless_superseded(std::uint64_t generation) const {
        run_failure_stop_hook();
        std::unique_lock<std::mutex> ilock(initiator_mutex_, std::try_to_lock);
        if (!ilock.owns_lock()) {
            bool first_deferral = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                first_deferral = !pending_stop_ || pending_stop_->generation != generation;
            }
            if (first_deferral) {  // the poll retries every tick; one line per deferred stop
                ALPACA_LOG_WARN("Celestron",
                                "failure stop deferred: an initiator holds the mount; retried by the poll");
            }
            return {StopOutcome::Busy, {}};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (motion_generation_ != generation) {
                ALPACA_LOG_WARN("Celestron", "failure stop skipped: a new motion command owns the mount");
                return {StopOutcome::Superseded, {}};
            }
        }
        return {StopOutcome::Sent, stop_park_slew_wire()};
    }

    // Sends the failure stops with mutex_ free and publishes the outcome: Sent finishes the failure, Busy keeps the
    // stop pending for the next poll tick, Superseded drops it. Busy never writes an older generation over a newer one
    // (generations only go up): a retry that read the stop before an initiator handed it on leaves it handed on.
    void send_failure_stop(std::uint64_t generation, const std::string& message, bool home) const {
        const FailureStop stop = stop_park_slew_unless_superseded(generation);
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop.outcome == StopOutcome::Busy) {
            if (!pending_stop_ || pending_stop_->generation <= generation) {
                pending_stop_ = PendingStop{generation, message, home};
            }
            return;
        }
        if (pending_stop_ && pending_stop_->generation == generation) {
            pending_stop_.reset();
        }
        if (stop.outcome == StopOutcome::Sent && connected_) {
            if (home) {
                finish_home_failure_locked(generation, message, stop.error);
            } else {
                finish_slew_poll_failure_locked(message, stop.error);
            }
        }
    }

    // Poll tick: retries a stop that found an initiator busy. A disconnect drops it here; a newer motion drops it in
    // send_failure_stop, once no initiator is busy, so an initiator that fails before it sends anything can hand it
    // back first (keep_pending_stop_locked).
    void retry_pending_stop() const {
        PendingStop pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_stop_) {
                return;
            }
            if (!connected_) {
                pending_stop_.reset();
                return;
            }
            pending = *pending_stop_;
        }
        send_failure_stop(pending.generation, pending.message, pending.home);
    }

    // An initiator that took the mount from generation `superseded` and then failed before it sent anything: a stop
    // still pending under `superseded` stays due under the new generation (only a stop or a motion on the wire, or a
    // disconnect, drops one).
    void keep_pending_stop_locked(std::uint64_t superseded) const {
        if (pending_stop_ && pending_stop_->generation == superseded) {
            pending_stop_->generation = motion_generation_;
        }
    }

    // The failure of a motion body is sticky from the moment it is known, before its stops go out with mutex_ free:
    // a client that polls Slewing in the window reads the failure, not a motion still in progress.
    void begin_motion_failure_locked(const std::string& message) const {
        if (!connected_) {
            return;
        }
        last_home_error_.clear();
        last_slew_error_ = message;
    }

    // After the stops: false when the mount disconnected meanwhile (the disconnect owns the state). A body that was
    // cancelled in the window (a newer initiator, an AbortSlew) still finishes its own failure, because the failure
    // has happened and the stops went out; the newer initiator joins the body first and then replaces or clears the
    // error itself.
    bool still_owns_failure_locked() const { return connected_; }

    // Park task failure path: stop the hardware, then drop the parking state so
    // the caller can retry. A successful stop publishes Slewing=false; a failed
    // stop leaves a persistent Slewing error because the mount may still move.
    void fail_park(std::unique_lock<std::mutex>& lock, const std::string& message, util::OperationContext& /*ctx*/) {
        begin_motion_failure_locked(message);
        const std::string stop_error = stop_park_slew(lock);
        if (!still_owns_failure_locked()) {
            return;
        }
        parking_ = false;
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ =
                message + "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron",
                             "stop after park failure failed: " + stop_error + "; the mount may still be moving");
        }
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        flip_in_progress_ = false;
        ALPACA_LOG_WARN("Celestron", message);
    }

    void fail_slew(std::unique_lock<std::mutex>& lock, const std::string& message, util::OperationContext& /*ctx*/) {
        begin_motion_failure_locked(message);
        const std::string stop_error = stop_park_slew(lock);
        if (!still_owns_failure_locked()) {
            return;
        }
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ =
                message + "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron",
                             "stop after slew failure failed: " + stop_error + "; the mount may still be moving");
        }
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        drop_poll_in_flight_locked();
        flip_in_progress_ = false;
        ALPACA_LOG_WARN("Celestron", message);
    }

    // `generation` is the motion generation the failing read was taken under.
    void fail_slew_poll(std::unique_lock<std::mutex>& lock, const std::string& message,
                        std::uint64_t generation) const {
        begin_motion_failure_locked(message);
        without_lock(lock, [&] { send_failure_stop(generation, message, false); });
    }

    // After the stops of a poll-found slew failure went out (`still_owns_failure_locked` already holds).
    void finish_slew_poll_failure_locked(const std::string& message, const std::string& stop_error) const {
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ =
                message + "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("Celestron", "stop after Slewing poll failure failed: " + stop_error +
                                              "; the mount may still be moving");
        }
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        drop_poll_in_flight_locked();
        flip_in_progress_ = false;
        ALPACA_LOG_WARN("Celestron", message);
    }

    void fail_home(std::unique_lock<std::mutex>& lock, const std::string& message) const {
        const std::uint64_t generation = motion_generation_;
        begin_home_failure_locked(message);
        finish_home_failure_locked(generation, message, stop_park_slew(lock));
    }

    // The two halves of fail_home, for the status poll, which sends the stop between them with mutex_ free
    // (the wrapper's per-link lock serialises it): the error is sticky from the first half on, so a getter in the
    // window reports the failure rather than a finished home.
    void begin_home_failure_locked(const std::string& message) const {
        homing_ = false;
        at_home_ = false;
        last_slew_error_ = message;
        last_home_error_ = message;
        ALPACA_LOG_WARN("Celestron", message);
    }

    // `message` is what begin_home_failure_locked stored and `generation` the motion generation it ran under; a new
    // initiator (it bumps the generation) owns the state now, so only the log carries the stop's outcome then.
    void finish_home_failure_locked(std::uint64_t generation, const std::string& message,
                                    const std::string& stop_error) const {
        if (!stop_error.empty()) {
            ALPACA_LOG_ERROR("Celestron",
                             "stop after FindHome failure failed: " + stop_error + "; the mount may still be moving");
        }
        if (motion_generation_ != generation) {
            return;
        }
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ =
                message + "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            last_home_error_ = last_slew_error_;
        }
    }

    void pulse_guide(int direction, int duration) override {
        // The MC_AUX_GUIDE duration byte counts 10 ms steps, 255 per command, chained up to 25.5 s.
        if (direction < 0 || direction > 3 || duration < 0 || duration > kMaxPulseGuideMs) {
            throw AlpacaException("PulseGuide direction must be 0-3 and duration 0-25500 ms",
                                  AlpacaError::InvalidValue);
        }
        if (!has_autoguider_port_) {
            throw AlpacaException("PulseGuide not supported — no autoguider port detected",
                                  AlpacaError::MethodNotImplemented);
        }
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        auto& protocol = CelestronProtocolWrapper::instance();

        int axis = -1;
        int velocity = 0;
        bool read_pier = false;
        // Validation only. The pier read, the join and the start below run with mutex_ free (decision 0006).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");

            switch (direction) {
                case 0:  // North → DEC axis, positive velocity
                    axis = 1;
                    velocity = static_cast<int>(std::round((guide_rate_.dec / kSiderealDegPerSec) * 100.0));
                    break;
                case 1:  // South → DEC axis, negative velocity
                    axis = 1;
                    velocity = -static_cast<int>(std::round((guide_rate_.dec / kSiderealDegPerSec) * 100.0));
                    break;
                case 2:  // East → RA axis, negative velocity (slow tracking = RA increases on sky)
                    axis = 0;
                    velocity = -static_cast<int>(std::round((guide_rate_.ra / kSiderealDegPerSec) * 100.0));
                    break;
                case 3:  // West → RA axis, positive velocity (speed tracking = RA decreases on sky)
                    axis = 0;
                    velocity = static_cast<int>(std::round((guide_rate_.ra / kSiderealDegPerSec) * 100.0));
                    break;
                default:
                    throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
            }

            if (velocity == 0) {
                velocity = (direction == 1 || direction == 2) ? -1 : 1;
            }

            read_pier = axis == 1 && hc_available_;
        }
        // MC_AUX_GUIDE uses raw motor coordinates: positive rate = South
        // when pierWest (normal), North when pierEast (through-the-pole).
        // Flip DEC sign on the normal (West) side so North means North.
        if (read_pier) {
            char pier = protocol.get_pier_side();
            if (pier == 'W') {
                velocity = -velocity;
            }
        }

        int total_cs = duration / 10;
        int first_chunk_cs = std::min(total_cs, 255);

        // The pulse body never takes mutex_, but joining it waits out the serial transaction it is in: do it
        // with mutex_ free, so no getter stalls for that long.
        pulse_ops_[axis].cancel_all_and_join();

        std::chrono::milliseconds pulse_length{duration};
        util::TaskClock::clock::time_point pulse_start;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");

            // Taken after the join: it can last a full transaction timeout, which would leave these stale.
            const bool is_dec_axis = (direction == 0 || direction == 1);
            const double duration_sec = duration / 1000.0;
            const auto now = clock_.now();
            const auto hold_end =
                now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay + kPulseGuideHoldGrace;
            const auto correction_end =
                now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay + kPulseGuideCorrectionGrace;

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

            pulse_guide_active_[static_cast<std::size_t>(axis)] = false;
            pulse_start = now;
        }
        // The guide write goes out with mutex_ free; the initiator lock keeps any other motion out meanwhile.
        protocol.pulse_guide_axis(axis, velocity, first_chunk_cs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guide_active_[static_cast<std::size_t>(axis)] = true;
            pulse_guide_end_time_[static_cast<std::size_t>(axis)] =
                pulse_start + pulse_length + kPulseGuideCompletionDelay;
            drop_poll_in_flight_locked();
        }

        // Chain remaining chunks on the axis's own slot (MC_AUX_GUIDE max =
        // 255cs = 2550ms): one slot per axis, so a pulse on RA never supersedes
        // one on Dec. Cancelled + joined on disconnect and in the destructor.
        // Started with mutex_ free, like the join above.
        int remaining_cs = total_cs - first_chunk_cs;
        if (remaining_cs > 0) {
            try {
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
            } catch (...) {
                // No chain will end the pulse the first chunk started on this axis, and the mount keeps
                // guiding for the first chunk only: stop the axis as AbortSlew does and drop the pulse state
                // before the refusal reaches the client.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto axis_index = static_cast<std::size_t>(axis);
                    pulse_guide_active_[axis_index] = false;
                    pulse_guide_end_time_[axis_index] = util::TaskClock::clock::time_point::min();
                }
                if (connected_) {
                    try {
                        protocol.move_axis_fixed_rate(axis, 0);
                    } catch (...) {
                        ALPACA_LOG_ERROR("Celestron", "PulseGuide stop after a refused chain start failed on axis " +
                                                          std::to_string(axis) + "; the axis may still be moving");
                    }
                }
                throw;
            }
        }
    }

    void set_park() override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        std::tie(park_ra_hours_, park_dec_degrees_) = measure_equatorial(lock);
        park_position_set_ = true;
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinates");
            validate_ra_dec(ra, dec, "SlewToCoordinates");
            check_slew_safety(lock, "SlewToCoordinates");
            check_tracking_for_slew(lock, "SlewToCoordinates");
        }
        cancel_async_tasks();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        clear_pulse_guiding_locked();
        check_not_parked_locked("SlewToCoordinates");
        // The initiator lock stays held through the dispatch, so no other initiator runs while mutex_ is free.
        do_slew_to_coordinates(lock, ra, dec);
        const uint64_t owner_generation = ++motion_generation_;
        ilock.unlock();
        if (!wait_for_slew_complete(lock, owner_generation)) {
            // Another client's AbortSlew / Park / MoveAxis / FindHome / slew took the
            // mount: skip the completion tail so it cannot write T2 over their motion.
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
        restore_tracking_after_slew(lock);
        learn_ra_offset(lock, ra);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see park()
        {
            // Gate BEFORE cancelling: cancelling the slew slot would end a park in
            // flight (clearing parking_) and let this slew clobber it.
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            check_tracking_for_slew(lock, "SlewToCoordinatesAsync");
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
        uint64_t superseded = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            clear_pulse_guiding_locked();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            check_slew_safety(lock, "SlewToCoordinatesAsync");
            slew_aborted_ = false;
            superseded = motion_generation_;
            ++motion_generation_;

            use_passthrough = !hc_available_;
            int bits = use_passthrough ? 24 : (use_precise_commands_ ? 24 : 16);
            double ra_biased = std::fmod(ra + ra_slew_offset_hours_, 24.0);
            if (ra_biased < 0.0) ra_biased += 24.0;
            ra_raw = encode_ra_raw(ra_biased, bits);
            dec_raw = encode_angle(dec, bits);
            precise = use_precise_commands_;

            try {
                do_flip = compute_flip(lock, ra, dec, ra_biased, flip_ra, flip_dec);
            } catch (...) {
                keep_pending_stop_locked(superseded);  // the pier-side read failed before the goto went out
                throw;
            }

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
                    // The goto goes out with mutex_ free. An initiator that cancels this body joins it before it
                    // sends anything of its own, so no stop or newer goto can pass this one on the wire.
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                        return;
                    }
                    try {
                        without_lock(lock, [&] {
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
                        });
                    } catch (const std::exception& ex) {
                        // open-astro#575: a reap by a newer initiator or AbortSlew is not a
                        // failure -- that operation owns clearing/replacing last_slew_error_.
                        if (ctx.stop_reason() == util::StopReason::None) {
                            fail_slew(lock, std::string("SlewToCoordinatesAsync dispatch failed: ") + ex.what(), ctx);
                        }
                        return;
                    } catch (...) {
                        if (ctx.stop_reason() == util::StopReason::None) {
                            fail_slew(lock, "SlewToCoordinatesAsync dispatch failed: unknown exception", ctx);
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
                            std::unique_lock<std::mutex> lock(mutex_);
                            if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                                fail_slew(lock,
                                          "Slew completion polling failed after " + std::to_string(failures) +
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
                            std::unique_lock<std::mutex> lock(mutex_);
                            if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                                fail_slew(lock,
                                          "Slew completion polling failed after " + std::to_string(failures) +
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
                        std::unique_lock<std::mutex> lock(mutex_);
                        if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                            fail_slew(lock, "Slew timed out after 120s", ctx);
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
                // Each step that asks the mount releases mutex_, so the body checks it still owns the slot after each.
                try {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                        return;
                    }
                    restore_tracking_after_slew(lock);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                        return;
                    }
                    learn_ra_offset(lock, slew_target_ra);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                        return;
                    }
                    const Measurement landed = measure_full(lock);
                    if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                        return;
                    }
                    // Slewing, the flags and the position the mount stopped at flip in one locked step.
                    publish_measurement_locked(landed);
                    set_slewing_locked(false);
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
                    flip_in_progress_ = false;
                } catch (const std::exception& ex) {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                        fail_slew(lock, std::string("Slew completion handling failed: ") + ex.what(), ctx);
                    }
                } catch (...) {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (connected_ && ctx.stop_reason() == util::StopReason::None) {
                        fail_slew(lock, "Slew completion handling failed: unknown exception", ctx);
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
                keep_pending_stop_locked(superseded);  // the goto never went out
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
        // An initiator like the motion commands: its write and its publish stay one step to them (the sync used
        // to hold mutex_ across the write, which kept them out).
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        validate_ra_dec(ra, dec, "SyncToCoordinates");
        const bool hc = hc_available_;
        const bool precise = use_precise_commands_;
        without_lock(lock, [&] {
            auto& protocol = CelestronProtocolWrapper::instance();
            if (hc) {
                int bits = precise ? 24 : 16;
                uint32_t ra_raw = encode_ra_raw(ra, bits);
                uint32_t dec_raw = encode_angle(dec, bits);
                protocol.sync_ra_dec_raw(ra_raw, dec_raw, precise);
            } else {
                uint32_t ra_raw = encode_ra_raw(ra, 24);
                uint32_t dec_raw = encode_angle(dec, 24);
                protocol.mc_set_position(0, ra_raw);
                protocol.mc_set_position(1, dec_raw);
            }
        });
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
        bool had_error = false;
        std::string stop_error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            parked_ = false;
            was_parking = parking_;
            had_error = !last_slew_error_.empty() || !last_home_error_.empty();
            if (was_parking) {
                // Unpark during a park wins the race: drop the parking state, and the bump tells the park body
                // (which may be inside an exchange) that it was taken over.
                parking_ = false;
                ++motion_generation_;
            }
        }
        if (was_parking) {
            // Joined before the stops go out, so a park GOTO still in the body's hands cannot land after them.
            slew_.cancel_all_and_join();
            stop_error = stop_park_slew_wire();
        } else if (had_error) {
            // A failed body may still be in its stops with mutex_ free: let it finish its failure before the error
            // is cleared, so Slewing reads false as soon as Unpark returns.
            slew_.cancel_all_and_join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (was_parking) {
                // A failed stop leaves Slewing to report the stored error, since the park slew may still be
                // running.
                if (stop_error.empty()) {
                    set_slewing_locked(false);
                }
                slew_force_until_ = util::TaskClock::clock::time_point::min();
                position_override_until_ = util::TaskClock::clock::time_point::min();
                flip_in_progress_ = false;
            }
            if (stop_error.empty()) {
                // A deferred failure stop stays pending: only a stop or a motion on the wire, or a disconnect, drops
                // one. An Unpark with no park in flight sends nothing, so the next poll tick still sends it.
                clear_motion_errors_locked();
            } else {
                last_home_error_.clear();
                last_slew_error_ = "Unpark could not stop the park slew: " + stop_error;
            }
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
        std::unique_lock<std::mutex> lock(mutex_);
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
        without_lock(lock,
                     [&] { CelestronProtocolWrapper::instance().move_axis_variable_rate(axis, moving ? rate : 0.0); });
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
            // Bumped before any stop goes out: a blocking slew that sees the mount stopped must also see that it
            // was superseded, so it cannot run its completion tail before slew_aborted_ is set.
            ++motion_generation_;
        }
        try {
            // Stop both pulse chains and the slew body before stopping hardware, so none can re-arm a guide pulse
            // or send a GOTO after the stop commands: the joins wait out an exchange a body is in, with mutex_ free.
            pulse_ops_[0].cancel_all_and_join();
            pulse_ops_[1].cancel_all_and_join();
            slew_.cancel_all_and_join();
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_fully_parked_locked("AbortSlew");
            // Try every stop on its own, so one lost command cannot skip the others (#781). The wrapper's lock
            // serialises them behind any exchange in flight.
            const std::string stop_error = stop_park_slew(lock);
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
            flip_in_progress_ = false;
            slew_force_until_ = util::TaskClock::clock::time_point::min();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
        } catch (...) {
            slew_.cancel_all_and_join();
            throw;
        }
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
    // Runs `fn`, a call into the protocol wrapper, with mutex_ released, and takes mutex_ again before it returns or
    // throws. The wrapper serialises its transactions itself (its per-link lock is the I/O lock), so a caller that
    // held mutex_ across the exchange would make every getter wait for it (decision 0016 rule 1). After it returns
    // the caller re-checks whatever state it depends on.
    template <typename Fn>
    static auto without_lock(std::unique_lock<std::mutex>& lock, Fn&& fn) -> decltype(fn()) {
        struct Relock {
            std::unique_lock<std::mutex>& lock;
            ~Relock() { lock.lock(); }
        } relock{lock};
        lock.unlock();
        return fn();
    }

    // The disconnect's state half, also the first half of a relink: the driver stops answering as connected before
    // the transport closes. The caller closes it afterwards with mutex_ free.
    void clear_session_state() {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        utc_offset_.clear();  // decision 0014: the written UTCDate lives until the next connect or disconnect
        mount_clock_unreadable_ = false;
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

    // The connect, under transition_mutex_ with mutex_ free. Every exchange runs under the wrapper's lock alone;
    // what it learns goes into locals and is published under mutex_ in short steps, and connected_ is set last, so a
    // getter that sees Connected also sees the identity and the first frame (decision 0016 rules 3 and 4).
    void connect_session(CelestronProtocolWrapper& protocol) {
        // An auto-detected mount resolves its port here, not in the factory (#659).
        util::connect_resolved(
            connection_info_, connection_resolved_, connection_resolver_,
            [&protocol](const ConnectionInfo& info) {
                if (!protocol.connect(info)) {
                    // The wrapper's reason (a port another device holds, #1202)
                    // goes to the operator.
                    const std::string reason = protocol.last_connect_error();
                    const std::string message = reason.empty() ? "Failed to connect to Celestron mount"
                                                               : "Failed to connect to Celestron mount: " + reason;
                    // Refused TCP connect, vanished serial node, or a port another connected
                    // device now holds (#1202): stale, re-scan.
                    if (info.type == ConnectionType::Network || util::serial_endpoint_stale(info.port_path)) {
                        throw util::StaleEndpoint(message);
                    }
                    throw AlpacaException(message);
                }
            },
            "Celestron");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            link_lost_ = false;
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
            mount_clock_unreadable_ = false;
            client_disagreement_warned_ = false;
            position_link_health_.reset();
            publish_link_fault_locked();
            last_site_info_attempt_ = util::TaskClock::clock::time_point::min();
            clear_motion_errors_locked();
        }

        std::string firmware;
        try {
            firmware = protocol.get_handset_firmware_version();
            // Determine if precise commands are available (v1.6+ for RA/Dec, v2.2+ for Alt/Az)
            // TODO: Parse firmware version to detect v1.6+ support; assume precise for now.
        } catch (...) {
            ALPACA_LOG_WARN("Celestron", "firmware version query failed; assuming precise commands");
        }
        // HC fw "0.0" means the hand controller isn't functional (e.g. WiFi-only connection).
        // In that case, HC-level commands ('E','e','R','r','S','s','L') don't work —
        // use pass-through MC commands instead (MC_GET_POSITION, MC_GOTO_FAST, etc.).
        const bool hc = !firmware.empty() && firmware != "0.0";
        // Publish the firmware to the web-UI getter under its OWN narrow mutex.
        {
            // Gate on the version string directly (not hc_available_) so the
            // UI display isn't coupled to HC-command support: suppress the
            // empty / "0.0" sentinel the same way SynScan suppresses "0.0.0".
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_ = hc ? firmware : std::string();
        }
        ALPACA_LOG_WARN("Celestron", "HC firmware: " + firmware + " — HC " +
                                         (hc ? "available" : "unavailable, using pass-through commands"));
        int model_id = -1;
        std::string model_name;
        try {
            model_id = protocol.get_model_id();
            model_name = protocol.get_model_name();
        } catch (...) {
            ALPACA_LOG_WARN("Celestron", "model query failed; the model stays unknown");
        }

        // Probe the AUX bus to discover connected devices and capabilities.
        bool autoguider_port = false;
        bool ra_switch = false;
        bool dec_switch = false;
        bool gps = false;
        bool focuser = false;
        std::vector<CelestronProtocolWrapper::BusDevice> devices;
        try {
            devices = protocol.probe_bus();
            for (const auto& dev : devices) {
                ALPACA_LOG_INFO("Celestron", "Detected: " + dev.name + " (0x" + ([](int a) {
                                                 char buf[8];
                                                 std::snprintf(buf, sizeof(buf), "%02X", a);
                                                 return std::string(buf);
                                             })(dev.address) +
                                                 ") fw " + dev.firmware_version);
                switch (dev.address) {
                    case CelestronProtocolWrapper::DEV_DEC_AG:
                        autoguider_port = true;
                        break;
                    case CelestronProtocolWrapper::DEV_RA_SW:
                        ra_switch = true;
                        break;
                    case CelestronProtocolWrapper::DEV_DEC_SW:
                        dec_switch = true;
                        break;
                    case CelestronProtocolWrapper::DEV_GPS:
                        gps = true;
                        break;
                    case CelestronProtocolWrapper::DEV_FOCUSER:
                        focuser = true;
                        break;
                    default:
                        break;
                }
            }
        } catch (...) {
            ALPACA_LOG_WARN("Celestron", "Bus probe failed, using default capabilities");
        }

        std::optional<LocationInfo> written_site;
        if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value()) {
            LocationInfo loc;
            loc.latitude_degrees = pending_site_latitude_.value();
            loc.longitude_degrees = pending_site_longitude_.value();
            try {
                protocol.set_location(loc);
                written_site = loc;
            } catch (...) {
                ALPACA_LOG_WARN("Celestron", "site write at connect failed; the mount keeps its own site");
            }
        }
        std::optional<TimeWrite> time_write;
        if (sync_time_on_connect_) {
            time_write = prepare_time_write(std::chrono::system_clock::now());
            protocol.set_time(time_write->info);
        }
        SamplePlan plan;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mount_firmware_version_ = firmware;
            hc_available_ = hc;
            mount_model_id_ = model_id;
            mount_model_name_ = model_name;
            has_autoguider_port_ = autoguider_port;
            has_ra_switch_ = ra_switch;
            has_dec_switch_ = dec_switch;
            has_gps_ = gps;
            has_focuser_ = focuser;
            detected_devices_ = devices;
            if (written_site) {
                site_latitude_cached_ = written_site->latitude_degrees;
                site_longitude_cached_ = written_site->longitude_degrees;
                site_info_valid_ = true;
            }
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            if (time_write) {
                publish_time_write_locked(*time_write);
            }
            plan = sample_plan_locked(true);
        }
        // Seed the snapshot so the first property reads are served from memory, status flags included, and
        // read the clock once, so no getter reaches the mount (ConformU holds the first reads to its
        // fast-response target).
        const auto token = snapshot_.begin_poll();
        const PolledFrame seed = seed_frame(plan);
        std::optional<MountClock> mount_clock;
        if (!time_write) {
            mount_clock = read_mount_clock();
        }
        std::optional<LocationInfo> read_site;
        try {
            read_site = protocol.get_location();
        } catch (...) {
            ALPACA_LOG_WARN("Celestron", "site read at connect failed; the site stays unread");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.publish(token, seed);
        if (!time_write) {
            publish_mount_clock_locked(mount_clock);
        }
        if (read_site) {
            site_latitude_cached_ = read_site->latitude_degrees;
            site_longitude_cached_ = read_site->longitude_degrees;
            site_info_valid_ = true;
        }
        connected_ = true;
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
    // flags, hands back a pending failure stop (keep_pending_stop_locked) and
    // the exception goes to the caller. `displaced` says a body was
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
            std::unique_lock<std::mutex> lock(mutex_);
            rollback_locked();
            if (displaced && connected_) {
                stop_after_refused_start(lock);
            }
            throw;
        }
    }

    // AbortSlew's stop and published state, for a refused start that cancelled
    // a body in flight. A failed stop leaves Slewing as it was: the mount may
    // still be moving.
    void stop_after_refused_start(std::unique_lock<std::mutex>& lock) {
        const std::string stop_error = stop_park_slew(lock);
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

    void set_pulse_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        pulse_ops_[0].set_spawn_for_testing(spawn);
        pulse_ops_[1].set_spawn_for_testing(std::move(spawn));
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

    // The handset tracking mode, as the poll last read it; the getter refuses when no poll has answered the query.
    int tracking_mode_locked() const {
        const auto frame = served_frame_locked();
        if (frame.value.tracking_mode >= 0) {
            return frame.value.tracking_mode;
        }
        throw AlpacaException("Celestron mount tracking state is not available: the last read failed",
                              AlpacaError::DriverException);
    }

    // The control path (a slew's Tracking gate, the tracking restore): the snapshot's mode, or one read of the mount
    // with mutex_ free when no poll has answered the tracking query yet.
    int tracking_mode_for_control(std::unique_lock<std::mutex>& lock) const {
        const auto frame = served_frame_locked();
        if (frame.value.tracking_mode >= 0) {
            return frame.value.tracking_mode;
        }
        const int mode = without_lock(lock, [] { return CelestronProtocolWrapper::instance().get_tracking_mode(); });
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

    // One read of the mount for the control path at the end of a motion (slew landed, park complete, home done), taken
    // with mutex_ free. The snapshot token is taken before the read, so a write that lands meanwhile drops the frame.
    struct Measurement {
        util::StateSnapshot<PolledFrame>::Token token{};
        std::optional<Sample> sample;
        std::string error;  // what the read threw, when `sample` is empty
    };

    // No driver state touched: `plan` is read under mutex_ by the caller.
    Measurement take_measurement(SamplePlan plan) const {
        plan.read_slewing = false;
        Measurement measurement;
        measurement.token = snapshot_.begin_poll();
        try {
            measurement.sample = sample_raw(plan);
        } catch (const std::exception& e) {
            measurement.error = e.what();
        }
        return measurement;
    }

    Measurement measure_full(std::unique_lock<std::mutex>& lock) const {
        const SamplePlan plan = sample_plan_locked(true);
        return without_lock(lock, [&] { return take_measurement(plan); });
    }

    // Publishes a measurement under mutex_: reads the mount now and publishes a full frame, so Slewing false is
    // never served beside a mid-motion position. The caller flips Slewing and its other flags in the same locked
    // step. A failed read leaves the position unknown, which the getters refuse until the next poll answers.
    void publish_measurement_locked(const Measurement& measurement) const {
        if (!measurement.sample) {
            snapshot_.write([](PolledFrame& frame) {
                frame.equatorial_known = false;
                frame.altaz_known = false;
            });
            note_position_failure_locked(std::runtime_error(measurement.error));
            return;
        }
        const auto previous = snapshot_.read();
        snapshot_.publish(measurement.token,
                          decode_sample(*measurement.sample, previous ? previous->value : PolledFrame{}));
        drop_poll_in_flight_locked();  // a poll that sampled before this read must not replace it
        note_position_reply_locked();
    }

    void publish_measurement(std::unique_lock<std::mutex>& lock) const {
        publish_measurement_locked(measure_full(lock));
    }

    // The first frame, read at the end of a connect with mutex_ free, so the getters have an answer the moment Connect
    // returns. Best effort: a mount that does not answer the slewing query still gives its position, and one that
    // answers nothing leaves a frame with nothing known; the poll fills it in.
    // The seed's failures do not feed position_link_health_: the status poll that follows judges the link.
    PolledFrame seed_frame(SamplePlan plan) const {
        PolledFrame frame;
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                frame = decode_sample(sample_raw(plan), frame);
                break;
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): retried without the slewing query
            }
            plan.read_slewing = false;
        }
        return frame;
    }

    // Control path: one direct read of the equatorial pair, for an operation that needs the mount's position now
    // (SetPark, Park without a park position, the RA offset learning). {RA hours, Dec degrees}. The read runs with
    // mutex_ free.
    std::pair<double, double> measure_equatorial(std::unique_lock<std::mutex>& lock) const {
        SamplePlan plan = sample_plan_locked(false);
        plan.read_slewing = false;
        try {
            const auto sample = without_lock(lock, [&] { return sample_raw(plan); });
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
    // Connected=true relinks. Caller holds mutex_ and closes the transport itself once it has released it. True when
    // the link was just declared lost.
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
    // So do a FindHome in flight's completion check and the stops of its third failure (decision 0016 rule 1).
    // Returns false when the connection is gone or lost and the poll body should end. `ctx` is null for the test seam's
    // direct tick, which also ignores the pause.
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
        bool check_home = false;
        std::uint64_t home_generation = 0;
        SamplePlan home_plan;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || link_lost_.load() || stopped()) {
                return false;
            }
            check_home = homing_;
            home_generation = motion_generation_;
            home_plan = sample_plan_locked(true);
        }
        retry_pending_stop();
        if (check_home) {
            // A FindHome in flight is completed here, not by a getter. The exchanges (the completion check, the
            // position read when it is done, and the stops of a third failure) run with mutex_ free; the result is
            // published under it only if the same FindHome is still on.
            const HomeProbe probe = probe_homing_complete();
            std::optional<Measurement> landed;
            if (probe.done) {
                landed = take_measurement(home_plan);
            }
            std::string failure;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_ || stopped()) {
                    return false;
                }
                if (homing_ && motion_generation_ == home_generation) {
                    failure = apply_home_probe_locked(probe, landed);
                    if (!failure.empty()) {
                        begin_home_failure_locked(failure);
                    }
                }
            }
            if (!failure.empty()) {
                // The three stop exchanges go out only while no newer initiator owns the mount.
                send_failure_stop(home_generation, failure, true);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (home_status_poll_failures_ != 0) {
                return true;  // it timed out: no second timeout in this tick (Disconnect waits for the tick)
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || link_lost_.load() || stopped()) {
                return false;
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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_ || stopped()) {
                    return false;
                }
                note_position_failure_locked(e);
                if (!declare_link_lost_if_stale_locked()) {
                    return true;
                }
            }
            // The link is lost: close the transport with mutex_ free (it waits for an exchange in flight).
            CelestronProtocolWrapper::instance().disconnect();
            return false;
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

    void run_slewing_read_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
            hook = slewing_read_hook_;
        }
        if (hook) {
            hook();
        }
    }

    void run_failure_stop_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
            hook = failure_stop_hook_;
        }
        if (hook) {
            hook();
        }
    }

    void run_slew_dispatch_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
            hook = slew_dispatch_hook_;
        }
        if (hook) {
            hook();
        }
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

    void set_slewing_read_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
        slewing_read_hook_ = std::move(hook);
    }

    void set_failure_stop_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
        failure_stop_hook_ = std::move(hook);
    }

    void set_slew_dispatch_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
        slew_dispatch_hook_ = std::move(hook);
    }

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

    // What the control path alone decides about Slewing, or nothing when the mount's own flag decides.
    std::optional<bool> slewing_from_control_path_locked() const {
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
        return std::nullopt;
    }

    // The getter: serves the snapshot, never asks the mount.
    bool get_slewing_locked() const {
        if (const auto decided = slewing_from_control_path_locked()) {
            return *decided;
        }
        const auto reading = served_frame_locked();
        if (!reading.value.slewing_known) {
            // The connect-time query failed; no guess is served until a poll (or the control path) answers.
            throw AlpacaException("Celestron mount state is not available yet: the slewing flag has not been answered",
                                  AlpacaError::DriverException);
        }
        return reading.value.slewing;
    }

    // The control path (the slew-complete loops): asks the mount, with mutex_ free for the exchange.
    bool get_slewing_fresh(std::unique_lock<std::mutex>& lock) const {
        if (const auto decided = slewing_from_control_path_locked()) {
            return *decided;
        }
        return poll_hardware_slewing(lock);
    }

    bool poll_hardware_slewing(std::unique_lock<std::mutex>& lock) const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (clock_.now() < slew_force_until_) {
            return true;
        }
        if (homing_) {
            // The status poll completes a FindHome with mutex_ free; this caller asks no exchange.
            if (!last_slew_error_.empty()) {
                throw AlpacaException(last_slew_error_);
            }
            return true;  // still homing = still slewing
        }
        bool was_slewing = slewing_cached_;
        const bool hc_slewing = hc_available_ && !flip_in_progress_;
        std::optional<Measurement> landed;
        bool hardware_slewing = false;
        // The read runs with mutex_ free: a newer motion command (AbortSlew, MoveAxis, Park, a new slew) may take the
        // mount meanwhile, and its state is then not this read's to change.
        const std::uint64_t generation = motion_generation_;
        try {
            const SamplePlan landed_plan = sample_plan_locked(true);
            std::tie(hardware_slewing, landed) = without_lock(lock, [&] {
                auto& protocol = CelestronProtocolWrapper::instance();
                bool slewing = false;
                if (hc_slewing) {
                    slewing = protocol.is_goto_in_progress();
                } else {
                    bool ra_done = protocol.is_slew_done(0);
                    bool dec_done = protocol.is_slew_done(1);
                    slewing = !ra_done || !dec_done;
                }
                // A slew that just ended is read for its position at once, in the same pass.
                std::optional<Measurement> measurement;
                if (was_slewing && !slewing) {
                    measurement = take_measurement(landed_plan);
                }
                run_slewing_read_hook();  // test seam: a newer motion command lands before the read is applied
                return std::make_tuple(slewing, measurement);
            });
            if (motion_generation_ != generation) {
                return slewing_cached_;  // superseded: the newer motion owns the published state
            }
            note_slewing_read_locked(hardware_slewing);
            slew_status_poll_failures_ = 0;
        } catch (const std::exception& ex) {
            if (motion_generation_ != generation) {
                return slewing_cached_;
            }
            const int failures = ++slew_status_poll_failures_;
            if (failures < kLinkFailureThreshold) {
                ALPACA_LOG_WARN("Celestron", "Slew status poll failed (" + std::to_string(failures) + "/" +
                                                 std::to_string(kLinkFailureThreshold) + "): " + ex.what());
                return slewing_cached_;
            }
            const std::string message = "Slew completion polling failed after " + std::to_string(failures) +
                                        " consecutive failures: " + ex.what();
            if (parking_) throw AlpacaException(message, AlpacaError::DriverException);
            if (slewing_cached_) fail_slew_poll(lock, message, generation);
            throw AlpacaException(last_slew_error_.empty() ? message : last_slew_error_, AlpacaError::DriverException);
        } catch (...) {
            if (motion_generation_ != generation) {
                return slewing_cached_;
            }
            const int failures = ++slew_status_poll_failures_;
            if (failures < kLinkFailureThreshold) {
                ALPACA_LOG_WARN("Celestron", "Slew status poll failed (" + std::to_string(failures) + "/" +
                                                 std::to_string(kLinkFailureThreshold) + "): unknown exception");
                return slewing_cached_;
            }
            const std::string message = "Slew completion polling failed after " + std::to_string(failures) +
                                        " consecutive failures: unknown exception";
            if (parking_) throw AlpacaException(message, AlpacaError::DriverException);
            if (slewing_cached_) fail_slew_poll(lock, message, generation);
            throw AlpacaException(last_slew_error_.empty() ? message : last_slew_error_, AlpacaError::DriverException);
        }
        if (was_slewing && !slewing_cached_) {
            flip_in_progress_ = false;
            if (landed) {
                publish_measurement_locked(*landed);
            }
        }
        return slewing_cached_;
    }

    // The FindHome completion exchanges, with no driver state touched: the status poll runs them with mutex_ free.
    struct HomeProbe {
        bool done = false;
        std::optional<std::string> error;  // the exchange failed: what it threw
    };

    static HomeProbe probe_homing_complete() {
        try {
            auto& protocol = CelestronProtocolWrapper::instance();
            const bool ra_done = protocol.is_level_done(0);
            const bool dec_done = protocol.is_level_done(1);
            return {ra_done && dec_done, std::nullopt};
        } catch (const std::exception& ex) {
            return {false, std::string(ex.what())};
        } catch (...) {
            return {false, std::string("unknown exception")};
        }
    }

    // Applies a probe under mutex_. Returns the failure message when this was the third failed check (the caller then
    // stops the mount and finishes the failure), else empty.
    std::string apply_home_probe_locked(const HomeProbe& probe, const std::optional<Measurement>& landed) const {
        if (!probe.error) {
            home_status_poll_failures_ = 0;
            if (probe.done) {
                // The position the mount stopped at, then the flags, in one locked step.
                if (landed) {
                    publish_measurement_locked(*landed);
                }
                homing_ = false;
                at_home_ = true;
                set_slewing_locked(false);
            }
            return {};
        }
        const int failures = ++home_status_poll_failures_;
        if (failures < kLinkFailureThreshold) {
            ALPACA_LOG_WARN("Celestron", "FindHome status poll failed (" + std::to_string(failures) + "/" +
                                             std::to_string(kLinkFailureThreshold) + "): " + *probe.error);
            return {};
        }
        return "FindHome completion polling failed after " + std::to_string(failures) +
               " consecutive failures: " + *probe.error;
    }

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). Read as the Tracking getter reads it, except that the control path asks the mount once
    // (with mutex_ free) when no poll has answered the tracking query yet.
    void check_tracking_for_slew(std::unique_lock<std::mutex>& lock, const char* who) const {
        if (tracking_mode_for_control(lock) == 0) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // The Tracking getter reads the snapshot only.
    bool get_tracking_locked() const { return tracking_mode_locked() != 0; }

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

    // True when a control-path site read is due (connected, and past kSiteInfoRetryDelay since the last try); stamps
    // the try.
    bool site_info_attempt_due_locked() const {
        if (!connected_) {
            return false;
        }
        auto now = clock_.now();
        if (!site_info_valid_ && last_site_info_attempt_ != util::TaskClock::clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSiteInfoRetryDelay) {
            return false;
        }
        last_site_info_attempt_ = now;
        return true;
    }

    // The site read for a caller with mutex_ free: the exchange runs unlocked, the result is published under it,
    // and a client's SiteLatitude/SiteLongitude write that landed meanwhile wins.
    void read_site_info_unlocked() {
        std::optional<LocationInfo> info;
        try {
            info = CelestronProtocolWrapper::instance().get_location();
        } catch (...) {  // NOLINT(bugprone-empty-catch): the caller reports the missing site
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_ && info && !site_info_valid_) {
            site_latitude_cached_ = info->latitude_degrees;
            site_longitude_cached_ = info->longitude_degrees;
            site_info_valid_ = true;
        }
    }

    // The control path's site read (a flip decision needs the longitude): one attempt per kSiteInfoRetryDelay, with
    // mutex_ free for the exchange; a client's SiteLatitude/SiteLongitude write that landed meanwhile wins.
    void ensure_site_info_cached(std::unique_lock<std::mutex>& lock) const {
        if (!site_info_attempt_due_locked()) {
            return;
        }
        const auto info = without_lock(lock, [] {
            try {
                return std::optional<LocationInfo>(CelestronProtocolWrapper::instance().get_location());
            } catch (...) {
                return std::optional<LocationInfo>();
            }
        });
        if (info && !site_info_valid_) {
            site_latitude_cached_ = info->latitude_degrees;
            site_longitude_cached_ = info->longitude_degrees;
            site_info_valid_ = true;
        }
    }

    // The site the mount holds, for a setter that rewrites one half of it: the cache, else a read with mutex_ free.
    // Throws NotConnected first.
    LocationInfo current_location() const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (site_info_valid_) {
                LocationInfo info;
                info.latitude_degrees = site_latitude_cached_;
                info.longitude_degrees = site_longitude_cached_;
                return info;
            }
        }
        return CelestronProtocolWrapper::instance().get_location();
    }

    // Centralized pier-safety gate used by both sync and async slew paths.
    // Called with mutex_ held; the alignment query runs with it free. Throws if the slew must be refused.
    void check_slew_safety(std::unique_lock<std::mutex>& lock, const char* context) const {
        bool aligned = false;
        std::string aligned_detail;
        try {
            aligned = without_lock(lock, [] { return CelestronProtocolWrapper::instance().is_aligned(); });
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
    void restore_tracking_after_slew(std::unique_lock<std::mutex>& lock) {
        int mode = tracking_mode_for_control(lock);
        if (mode == 0) {
            return;
        }
        if (mode < 0) {
            mode = 2;  // EQ-North default
        }
        without_lock(lock, [mode] { CelestronProtocolWrapper::instance().set_tracking_mode(mode); });
        set_tracking_mode_cached_locked(mode);
    }

    void learn_ra_offset(std::unique_lock<std::mutex>& lock, double target_ra) {
        if (slew_aborted_) {
            ALPACA_LOG_WARN("Celestron", "RA offset learning skipped (slew was aborted)");
            return;
        }
        if (skip_next_ra_learn_) {
            skip_next_ra_learn_ = false;
            ALPACA_LOG_WARN("Celestron", "RA offset learning skipped (first slew after SyncToCoordinates)");
            return;
        }
        const double measured_ra = measure_equatorial(lock).first;
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
    bool compute_flip(std::unique_lock<std::mutex>& lock, double ra, double dec, double ra_biased, uint32_t& ra_raw_out,
                      uint32_t& dec_raw_out) const {
        if (!hc_available_) return false;
        if (!site_info_valid_) {
            ensure_site_info_cached(lock);
            // ensure_site_info_cached() sets site_info_valid_ on success;
            // this re-check detects a failed cache populate, not a dead condition.
            // cppcheck-suppress identicalInnerCondition
            if (!site_info_valid_) return false;
        }
        char hw_side = without_lock(lock, [] { return CelestronProtocolWrapper::instance().get_pier_side(); });
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

    // The blocking slew's and the park's dispatch. The blocking slew's caller holds the initiator lock. The park
    // body runs on the slot thread without it; that is safe because every initiator cancels and joins slew_
    // before it acts, so no other initiator runs while mutex_ is free for the exchanges.
    void do_slew_to_coordinates(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        validate_ra_dec(ra, dec, "SlewToCoordinates");
        check_slew_safety(lock, "SlewToCoordinates");
        slew_aborted_ = false;

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
        bool flip = compute_flip(lock, ra, dec, ra_biased, flip_ra, flip_dec);

        flip_in_progress_ = flip;
        const bool hc = hc_available_;
        const bool precise = use_precise_commands_;
        without_lock(lock, [&] {
            run_slew_dispatch_hook();  // test seam: an initiator lands between the flip decision and the GOTO
            auto& protocol = CelestronProtocolWrapper::instance();
            if (flip) {
                protocol.mc_goto_fast(0, flip_ra);
                protocol.mc_goto_fast(1, flip_dec);
            } else if (hc) {
                protocol.goto_ra_dec_raw(ra_raw_target, dec_raw_target, precise);
            } else {
                protocol.mc_goto_fast(0, ra_raw_target);
                protocol.mc_goto_fast(1, dec_raw_target);
            }
        });
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
            bool slewing = get_slewing_fresh(lock);
            // cppcheck-suppress identicalConditionAfterEarlyExit
            if (motion_generation_ != owner_generation) {
                return false;  // the read released mutex_: a concurrent motion command took the mount meanwhile
            }
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
        // The position is read first; Slewing then flips beside it in one locked step.
        const Measurement landed = measure_full(lock);
        if (motion_generation_ != owner_generation) {
            return false;
        }
        publish_measurement_locked(landed);
        set_slewing_locked(false);
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        if (slew_settle_time_seconds_ > 0) {
            sleep_unlocked(std::chrono::seconds(slew_settle_time_seconds_));
        }
        return motion_generation_ == owner_generation;
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
    std::atomic<bool> connected_{false};       // set last by a connect, first by a disconnect; lock-free for Connected

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
    std::optional<double> site_elevation_m_;  // nullopt until set or configured
    mutable int timezone_offset_minutes_;
    mutable bool timezone_offset_valid_;
    mutable bool dst_observed_;
    // The client's UTCDate (or the mount/host seed) and the task-clock time since (decision 0014); guarded by mutex_.
    mutable util::UtcOffset utc_offset_;
    mutable bool mount_clock_unreadable_ = false;  // the connect-time clock read failed; guarded by mutex_
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
    // capability getters a polling client calls, so they stay lock-free -- the
    // issue #130 shape (alpaca-http-conformance.instructions.md states the rule
    // for get_connected()).
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
    mutable std::optional<PendingStop> pending_stop_;  // a failure stop that found an initiator busy; mutex_
    bool skip_next_ra_learn_ = false;
    mutable bool flip_in_progress_ = false;

    // Serializes motion handoffs: sync/async slews, park, FindHome, PulseGuide,
    // MoveAxis, AbortSlew, Unpark, and disconnect. Never taken under mutex_; a
    // blocking slew releases it before waiting for motion completion.
    mutable std::mutex initiator_mutex_;
    // Serialises the connect, the disconnect and the setters that write the mount (tracking, guide rate, site, UTC)
    // end to end: wire write, then publish. No getter takes it. Lock order (decision 0016): transition_mutex_,
    // initiator_mutex_, the wrapper's per-link lock, mutex_ (leaf), then the AsyncOperation slot and StateSnapshot
    // mutexes. Never taken under mutex_ or by a slew or pulse body.
    std::mutex transition_mutex_;
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
    std::function<void()> slewing_read_hook_;   // guarded by status_poll_hook_mutex_
    std::function<void()> failure_stop_hook_;   // guarded by status_poll_hook_mutex_
    std::function<void()> slew_dispatch_hook_;  // guarded by status_poll_hook_mutex_
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

void set_pulse_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_pulse_spawn_for_testing(std::move(spawn));
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

void set_slewing_read_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_slewing_read_hook_for_testing(std::move(hook));
    }
}

void set_failure_stop_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_failure_stop_hook_for_testing(std::move(hook));
    }
}

void set_slew_dispatch_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* celestron = dynamic_cast<CelestronTelescopeDriver*>(&driver)) {
        celestron->set_slew_dispatch_hook_for_testing(std::move(hook));
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

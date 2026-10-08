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
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <exception>
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
constexpr int kPositionLinkFailureThreshold = 3;
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
constexpr double kDefaultGuideRateDegPerSec = 7.5 / 3600.0;
constexpr double kSiderealDegPerSec = 15.0411 / 3600.0;
// open-astro#880: a GOTO can land tens of arcseconds off (EQM-35 Pro: 29.4" in RA) and the driver reports the
// handset's own position afterwards. Refinement re-issues the GOTO until the readback is inside the tolerance,
// well inside ConformU's +/-10", for at most this many extra passes. A residual still above the failure
// threshold after the last pass is reported as a slew failure rather than as a success.
constexpr double kLandingToleranceArcsec = 3.0;
constexpr double kLandingFailureArcsec = 10.0;
constexpr int kMaxLandingRefinePasses = 3;
// A readback this far from the target is not a landing error a fine re-GOTO corrects (a refused or limit-stopped
// GOTO, a mount that never moved): it is left as it was before refinement existed and logged, not chased.
constexpr double kLandingRefineLimitArcsec = 600.0;
// How long a refinement GOTO forces Slewing true while the handset starts moving (the first GOTO uses 8 s).
constexpr auto kRefineStartForce = std::chrono::seconds(2);
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
        // 50 (0x32) is absent from the published V3/V4 table but is what an
        // EQM-35 Pro reports; cross-confirmed against the same mount's motor
        // controller, whose ":e" mount-code byte is also 0x32.
        case 50:
            return "EQM-35 Pro";
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
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    SynScanTelescopeDriver(int device_number, const ConnectionInfo& connection_info, SynScanVersion version,
                           std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                           std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                           SynScanAlignmentSetting alignment,
                           util::ConnectionResolver<ConnectionInfo> connection_resolver = {})
        : AsyncConnectable("SynScan"),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
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
          sync_time_on_connect_(sync_time_on_connect.value_or(false)),
          alignment_setting_(alignment) {
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

    // The "(SynScan)" suffix disambiguates from the skywatcher direct
    // driver's get_name(), which resolves to the identical model string for a
    // mount reachable over both connections (see the sibling comment there).
    // "SynScan" / "EQMOD" are the terms both ecosystems already use for the
    // two paths (INDI labels: "SynScan", "EQMod Mount"; ASCOM: "SynScan App
    // Driver", "EQMOD"), so keep them rather than "hand controller"/"direct".
    std::string get_name() const override {
        const int model_id = mount_model_id_.load();
        if (model_id >= 0) {
            return "Sky-Watcher " + synscan_model_id_to_name(model_id) + " (SynScan)";
        }
        return "Sky-Watcher Mount (SynScan)";
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

    // Lock-free on purpose: set_connected(true) holds mutex_ for the whole
    // handset handshake (echo, firmware, model, site, time, position warm-up,
    // each a serial round trip with its own response timeout), and the router
    // polls this getter from the PUT connected wait and from every GET
    // connected in between. Taking mutex_ here made those calls block for the
    // entire connect (25 s measured against a silent hand controller: five
    // 5 s timeouts), so the router's 8 s deadline never fired and clients
    // reported "Dynamic client timeout for method Connected" (issue #130).
    // Same atomic-flag pattern as every other AsyncConnectable driver except
    // the telescopes and the wrapper-backed switches named in
    // async_connectable.h. Stated as the rule rather than as a count: a bare
    // number in a comment has nothing tying it to the code it describes, and
    // every new driver invalidates it silently (open-astro#381).
    bool get_connected() const override { return connected_.load(); }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_, std::defer_lock);
        if (!connected) {
            ilock.lock();
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
        ++motion_generation_;

        auto& protocol = SynScanProtocolWrapper::instance();
        if (connected) {
            // An auto-detected mount resolves its port here, not in the factory (#659).
            // The echo test lives INSIDE the retry lambda: connect() only opens the
            // port, and a stale auto-detected path that another adapter now owns
            // still opens, so an identity gate outside the lambda would count that
            // as "the resolved endpoint still answers" and never re-scan.
            util::connect_resolved(
                connection_info_, connection_resolved_, connection_resolver_,
                [&protocol](const ConnectionInfo& info) {
                    if (!protocol.connect(info)) {
                        // Refused TCP connect or vanished serial node: stale, re-scan.
                        if (info.type == alpacacore::vendor::synscan::ConnectionType::Network ||
                            util::device_node_missing(info.port_path)) {
                            throw util::StaleEndpoint("Failed to connect to SynScan mount");
                        }
                        throw AlpacaException("Failed to connect to SynScan mount");
                    }
                    if (!protocol.echo_test()) {
                        // Without this gate a link with nothing listening came up
                        // as Connected=true once every query below had burnt its
                        // full response timeout (all of them swallowed), and the
                        // client then saw each command time out in turn. Fail
                        // within one timeout, and say where to look. It is the
                        // identity gate: a port that opens but does not echo is
                        // not (or no longer) this handset, so it is stale.
                        protocol.disconnect();
                        const std::string where = info.type == alpacacore::vendor::synscan::ConnectionType::Serial
                                                      ? info.port_path
                                                      : info.host + ":" + std::to_string(info.tcp_port);
                        throw util::StaleEndpoint("SynScan hand controller did not answer the echo test on " + where +
                                                  " - check that the cable is on the handset's PC port, the handset is "
                                                  "powered and past its start-up prompts, and the baud rate is 9600");
                    }
                },
                "SynScan");
            connected_ = true;
            mount_firmware_version_ = "";
            mount_model_id_ = -1;
            site_info_valid_ = false;
            timezone_offset_valid_ = false;
            tracking_mode_valid_ = false;
            target_ra_set_ = false;
            target_dec_set_ = false;
            parked_ = false;
            parking_ = false;
            at_home_ = false;
            clear_pulse_guiding_locked();
            slewing_cached_ = false;
            refining_generation_ = 0;
            last_slew_error_.clear();
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            guide_position_valid_ = false;
            last_utc_valid_ = false;
            client_disagreement_warned_ = false;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            position_link_health_.reset();
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
            target_ra_set_ = false;
            target_dec_set_ = false;
            parked_ = false;
            parking_ = false;
            at_home_ = false;
            clear_pulse_guiding_locked();
            slewing_cached_ = false;
            refining_generation_ = 0;
            last_slew_error_.clear();
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            guide_position_valid_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            position_link_health_.reset();
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
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return alignment_mode_locked();
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
        check_connected();
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
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t axis = 0; axis < pulse_guiding_active_.size(); ++axis) {
            if (pulse_guiding_active_[axis] && now >= pulse_guide_end_time_[axis]) {
                pulse_guiding_active_[axis] = false;
            }
        }
        return pulse_guiding_active_[0] || pulse_guiding_active_[1];
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
        if (!position_link_health_.faulted() && guide_position_valid_ &&
            std::chrono::steady_clock::now() < position_override_until_ && !get_slewing_locked()) {
            return std::clamp(guide_position_dec_degrees_, -90.0, 90.0);
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
            const AlignmentMode alignment = alignment_mode_locked();
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                ensure_site_info_cached_locked();
            }
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                throw AlpacaException("Set SiteLatitude before enabling equatorial tracking", AlpacaError::ValueNotSet);
            }
            const int mode = alignment == AlignmentMode::AltAz ? 1 : (site_latitude_cached_ < 0.0 ? 3 : 2);
            protocol.set_tracking_mode(mode);
            tracking_mode_cached_ = mode;
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
        std::lock_guard<std::mutex> lock(mutex_);
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate must be a finite number", AlpacaError::InvalidValue);
        }
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
        if (!position_link_health_.faulted() && guide_position_valid_ &&
            std::chrono::steady_clock::now() < position_override_until_ && !get_slewing_locked()) {
            return guide_position_ra_hours_;
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
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
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
        if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
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
        if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
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

        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        SynScanProtocolWrapper::instance().set_time(info);
        timezone_offset_minutes_ = tz_info.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = tz_info.dst;
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("SynScan", client_minus_host, client_disagreement_warned_);
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
        // Serialize against other initiators (SlewToCoordinatesAsync): the
        // check-then-reap-then-spawn sequence must not interleave with
        // another initiator's, or a park could be cancelled and restarted
        // (or a slew could clobber a park) in the gap.
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            // Check BEFORE reaping: reap_slew_task() would cancel a park in
            // flight (its task clears parking_), so a second Park would
            // restart the slew instead of being the documented no-op.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // calling Park twice (or while parking) is harmless
            }
        }
        bool park_altaz = false;
        double park_target_first = 0.0;
        double park_target_second = 0.0;
        // Cancel + join any previous slew task first. Must run without mutex_
        // held: the task takes mutex_.
        reap_slew_task();
        reap_pulse_tasks();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // ASCOM: Park on a parked (or parking) mount is harmless.
            }
            if (!park_position_set_) {
                store_park_position_locked();
            }
            park_altaz = park_alignment_mode_ == AlignmentMode::AltAz;
            if (park_altaz) {
                park_target_first = park_azimuth_degrees_;
                park_target_second = park_altitude_degrees_;
            } else if (!park_ra_uses_hour_angle_) {
                park_target_first = park_ra_hours_;
                park_target_second = park_dec_degrees_;
            } else {
                if (!site_info_valid_) {
                    ensure_site_info_cached_locked();
                }
                // An hour angle is saved only while a site is known, so a
                // handset that has since stopped reporting one (a reconnect
                // without location) still leaves the last known longitude here.
                const double lst =
                    compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_cached_);
                park_target_first = std::fmod(lst - park_hour_angle_hours_, 24.0);
                if (park_target_first < 0.0) {
                    park_target_first += 24.0;
                }
                park_target_second = park_dec_degrees_;
                validate_ra_dec(park_target_first, park_target_second, "Park");
            }
            // Publish the slewing state before the task starts so a poller
            // never sees Slewing false between Park returning and dispatch.
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            guide_position_valid_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            at_home_ = false;
            parking_ = true;
            ++motion_generation_;
            clear_pulse_guiding_locked();
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that calls Park after a failed GOTO must not be told the OLD
            // goto failed while it's parking.
            last_slew_error_.clear();
        }

        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (slew_task_thread_.joinable()) {
            std::thread stale = std::move(slew_task_thread_);
            tlock.unlock();
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            stale.join();
            slew_task_cancel_.store(false);
            tlock.lock();
        }
        slew_task_thread_ = std::thread([this, park_altaz, park_target_first, park_target_second]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load() || !parking_) {
                parking_ = false;
                return;
            }
            try {
                if (park_altaz) {
                    do_slew_to_altaz_locked(park_target_second, park_target_first);
                } else {
                    do_slew_to_coordinates_locked(park_target_first, park_target_second);
                }
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
                // Cancelled (AbortSlew, disconnect / destruction, or a newer
                // initiator), aborted, or unparked meanwhile: the canceller owns
                // the state.
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
            guide_position_valid_ = false;
            // AtPark and Slewing flip in the same locked step.
            parked_ = true;
            parking_ = false;
        });
    }

    // Stop a park slew: cancel the GOTO, then both axes to rate 0. Each stop
    // is tried on its own so one failure does not skip the others. Returns
    // the first failure's message, or an empty string when every stop was
    // answered (#742). mutex_ must be held.
    std::string stop_park_slew_locked() {
        auto& protocol = SynScanProtocolWrapper::instance();
        std::string first_error;
        const auto try_stop = [&first_error](auto&& stop) {
            try {
                stop();
            } catch (const std::exception& ex) {
                if (first_error.empty()) {
                    first_error = ex.what();
                }
            } catch (...) {
                if (first_error.empty()) {
                    first_error = "unknown exception";
                }
            }
        };
        try_stop([&protocol] { protocol.cancel_goto(); });
        try_stop([&protocol] { protocol.move_axis_fixed_rate(0, 0); });
        try_stop([&protocol] { protocol.move_axis_fixed_rate(1, 0); });
        return first_error;
    }

    // Park task failure path: stop the hardware so the reported idle state
    // (Slewing false, AtPark false) matches reality, then drop the parking
    // state so the caller can retry. When a stop fails the mount may still
    // be moving, so Slewing keeps its cached value. mutex_ must be held.
    void fail_park_locked(const std::string& message) {
        const std::string stop_error = stop_park_slew_locked();
        parking_ = false;
        if (stop_error.empty()) {
            slewing_cached_ = false;
        } else {
            ALPACA_LOG_ERROR("SynScan",
                             "stop after park failure failed: " + stop_error + "; the mount may still be moving");
        }
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        ALPACA_LOG_WARN("SynScan", message);
    }

    void pulse_guide(int direction, int duration) override {
        if (duration < 0) {
            throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
        }
        int axis = -1;
        double direction_sign = 0.0;
        switch (direction) {
            case 0:
                axis = 1;
                direction_sign = 1.0;
                break;
            case 1:
                axis = 1;
                direction_sign = -1.0;
                break;
            case 2:
                axis = 0;
                direction_sign = -1.0;
                break;
            case 3:
                axis = 0;
                direction_sign = 1.0;
                break;
            default:
                throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            // open-astro#775: ITelescopeV4 raises InvalidOperation for a
            // PulseGuide while a slew is in progress.
            if (get_slewing_locked()) {
                throw AlpacaException("PulseGuide is not allowed while the mount is slewing",
                                      AlpacaError::InvalidOperation);
            }
        }
        reap_pulse_task(axis);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            auto& protocol = SynScanProtocolWrapper::instance();
            const double guide_rate = axis == 1 ? guide_rate_.dec : guide_rate_.ra;
            double slew_rate_deg_per_sec = direction_sign * guide_rate;
            if (axis == 1) {
                char pier = protocol.get_pointing_state();
                if (pier == 'W') {
                    slew_rate_deg_per_sec = -slew_rate_deg_per_sec;
                }
            }

            const double duration_sec = duration / 1000.0;
            const auto now = std::chrono::steady_clock::now();

            // Initialize the guide-position estimate from mount position when its override expires.
            if (!guide_position_valid_ || now >= position_override_until_) {
                refresh_equatorial_cache_locked();
                guide_position_ra_hours_ = cached_ra_hours_;
                guide_position_dec_degrees_ = cached_dec_degrees_;
                guide_position_valid_ = true;
            }

            // Accumulate expected pulse delta in the private guide-position estimate.
            if (direction == 0 || direction == 1) {
                double delta_deg = guide_rate_.dec * duration_sec;
                if (direction == 1) delta_deg = -delta_deg;
                guide_position_dec_degrees_ = std::clamp(guide_position_dec_degrees_ + delta_deg, -90.0, 90.0);
            } else {
                double delta_hours = (guide_rate_.ra * duration_sec) / 15.0;
                if (direction == 3) delta_hours = -delta_hours;
                guide_position_ra_hours_ = std::fmod(guide_position_ra_hours_ + delta_hours, 24.0);
                if (guide_position_ra_hours_ < 0.0) guide_position_ra_hours_ += 24.0;
            }

            // Keep position override active so get_ra/get_dec return the estimate.
            position_override_until_ =
                now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay + kPulseGuidePositionGrace;

            protocol.move_axis_variable_rate(axis, slew_rate_deg_per_sec);

            pulse_guiding_active_[static_cast<std::size_t>(axis)] = true;
            pulse_guide_end_time_[static_cast<std::size_t>(axis)] =
                now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay;
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
        }

        // Each axis has its own timer; reaping above happened before the new
        // rate command, so the old timer cannot stop this replacement pulse.
        std::lock_guard<std::mutex> tlock(task_mutex_);
        pulse_task_threads_[static_cast<std::size_t>(axis)] = std::thread([this, axis, duration]() {
            const auto axis_index = static_cast<std::size_t>(axis);
            auto& cancel = pulse_task_cancel_[axis_index];
            if (!task_wait_for(std::chrono::milliseconds(duration), cancel)) {
                // Cancelled by replacement, AbortSlew, disconnect or
                // destruction. Still make a best-effort attempt to stop the
                // axis before exiting — the mount would otherwise keep slewing.
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
                if (!stopped && !task_wait_for(std::chrono::milliseconds(100), cancel)) {
                    break;
                }
            }
            if (stopped) {
                if (axis == 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!connected_) {
                        return;
                    }
                    const int tracking_mode = tracking_mode_cached_;
                    if (tracking_mode <= 0) {
                        return;
                    }
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
                pulse_guiding_active_[axis_index] = false;
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
        store_park_position_locked();
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinates");
            validate_ra_dec(ra, dec, "SlewToCoordinates");
        }
        reap_slew_task();
        reap_pulse_tasks();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        clear_pulse_guiding_locked();
        check_not_parked_locked("SlewToCoordinates");
        do_slew_to_coordinates_locked(ra, dec);
        const uint64_t owner_generation = ++motion_generation_;
        // Slewing stays true from the first GOTO to the end of the last refinement pass (open-astro#880).
        const RefineScope refine_scope(*this, owner_generation, !use_precise_commands_);
        ilock.unlock();
        if (!wait_for_slew_complete(lock, owner_generation, false)) {
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
        if (!refine_goto_landing_locked(lock, ra, dec, owner_generation, false) ||
            !settle_after_slew_locked(lock, owner_generation)) {
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see park()
        {
            // Gate BEFORE reaping: reap_slew_task() would cancel a park in
            // flight (clearing parking_) and let this slew clobber it.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            // Argument validation precedes the state check (ASCOM contract).
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            // open-astro#775: ITelescopeV4 raises InvalidOperation when a slew
            // is requested with Tracking false.
            if (!get_tracking_locked()) {
                throw AlpacaException("SlewToCoordinatesAsync requires Tracking to be true",
                                      AlpacaError::InvalidOperation);
            }
        }
        uint32_t ra_raw = 0;
        uint32_t dec_raw = 0;
        bool precise = false;
        uint64_t owner_generation = 0;
        // Cancel + join any previous slew dispatch task first. Must run
        // without mutex_ held: the task takes mutex_.
        reap_slew_task();
        reap_pulse_tasks();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            owner_generation = ++motion_generation_;
            if (use_precise_commands_) {
                refining_generation_ = owner_generation;
            }

            int bits = use_precise_commands_ ? 24 : 16;
            ra_raw = encode_ra_raw(ra, bits);
            dec_raw = encode_angle(dec, bits);
            precise = use_precise_commands_;

            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            slewing_cached_ = true;
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that retries a rejected goto must not be told the OLD goto
            // failed.
            last_slew_error_.clear();
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            position_override_until_ = std::chrono::steady_clock::time_point::min();
            guide_position_valid_ = false;
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
            clear_pulse_guiding_locked();
        }

        // Dispatch in a joinable member thread (never detached), cancelled +
        // joined on disconnect and in the destructor.
        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (slew_task_thread_.joinable()) {
            std::thread stale = std::move(slew_task_thread_);
            tlock.unlock();
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            stale.join();
            slew_task_cancel_.store(false);
            tlock.lock();
        }
        slew_task_thread_ = std::thread([this, ra, dec, ra_raw, dec_raw, precise, owner_generation]() {
            std::unique_lock<std::mutex> lock(mutex_);
            const RefineScope refine_scope(*this, owner_generation, true);  // armed by the initiator; this only clears
            if (!connected_ || slew_task_cancel_.load()) {
                return;
            }
            try {
                SynScanProtocolWrapper::instance().goto_ra_dec_raw(ra_raw, dec_raw, precise);
                // Slewing stays true across the refinement passes: the first wait ends when the handset reports
                // the GOTO done, then the landing is read back and corrected (open-astro#880).
                if (precise && wait_for_slew_complete(lock, owner_generation, false, &slew_task_cancel_)) {
                    refine_goto_landing_locked(lock, ra, dec, owner_generation, true);
                }
            } catch (const std::exception& ex) {
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
                // open-astro#575: a reap by a newer initiator is not a failure -- that
                // initiator already owns clearing/replacing last_slew_error_.
                // AbortSlew now reaps the worker as well, so recording an error
                // after cancellation would poison the NEXT Slewing read with an
                // artifact of the cancel, not a real fault.
                if (!slew_task_cancel_.load()) {
                    last_slew_error_ = std::string("SlewToCoordinatesAsync failed: ") + ex.what();
                }
                ALPACA_LOG_WARN("SynScan", std::string("Async slew dispatch failed: ") + ex.what());
            } catch (...) {
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
                if (!slew_task_cancel_.load()) {
                    last_slew_error_ = "SlewToCoordinatesAsync failed with an unknown error";
                }
                ALPACA_LOG_WARN("SynScan", "Async slew dispatch failed with unknown exception");
            }
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
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t ra_raw = encode_ra_raw(ra, bits);
        uint32_t dec_raw = encode_angle(dec, bits);
        protocol.sync_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_);
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        guide_position_ra_hours_ = ra;
        guide_position_dec_degrees_ = dec;
        guide_position_valid_ = true;
        position_override_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
                // failed stop leaves Slewing at its cached value, since the
                // park slew may still be running.
                parking_ = false;
                stop_error = stop_park_slew_locked();
                if (stop_error.empty()) {
                    slewing_cached_ = false;
                }
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
            }
        }
        if (was_parking) {
            reap_slew_task();  // without mutex_ held
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
        reap_slew_task();
        reap_pulse_task(axis);
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("MoveAxis");
        ++motion_generation_;

        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;
        manual_axis_slewing_[axis] = moving;
        if (moving) {
            parked_ = false;
            at_home_ = false;
        }
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that jogs an axis after a failed GOTO must not be told the OLD
        // goto failed.
        last_slew_error_.clear();

        SynScanProtocolWrapper::instance().move_axis_variable_rate(axis, moving ? rate : 0.0);
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        pulse_guiding_active_[static_cast<std::size_t>(axis)] = false;
        pulse_guide_end_time_[static_cast<std::size_t>(axis)] = std::chrono::steady_clock::time_point::min();
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            // Tertiary axis is not supported; return an empty range set per ASCOM semantics.
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
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
        }
        try {
            reap_pulse_tasks();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                check_connected();
                check_not_fully_parked_locked("AbortSlew");
                // The wrapper serializes transactions, so a GOTO already in flight
                // must finish (or time out) before these stops can reach the mount.
                auto& protocol = SynScanProtocolWrapper::instance();
                protocol.cancel_goto();
                protocol.move_axis_fixed_rate(0, 0);
                protocol.move_axis_fixed_rate(1, 0);
                if (tracking_mode_cached_ > 0) {
                    // Stopping an RA guide pulse also stops sidereal tracking.
                    // AbortSlew owns this stop, so restore the currently
                    // requested mode only after both axes have stopped.
                    protocol.set_tracking_mode(tracking_mode_cached_);
                }
                parking_ = false;  // an aborted park never reaches AtPark
                ++motion_generation_;
                slewing_cached_ = false;
                clear_pulse_guiding_locked();
                // open-astro#575: AbortSlew is a valid clearing command for a stored
                // slew failure -- the client acted on the error, so the next Slewing
                // read must answer normally again.
                last_slew_error_.clear();
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                position_override_until_ = std::chrono::steady_clock::time_point::min();
                manual_axis_slewing_[0] = false;
                manual_axis_slewing_[1] = false;
            }
        } catch (...) {
            reap_slew_task();
            throw;
        }
        reap_slew_task();
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
            throw AlpacaException("Not connected to SynScan mount", AlpacaError::NotConnected);
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
        for (auto& cancel : pulse_task_cancel_) {
            cancel.store(true);
        }
        task_cv_.notify_all();
        std::thread slew_thread;
        std::array<std::thread, 2> pulse_threads;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            slew_thread = std::move(slew_task_thread_);
            for (std::size_t axis = 0; axis < pulse_task_threads_.size(); ++axis) {
                pulse_threads[axis] = std::move(pulse_task_threads_[axis]);
            }
        }
        if (slew_thread.joinable()) {
            slew_thread.join();
        }
        for (auto& pulse_thread : pulse_threads) {
            if (pulse_thread.joinable()) {
                pulse_thread.join();
            }
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

    // Join the previous pulse-stop task for one axis and reset its cancel flag.
    // Must be called WITHOUT mutex_ held (the task failure path takes mutex_).
    void reap_pulse_task(int axis) {
        const auto index = static_cast<std::size_t>(axis);
        pulse_task_cancel_[index].store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(pulse_task_threads_[index]);
        }
        if (prev.joinable()) {
            prev.join();
        }
        pulse_task_cancel_[index].store(false);
    }

    void reap_pulse_tasks() {
        reap_pulse_task(0);
        reap_pulse_task(1);
    }

    void clear_pulse_guiding_locked() {
        pulse_guiding_active_.fill(false);
        pulse_guide_end_time_.fill(std::chrono::steady_clock::time_point::min());
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

    void refresh_equatorial_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (!position_link_health_.faulted() && equatorial_cache_valid_ &&
            (now - last_equatorial_update_) < kPositionCacheTtl) {
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
            note_position_reply_locked();
        } catch (const std::exception& e) {
            equatorial_cache_valid_ = false;
            note_position_failure_locked(e);
            if (position_link_health_.faulted()) {
                throw_position_link_fault_locked();
            }
            throw;
        }
    }

    void refresh_altaz_cache_locked() const {
        auto now = std::chrono::steady_clock::now();
        if (!position_link_health_.faulted() && altaz_cache_valid_ && (now - last_altaz_update_) < kPositionCacheTtl) {
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
            note_position_reply_locked();
        } catch (const std::exception& e) {
            altaz_cache_valid_ = false;
            note_position_failure_locked(e);
            if (position_link_health_.faulted()) {
                throw_position_link_fault_locked();
            }
            throw;
        }
    }

    void note_position_reply_locked() const {
        if (position_link_health_.on_reply()) {
            ALPACA_LOG_INFO("SynScan", "Position link recovered; mount readback is available again");
        }
    }

    void note_position_failure_locked(const std::exception& e) const {
        if (auto fault = position_link_health_.note_failure(e.what(), kPositionLinkFailureThreshold)) {
            ALPACA_LOG_ERROR("SynScan", "Position link faulted: " + *fault);
        } else {
            ALPACA_LOG_WARN("SynScan", "Position read failed (" +
                                           std::to_string(position_link_health_.consecutive_failures()) +
                                           " consecutive failures): " + e.what());
        }
    }

    [[noreturn]] void throw_position_link_fault_locked() const {
        throw AlpacaException("SynScan mount communications compromised: " + position_link_health_.fault(),
                              AlpacaError::DriverException);
    }

    bool get_slewing_locked() const {
        // A park in flight reports Slewing until the park task flips AtPark
        // (same locked step) — never Slewing false with AtPark false.
        if (parking_) {
            return true;
        }
        if (refining_generation_ != 0 && refining_generation_ == motion_generation_) {
            return true;  // a GOTO whose landing is still being refined (open-astro#880)
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
            guide_position_valid_ = false;
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

    AlignmentMode alignment_mode_locked() const {
        const int model_id = mount_model_id_.load();
        switch (model_id) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 50:
            case 56:
                return AlignmentMode::GermanPolar;
            case 160:
                return AlignmentMode::AltAz;
            case 5:
            case 6:
                // The handset does not say which geometry an AZ-EQ mount is set
                // up in; only the device config's alignmentMode can (#860).
                if (alignment_setting_ == SynScanAlignmentSetting::AltAz) {
                    return AlignmentMode::AltAz;
                }
                if (alignment_setting_ == SynScanAlignmentSetting::Equatorial) {
                    return AlignmentMode::GermanPolar;
                }
                throw AlpacaException(
                    "SynScan cannot report whether this AZ-EQ mount is currently configured "
                    "for Alt-Az or equatorial alignment",
                    AlpacaError::DriverException);
            default:
                if ((model_id >= 128 && model_id <= 159)) {
                    return AlignmentMode::AltAz;
                }
                throw AlpacaException(
                    "SynScan cannot determine AlignmentMode for mount model ID " + std::to_string(model_id),
                    AlpacaError::DriverException);
        }
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

    // Saves the current RA/Dec as the park target, as main did for every mount.
    void store_legacy_park_position_locked() {
        refresh_equatorial_cache_locked();
        park_alignment_mode_ = AlignmentMode::GermanPolar;
        park_ra_uses_hour_angle_ = false;
        park_ra_hours_ = cached_ra_hours_;
        park_dec_degrees_ = cached_dec_degrees_;
        park_position_set_ = true;
    }

    void store_park_position_locked() {
        try {
            park_alignment_mode_ = alignment_mode_locked();
        } catch (const AlpacaException&) {
            // Dual-mode and unidentified handsets cannot tell us whether the
            // saved RA/Dec needs sidereal conversion. Keep their prior usable
            // park behavior instead of making SetPark and Park unavailable.
            store_legacy_park_position_locked();
            return;
        }
        if (park_alignment_mode_ == AlignmentMode::AltAz) {
            refresh_altaz_cache_locked();
            park_azimuth_degrees_ = cached_az_degrees_;
            park_altitude_degrees_ = cached_alt_degrees_;
            park_ra_uses_hour_angle_ = false;
        } else {
            if (!site_info_valid_) {
                ensure_site_info_cached_locked();
            }
            if (!site_info_valid_) {
                // No longitude, so no LST for an hour angle: keep the prior
                // park behavior rather than making SetPark and Park unavailable.
                store_legacy_park_position_locked();
                return;
            }
            refresh_equatorial_cache_locked();
            const double lst =
                compute_local_sidereal_time_hours(std::chrono::system_clock::now(), site_longitude_cached_);
            park_hour_angle_hours_ = shortest_ra_delta_hours(lst, cached_ra_hours_);
            park_ra_hours_ = cached_ra_hours_;
            park_dec_degrees_ = cached_dec_degrees_;
            park_ra_uses_hour_angle_ = true;
        }
        park_position_set_ = true;
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
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that retries a rejected goto (even via the blocking
        // SlewToCoordinates) must not be told the OLD goto failed.
        last_slew_error_.clear();
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        guide_position_valid_ = false;
        protocol.goto_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_);
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
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
        equatorial_cache_valid_ = false;
        altaz_cache_valid_ = false;
        slewing_cached_ = true;
        last_slew_error_.clear();
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        guide_position_valid_ = false;
        protocol.goto_alt_az_raw(az_raw, alt_raw, use_precise_commands_);
        parked_ = false;
        at_home_ = false;
    }

    // Poll for slew completion, RELEASING the driver mutex around every sleep
    // so a sync slew/park doesn't block all GETs and disconnect for up to
    // 120 s (Bisque's unlock/sleep/relock loop is the reference pattern).
    // `lock` must be held on entry; it is held again on return/throw. The
    // connection state is re-checked after each relock.
    // Returns false when a concurrent motion command bumped motion_generation_
    // past `owner_generation` while the lock was released (the slew was superseded).
    // `cancel` (async slew task only) makes every sleep interruptible through task_wait_for(), the way the park
    // task waits, so a join by Disconnect, MoveAxis, a new slew or the destructor returns at once; the wait then
    // returns false.
    bool wait_for_slew_complete(std::unique_lock<std::mutex>& lock, uint64_t owner_generation, bool apply_settle = true,
                                std::atomic<bool>* cancel = nullptr) const {
        const auto timeout = std::chrono::seconds(120);
        auto start = std::chrono::steady_clock::now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            if (cancel != nullptr) {
                task_wait_for(d, *cancel);
            } else {
                std::this_thread::sleep_for(d);
            }
            lock.lock();
            // The mount may have been disconnected while the lock was released.
            check_connected();
        };
        while (true) {
            if (motion_generation_ != owner_generation || (cancel != nullptr && cancel->load())) {
                return false;
            }
            // The hardware answer, not get_slewing_locked(): a refinement scope holds the public Slewing true.
            bool slewing = poll_hardware_slewing_locked();
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
        guide_position_valid_ = false;
        position_override_until_ = std::chrono::steady_clock::time_point::min();
        if (apply_settle && slew_settle_time_seconds_ > 0) {
            sleep_unlocked(std::chrono::seconds(slew_settle_time_seconds_));
        }
        return motion_generation_ == owner_generation && (cancel == nullptr || !cancel->load());
    }

    // The configured settle time, applied once after the LAST GOTO pass (open-astro#880). `lock` held on entry
    // and on return; false when a concurrent motion command superseded the slew meanwhile.
    bool settle_after_slew_locked(std::unique_lock<std::mutex>& lock, uint64_t owner_generation) const {
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
        return motion_generation_ == owner_generation;
    }

    // Marks a GOTO whose landing will be refined: get_slewing_locked() reports true while the scope's generation
    // is current, so Slewing cannot dip between the passes. Cleared on every exit; a newer motion command (or
    // AbortSlew) bumps motion_generation_, which ends it on its own. `skip` leaves it unset (no refinement).
    class RefineScope {
    public:
        RefineScope(const SynScanTelescopeDriver& driver, uint64_t generation, bool skip)
            : driver_(driver), generation_(generation) {
            if (!skip) {
                driver_.refining_generation_ = generation;
            }
        }
        ~RefineScope() {
            if (driver_.refining_generation_ == generation_) {
                driver_.refining_generation_ = 0;
            }
        }
        RefineScope(const RefineScope&) = delete;
        RefineScope& operator=(const RefineScope&) = delete;

    private:
        const SynScanTelescopeDriver& driver_;
        uint64_t generation_;
    };

    // After a GOTO completes, read the handset's own position and re-issue the GOTO while RA or Dec is off by
    // more than kLandingToleranceArcsec, at most kMaxLandingRefinePasses times (open-astro#880). Each pass aims at
    // the target corrected by the residual just read, so a systematic landing offset is cancelled rather than
    // repeated. 16-bit handsets (V3) step ~19.8" in RA, so they are not refined. `lock` is held on entry and on
    // return. Returns false when the slew was superseded, aborted or cancelled; throws when a pass fails or the
    // last pass still leaves more than kLandingFailureArcsec.
    bool refine_goto_landing_locked(std::unique_lock<std::mutex>& lock, double ra, double dec,
                                    uint64_t owner_generation, bool async_task) {
        if (!use_precise_commands_) {
            return true;
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        double aim_ra = ra;
        double aim_dec = dec;
        for (int pass = 0;; ++pass) {
            if (motion_generation_ != owner_generation || (async_task && slew_task_cancel_.load())) {
                return false;
            }
            check_connected();
            const auto raw = protocol.get_ra_dec_raw(true);
            double ra_error_hours = ra - decode_ra_hours(raw.first, 24);
            if (ra_error_hours > 12.0) ra_error_hours -= 24.0;
            if (ra_error_hours < -12.0) ra_error_hours += 24.0;
            const double dec_error_degrees = dec - decode_angle(raw.second, 24);
            const double ra_error_arcsec = std::abs(ra_error_hours) * kHoursToDegrees * 3600.0;
            const double dec_error_arcsec = std::abs(dec_error_degrees) * 3600.0;
            if (ra_error_arcsec <= kLandingToleranceArcsec && dec_error_arcsec <= kLandingToleranceArcsec) {
                return true;
            }
            if (ra_error_arcsec > kLandingRefineLimitArcsec || dec_error_arcsec > kLandingRefineLimitArcsec) {
                ALPACA_LOG_WARN("SynScan", "GOTO ended " + std::to_string(ra_error_arcsec) + " arcsec (RA) and " +
                                               std::to_string(dec_error_arcsec) +
                                               " arcsec (Dec) from the target; too far for a landing refinement");
                return true;
            }
            if (pass == kMaxLandingRefinePasses) {
                const std::string detail = "SynScan GOTO landed " + std::to_string(ra_error_arcsec) +
                                           " arcsec (RA) and " + std::to_string(dec_error_arcsec) +
                                           " arcsec (Dec) off target after " + std::to_string(kMaxLandingRefinePasses) +
                                           " refinement passes";
                if (ra_error_arcsec > kLandingFailureArcsec || dec_error_arcsec > kLandingFailureArcsec) {
                    throw AlpacaException(detail, AlpacaError::DriverException);
                }
                ALPACA_LOG_WARN("SynScan", detail);
                return true;
            }
            aim_ra = std::fmod(aim_ra + ra_error_hours + 24.0, 24.0);
            aim_dec = std::clamp(aim_dec + dec_error_degrees, -90.0, 90.0);
            equatorial_cache_valid_ = false;
            altaz_cache_valid_ = false;
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + kRefineStartForce;
            guide_position_valid_ = false;
            protocol.goto_ra_dec_raw(encode_ra_raw(aim_ra, 24), encode_angle(aim_dec, 24), true);
            if (!wait_for_slew_complete(lock, owner_generation, false, async_task ? &slew_task_cancel_ : nullptr)) {
                return false;
            }
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
    SynScanVersion version_;
    mutable std::mutex mutex_;
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect
    std::atomic<bool> connected_;  // written under mutex_, read lock-free by get_connected()

    double target_ra_hours_;
    double target_dec_degrees_;
    double guide_position_ra_hours_ = 0.0;
    double guide_position_dec_degrees_ = 0.0;
    mutable bool guide_position_valid_ = false;
    double aperture_diameter_m_;
    double aperture_area_m2_;
    double focal_length_m_;
    mutable double cached_ra_hours_ = 0.0;
    mutable double cached_dec_degrees_ = 0.0;
    mutable double cached_alt_degrees_ = 0.0;
    mutable double cached_az_degrees_ = 0.0;
    mutable bool equatorial_cache_valid_ = false;
    mutable bool altaz_cache_valid_ = false;
    mutable util::PolledLinkHealth position_link_health_;
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
    mutable bool slewing_cached_ = false;
    // motion_generation_ of the GOTO whose landing refinement is under way; 0 when none (open-astro#880).
    mutable uint64_t refining_generation_ = 0;
    // open-astro#575: an async slew dispatch that fails AFTER
    // slew_to_coordinates_async() returned used to be logged and forgotten,
    // leaving Slewing read FALSE -- indistinguishable from a landed goto. Set
    // (under mutex_) by the slew task's catch block on a REAL failure (never
    // on the task's own cancellation), cleared by the next slew initiator and
    // by AbortSlew. Consulted by get_slewing() before the cached bool.
    mutable std::string last_slew_error_;
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
    // Read by get_name() on the HTTP thread with no driver lock while the
    // async connect sequence writes it under mutex_; whole-value atomic so
    // that read is not a data race (PR #281 review).
    std::atomic<int> mount_model_id_;
    bool use_precise_commands_;
    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
    const SynScanAlignmentSetting alignment_setting_;  // AZ-EQ geometry from the device config (#860)
    GuideRate guide_rate_{};
    mutable std::array<bool, 2> pulse_guiding_active_{};
    mutable std::array<std::chrono::steady_clock::time_point, 2> pulse_guide_end_time_{};

    bool park_position_set_ = false;
    mutable bool parking_ = false;  // park task in flight (Slewing true, AtPark false)
    AlignmentMode park_alignment_mode_ = AlignmentMode::GermanPolar;
    bool park_ra_uses_hour_angle_ = false;
    double park_hour_angle_hours_ = 0.0;
    // Bumped under mutex_ by every motion initiator; a sync slew waiting with the lock
    // released compares it to learn it was superseded (Sky-Watcher shape, #832).
    uint64_t motion_generation_ = 0;
    double park_ra_hours_ = 0.0;
    double park_dec_degrees_ = 0.0;
    double park_azimuth_degrees_ = 0.0;
    double park_altitude_degrees_ = 0.0;

    // Background task threads — see the helpers above. task_mutex_ only guards
    // thread handles and the cv; it is never held across protocol I/O.
    // Serializes motion handoffs: sync/async slews, park, PulseGuide, MoveAxis,
    // AbortSlew, Unpark, and disconnect. Never taken under mutex_; a blocking
    // slew releases it before waiting for motion completion.
    std::mutex initiator_mutex_;
    mutable std::mutex task_mutex_;
    mutable std::condition_variable task_cv_;
    std::thread slew_task_thread_;
    std::array<std::thread, 2> pulse_task_threads_;
    mutable std::atomic<bool> slew_task_cancel_{false};
    std::array<std::atomic<bool>, 2> pulse_task_cancel_{};
};

std::unique_ptr<TelescopeDriver> create_synscan_telescope(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version) {
    return create_synscan_telescope_with_site(device_number, connection_info, version, std::nullopt, std::nullopt,
                                              std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, SynScanVersion version,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
    SynScanAlignmentSetting alignment) {
    return std::make_unique<SynScanTelescopeDriver>(device_number, connection_info, version, site_latitude_deg,
                                                    site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                    alignment);
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver, SynScanVersion version,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
    SynScanAlignmentSetting alignment) {
    if (!connection_resolver) {
        throw AlpacaException("SynScan telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<SynScanTelescopeDriver>(device_number, ConnectionInfo{}, version, site_latitude_deg,
                                                    site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                    alignment, std::move(connection_resolver));
}

ConnectionInfo resolve_synscan_serial_auto(int mount_index) {
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
    return conn;
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_auto(
    int device_number, int mount_index, SynScanVersion version, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, SynScanAlignmentSetting alignment) {
    // The serial scan runs at connect time (#659), not here.
    return create_synscan_telescope_deferred(
        device_number, [mount_index] { return resolve_synscan_serial_auto(mount_index); }, version, site_latitude_deg,
        site_longitude_deg, site_elevation_m, sync_time_on_connect, alignment);
}

} // namespace alpacacore::vendor::synscan

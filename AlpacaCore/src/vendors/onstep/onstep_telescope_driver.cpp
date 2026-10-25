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
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/util/pointing.h>
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
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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
// open-astro#1430: how long Park and FindHome wait for the motion they stopped
// to end, and how long they retry a park or home OnStepX refuses as "in motion"
// (an axis brakes for about 0.4 s at 0.5 deg/s on the HM-17; a home that just
// ended keeps its guide state a moment longer).
constexpr auto kStopSettleTimeout = std::chrono::seconds(6);
constexpr auto kStopSettlePoll = std::chrono::milliseconds(100);
constexpr auto kInMotionRetryWindow = std::chrono::seconds(3);
constexpr auto kInMotionRetryPoll = std::chrono::milliseconds(200);
// OnStepX :GE# codes (docs/COMMAND_REFERENCE.md, "Command Error Codes").
constexpr int kCeNone = 0;
// The longest PulseGuide the :Mg[nsew]#### command can carry (#1478).
constexpr int kMaxPulseGuideMs = 9999;
constexpr int kCeSlewInMotion = 22;
constexpr int kCeTrue = 25;
constexpr int kCeNotParked = 11;

std::string describe_onstepx_error(int code) {
    switch (code) {
        case 8:
            return "not parked or at home";
        case 9:
            return "already parked";
        case 10:
            return "park failed";
        case 11:
            return "not parked";
        case 12:
            return "no park position set";
        case 13:
            return "goto failed";
        case 15:
            return "target below the horizon limit";
        case 16:
            return "target above the overhead limit";
        case 17:
            return "controller in standby";
        case 18:
            return "mount parked";
        case 19:
            return "goto already active";
        case 20:
            return "outside configured limits";
        case 21:
            return "hardware fault";
        case 22:
            return "mount still in motion";
        case 23:
            return "slew error";
        default:
            return "error code " + std::to_string(code);
    }
}

// open-astro#1414 (decision 0009): consecutive failed status or position reads
// that latch a link fault, as on SynScan and Celestron. A read only happens
// when a client asks, so the time to latch is the client's poll cadence times
// this, each failed read costing the response timeout.
constexpr int kLinkFaultThreshold = 3;
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

    ra_hours_out = util::wrap_hours(lst_hours - ha_hours);
    dec_deg_out = dec_rad * 180.0 / std::numbers::pi;
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
          site_elevation_m_(site_elevation_m.value_or(std::numeric_limits<double>::quiet_NaN())),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)) {}

    ~OnStepTelescopeDriver() override {
        // MUST be first, before members the async connection task touches
        // are destroyed (AsyncConnectable base contract). The other workers,
        // the MoveAxis re-send (#1378) and the Park/FindHome tail (#1430),
        // are joined next without mutex_ (they take mutex_ to send).
        shutdown_connection();
        move_axis_resend_.cancel_all_and_join();
        park_home_.cancel_all_and_join();
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

    std::string fallback_unique_id() const override { return "OnStep_" + std::to_string(device_number_); }

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

    // NotConnected exactly when Connected reads false, without mutex_.
    void check_connected_lock_free() const {
        if (!get_connected()) {
            throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
        }
    }

    // Why the serial link was lost, kept until the next Connect or Disconnect
    // (decision 0009). Wrapper state only: never waits on mutex_.
    std::string get_link_fault() const override {
        auto& protocol = OnStepProtocolWrapper::instance();
        (void)protocol.link_alive();  // notice an unplug the listing would otherwise miss
        const std::string reason = protocol.link_lost_reason();
        if (!reason.empty()) {
            return "Link to the mount lost (" + reason + "); reconnect";
        }
        // A latched fault: Connected still true, reads refused (#1414).
        std::lock_guard<std::mutex> fault_lock(link_fault_mutex_);
        return link_fault_text_.empty() ? std::string{} : "Mount not answering (" + link_fault_text_ + ")";
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
                        // The wrapper's reason (a port another device holds, #1202)
                        // goes to the operator.
                        const std::string reason = protocol.last_connect_error();
                        const std::string message = reason.empty() ? "Failed to connect to OnStep mount"
                                                                   : "Failed to connect to OnStep mount: " + reason;
                        // Refused TCP connect, vanished serial node, or a port another connected
                        // device now holds (#1202): stale, re-scan.
                        if (info.type == ConnectionType::Network || util::serial_endpoint_stale(info.port_path)) {
                            throw util::StaleEndpoint(message);
                        }
                        throw AlpacaException(message);
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
            mount_clock_unreadable_.reset();
            client_disagreement_warned_ = false;
            firmware_reports_guide_move_ = false;
            link_health_.reset();
            set_link_fault_text("");
            target_ra_set_ = false;
            target_dec_set_ = false;
            clear_manual_motion_locked();
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            pulse_guiding_active_ = false;
            tracking_before_slew_ = false;
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();

            // The two reads run without firmware_mutex_: get_device_firmware()
            // takes only that lock and must not wait for a serial round trip
            // (decision 0016). Only the store is under it.
            std::string firmware_version;
            std::string firmware;
            try {
                std::string product = protocol.get_product_name();
                firmware_version = protocol.get_version_number();
                firmware = product + (firmware_version.empty() ? "" : (" " + firmware_version));
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                firmware.clear();
            }
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = firmware;
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
            // The mount clock is read here, once, so UTCDate (and so DeviceState)
            // performs no serial exchange of its own (#1478); a written UTCDate needs none.
            if (!utc_offset_.valid()) {
                seed_utc_from_mount_locked();
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
        raise_park_home_error_locked(true);
        refresh_status_cache_locked(false);
        return cached_status_.is_at_home;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        raise_park_home_error_locked(false);
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

    // Rates are not supported (always 0), but an ASCOM read still throws
    // NotConnected while disconnected (#1478). Read lock-free, as Connected
    // is, so the getter never waits behind serial I/O under mutex_ (#1449).
    double get_declination_rate() const override {
        check_connected_lock_free();
        return 0.0;
    }

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
        refuse_while_faulted_locked("Tracking");
        if (tracking) {
            check_not_parked_locked("Tracking");
        }
        // The reply is read, not drained: OnStep 4.x refuses :ST while slewing
        // (#1379); the internal tracking restores stay blind.
        if (!OnStepProtocolWrapper::instance().set_tracking_checked(tracking)) {
            // With no :GE# code, refusal_code_locked() has just re-read :GU#: a
            // mount that now tracks as asked took it (#1379 review).
            if (const auto code = refusal_code_locked("Tracking");
                code && !(*code < 0 && cached_status_.is_tracking == tracking)) {
                throw mount_refused("Tracking", *code);
            }
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

    double get_right_ascension_rate() const override {
        check_connected_lock_free();  // see get_declination_rate()
        return 0.0;
    }

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
        // read at all (e.g. immediately after connect), or while the link
        // fault is latched (no cache is served then, #1414).
        ensure_site_info_cached_locked();
        if (!equatorial_cache_valid_ || link_health_.faulted()) {
            refresh_equatorial_cache_locked();  // faulted: never the cache (decision 0009 rule 1, #1414)
        }
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, cached_ra_hours_);
        return util::pier_side_for_hour_angle(hour_angle);
    }

    void set_side_of_pier(int side) override {
        (void)side;
        throw AlpacaException("Pier side is not settable", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        // Arguments first, even while disconnected (#1478).
        util::validate_ra_dec(ra, dec, "DestinationSideOfPier");
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_site_info_cached_locked();
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, ra);
        return util::pier_side_for_hour_angle(hour_angle);
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
        return util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
    }

    // Unset (NaN) until the device config or a client sets it: 0 m is a real
    // place (decision 0019, #1478). Lock-free: the protocol has no elevation
    // command, so this never waits on the mount.
    double get_site_elevation() const override {
        const double elevation = site_elevation_m_.load();
        if (std::isnan(elevation)) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return elevation;
    }

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
        util::validate_latitude(latitude);
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
        util::validate_longitude(longitude);
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
        if (park_home_request_) {
            return true;  // stopping, settling or retrying a Park or FindHome (#1430)
        }
        raise_park_home_error_locked(std::nullopt);
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
        util::validate_dec(dec, "TargetDeclination");
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
        util::validate_ra(ra, "TargetRightAscension");
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
            // The host clock is not the mount's (ITelescopeV4 UTCDate, #1478). The
            // mount clock is read once at connect, so a getter never waits on it.
            throw AlpacaException("The mount clock could not be read at connect" +
                                      (mount_clock_unreadable_ ? ": " + *mount_clock_unreadable_ : std::string()) +
                                      "; write UTCDate to set it",
                                  AlpacaError::InvalidOperation);
        }
        return current_utc_time_locked();
    }

    // At connect (#1478): seeds utc_offset_ from :GL#/:GC#/:GG#, or keeps why it
    // could not, for UTCDate to report until a UTCDate write or the next connect.
    void seed_utc_from_mount_locked() {
        try {
            TimeInfo info = OnStepProtocolWrapper::instance().get_time();
            using namespace std::chrono;
            int year = 2000 + info.year;
            sys_days date = sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(info.month)} /
                                     std::chrono::day{static_cast<unsigned>(info.day)}};
            auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
            auto offset = duration_cast<system_clock::duration>(
                std::chrono::duration<double, std::ratio<3600>>(info.utc_offset_hours));
            utc_offset_.set(local_time - offset, clock_.now());
            mount_clock_unreadable_.reset();
        } catch (const std::exception& ex) {
            mount_clock_unreadable_ = ex.what();
        } catch (...) {
            mount_clock_unreadable_ = "unknown error";
        }
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
    // Not waited for: the physical park or home, nor (#1430) the end of a
    // motion the call had to stop first or a park or home the mount refuses
    // as "in motion": those finish in the park_home_ body, and a refusal it
    // meets is kept and raised by Slewing, AtPark or AtHome (the shared rule
    // "A failed async operation body keeps its error").
    void find_home() override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        refuse_while_faulted_locked("FindHome");
        check_not_parked_locked("FindHome");
        refresh_status_cache_locked(true);
        if (cached_status_.is_at_home || cached_status_.is_homing) {
            return;  // there, or on its way
        }
        start_park_or_home_locked(lock, "FindHome", true);
    }

    void park() override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        refuse_while_faulted_locked("Park");
        refresh_status_cache_locked(true);
        if (cached_status_.is_parked || cached_status_.is_parking) {
            return;  // ASCOM: Park() on an already-parked mount is a no-op; one already parking too.
        }
        start_park_or_home_locked(lock, "Park", false);
    }

    void pulse_guide(int direction, int duration) override {
        // Arguments before the connected, fault and parked checks (#1478).
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }
        // The wire command's duration field is 4 digits (:Mgn####), so a
        // longer pulse cannot be sent: refuse it instead of pulsing for less
        // than asked (#1478).
        if (duration < 0 || duration > kMaxPulseGuideMs) {
            throw AlpacaException("PulseGuide duration must be 0 to " + std::to_string(kMaxPulseGuideMs) + " ms",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refuse_while_faulted_locked("PulseGuide");
        check_not_parked_locked("PulseGuide");
        // Hardware pulse guide: the mount times the pulse internally
        // (:Mgn####/:Mgs####/:Mge####/:Mgw####), so unlike a software-timed
        // implementation there is no background stop thread to manage here —
        // is_pulse_guiding() just compares against the known end time.
        OnStepProtocolWrapper::instance().pulse_guide(direction, duration);
        pulse_guiding_active_ = true;
        pulse_guide_end_time_ = clock_.now() + std::chrono::milliseconds(duration);
        equatorial_cache_valid_ = false;
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refuse_while_faulted_locked("SetPark");
        // OnStepX refuses :hQ# while parked, during a GOTO or a guide move, or on a
        // motor fault (Park.cpp set()); a refusal used to read as success (#1379).
        if (!OnStepProtocolWrapper::instance().set_park()) {
            // Unlike Unpark and Tracking, no :GU# letter shows the park position,
            // so a 0 with no :GE# code cannot be confirmed and stays a refusal.
            if (const auto code = refusal_code_locked("SetPark")) {
                throw mount_refused("SetPark", *code);
            }
        }
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

    // Under the target slew's own name, so a refusal names the member called (#1379).
    void slew_to_target() override {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto [ra, dec] = target_or_throw_locked();
        check_equatorial_slew_locked(ra, dec, "SlewToTarget");
        start_slew_locked(ra, dec, "SlewToTarget");
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_target_async() override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto [ra, dec] = target_or_throw_locked();
        check_equatorial_slew_locked(ra, dec, "SlewToTargetAsync");
        start_slew_locked(ra, dec, "SlewToTargetAsync");
    }

    std::pair<double, double> target_or_throw_locked() const {
        if (!target_ra_set_ || !target_dec_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        return {target_ra_hours_, target_dec_degrees_};
    }

    void sync_to_coordinates(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sync_locked(ra, dec, "SyncToCoordinates");
    }

    void sync_to_target() override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto [ra, dec] = target_or_throw_locked();
        sync_locked(ra, dec, "SyncToTarget");
    }

    // Under the member's own name (#1379). A sync during a GOTO is refused by the
    // mount (Goto.cpp validate(): CE_SLEW_IN_SLEW) and used to read as success, as
    // did every :CM# refusal; the driver refuses first and reads the reply.
    void sync_locked(double ra, double dec, const char* who) {
        util::validate_ra_dec(ra, dec, who);  // arguments first, even while disconnected (#1478)
        check_connected();
        refuse_while_faulted_locked(who);
        check_not_parked_locked(who);
        refuse_while_goto_running_locked(who);
        auto& protocol = OnStepProtocolWrapper::instance();
        // Use the mount's own sync command — do NOT maintain a driver-level
        // offset, which would cause coordinate divergence during subsequent
        // slews (project-wide rule; see the iOptron and Celestron files under .github/instructions/).
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        const std::string reply = protocol.sync_to_target();
        if (reply.size() >= 2 && reply[0] == 'E' && std::isdigit(static_cast<unsigned char>(reply[1]))) {
            throw digit_refused(who, "sync", ":CM#", reply[1], reply);
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        cached_ra_hours_ = ra;
        cached_dec_degrees_ = dec;
        equatorial_cache_valid_ = true;
        last_equatorial_update_ = clock_.now();
    }

    void unpark() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refuse_while_faulted_locked("Unpark");
        supersede_park_or_home_locked();  // a newer motion command wins (#1430)
        // A refusal used to read as success (#1379): OnStepX refuses :hR# before the
        // date and time are set or on a motor fault (Park.cpp restore()). "Not
        // parked" (11) is ASCOM's no-op, not a refusal.
        if (!OnStepProtocolWrapper::instance().unpark()) {
            // 11 (not parked) is ASCOM's no-op; with no :GE# code, the :GU# that
            // refusal_code_locked() has just re-read decides (#1379 review).
            if (const auto code = refusal_code_locked("Unpark");
                code && *code != kCeNotParked && !(*code < 0 && !cached_status_.is_parked)) {
                throw mount_refused("Unpark", *code);
            }
        }
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

        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;
        if (moving) {
            refuse_while_faulted_locked("MoveAxis");  // before it ends a waiting Park or FindHome
        }
        supersede_park_or_home_locked();  // a newer motion command wins (#1430)
        auto& protocol = OnStepProtocolWrapper::instance();

        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        const int positive_dir = (axis == 0) ? 2 : 0;
        const int negative_dir = (axis == 0) ? 3 : 1;

        if (!moving) {
            const std::string stop_error = stop_axis_locked(axis);
            if (!stop_error.empty()) {
                throw AlpacaException("MoveAxis stop failed: " + stop_error, AlpacaError::DriverException);
            }
        }
        std::uint64_t generation = 0;
        if (moving) {
            // Drop this axis's earlier move from the re-send first: the new
            // move sets it again only once its own command went out. This
            // only withdraws the old move, under mutex_ where no body runs;
            // decision 0006 rule 3's bump still comes before the new flags.
            manual_axis_resend_[axis] = false;
            const int dir = rate > 0.0 ? positive_dir : negative_dir;
            protocol.move_axis_start(dir, std::abs(rate));
            // Decision 0006 rule 3: supersede the running re-send under mutex_
            // before this move's flags change; start() below replaces it. Only
            // once the move went out: a failed send must leave the other
            // axis's re-send running.
            generation = move_axis_generation_.bump();
            manual_axis_dir_[axis] = dir;
            manual_axis_rate_[axis] = std::abs(rate);
            manual_axis_resend_[axis] = true;
            manual_axis_resend_due_[axis] = clock_.now() + kMoveAxisResendInterval;
            status_read_since_move_ = false;  // a :GU# from before the move must not end it (#1378)
        }

        manual_axis_slewing_[axis] = moving;
        if (!moving) {
            if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
                clear_manual_motion_locked();
            }
            // Sent, but a silent mount may not have heard it (#1414).
            refuse_while_faulted_locked("MoveAxis stop");
            return;
        }
        // Always a fresh body, never "already running": a body that has just
        // found nothing to re-send may still read as running while it exits.
        // The new one supersedes it.
        lock.unlock();  // start() without mutex_: decision 0006 rule 4, async_operation.h rule 10
        try {
            start_move_axis_resend();
        } catch (const AlpacaException& ex) {
            lock.lock();
            stop_move_without_resend_locked(axis, generation, ex);
        }
    }

    // Stops `axis` as MoveAxis(axis, 0) does and drops its re-send first: a
    // stop asks for none whatever its outcome. Defensive dual-stop, matching
    // the project's convention for fixed-direction (rather than signed-rate)
    // motion protocols. Both stops are always tried. The driver cannot tell
    // which direction the mount needed stopped, so any failed stop may be the
    // one that mattered: the first error is returned, and the caller keeps
    // the axis moving (#742). Empty when both went out.
    std::string stop_axis_locked(int axis) {
        manual_axis_resend_[axis] = false;
        auto& protocol = OnStepProtocolWrapper::instance();
        std::string stop_error;
        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        for (const int dir : {axis == 0 ? 2 : 0, axis == 0 ? 3 : 1}) {
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
        }
        return stop_error;
    }

    // #1458: the slot refused the re-send body (no thread, or replaced bodies
    // still finishing) after the move went out. The bump above has already
    // superseded the running body, so the move would end at the firmware's
    // time limit while Slewing read true: stop this axis and report it, as a
    // MoveAxis that did not happen. Nothing is stopped when a newer MoveAxis
    // went out meanwhile (the generation moved on), or when AbortSlew, a GOTO,
    // Park, FindHome or a disconnect ended the move meanwhile (the axis no
    // longer reads as moving): its stops would end that newer command. After
    // a link drop the stops fail and the message says so. The other axis keeps
    // moving, without a re-send until the next MoveAxis.
    void stop_move_without_resend_locked(int axis, std::uint64_t generation, const AlpacaException& refusal) {
        if (move_axis_generation_.current() != generation || !manual_axis_slewing_[axis]) {
            throw refusal;
        }
        std::string message = std::string("MoveAxis could not keep the move going (") + refusal.what() + ")";
        const std::string stop_error = stop_axis_locked(axis);
        if (stop_error.empty()) {
            manual_axis_slewing_[axis] = false;
            message += "; the axis was stopped";
        } else {
            message += "; stopping the axis failed too: " + stop_error;  // Slewing stays true (#742)
        }
        if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
            clear_manual_motion_locked();
        }
        ALPACA_LOG_WARN("OnStep", message);
        throw AlpacaException(message, AlpacaError::DriverException);
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
        supersede_park_or_home_locked();  // also ends a Park or FindHome still finishing (#1430)
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
        // The stops went out, but a silent mount may not have heard them
        // (#1414). Checked after the tracking decision, so a faulted abort
        // still gives up the restore instead of leaving it for a later slew end.
        refuse_while_faulted_locked("AbortSlew");
    }

    // An Alt/Az slew is the opposite of an equatorial one (open-astro#1274): ITelescopeV4
    // refuses it while Tracking is true (#1377), since a tracking mount drifts off the
    // requested Alt/Az. It converts to RA/Dec and runs the same GOTO.
    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        std::unique_lock<std::mutex> lock(mutex_);
        check_alt_az_slew_locked(altitude, azimuth, ra, dec, "SlewToAltAz");
        start_slew_locked(ra, dec, "SlewToAltAz");
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        std::lock_guard<std::mutex> lock(mutex_);
        check_alt_az_slew_locked(altitude, azimuth, ra, dec, "SlewToAltAzAsync");
        start_slew_locked(ra, dec, "SlewToAltAzAsync");
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void set_park_home_before_start_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        park_home_before_start_ = std::move(hook);
    }

    void set_move_axis_resend_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        move_axis_resend_.set_spawn_for_testing(std::move(spawn));
    }

    void set_park_home_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        park_home_.set_spawn_for_testing(std::move(spawn));
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
    // Slews, Park, FindHome and AbortSlew end the re-send here with cancel() rather
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
            // No 'g' since this connect and no pulse guide hiding it: firmware
            // without the letter, or a move that lapsed before this tick. The
            // body ends instead of reading :GU# every 7 s for nothing (#1461);
            // Slewing stays true until the client stops.
            // (After 'g' was seen, a read without it and without 'G' already
            // ended the move above.)
            return cached_status_.is_pulse_guide_active;
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

    // open-astro#1430: OnStepX refuses a park or a home while a guide move
    // (a MoveAxis, or its braking), a pulse guide, a GOTO or a home runs
    // (Park::request, Home::request: CE_SLEW_IN_MOTION). Park and FindHome
    // are asynchronous initiators (the 1 s STANDARD target), so they only do
    // what is quick here: when that motion runs they send AbortSlew's stops
    // (a stop that cannot be sent throws), and on a still mount they send the
    // park or home and read its answer at once, so a refusal a retry cannot
    // change (no park position set, outside limits) throws from the call.
    // The rest runs in park_home_ with mutex_ released between steps: wait
    // until :GU# shows the mount still (no 'g', 'G', GOTO or 'h'; up to
    // kStopSettleTimeout), then send and retry "in motion" refusals (up to
    // kInMotionRetryWindow). A refusal there is kept and raised by Slewing,
    // and by AtPark or AtHome, until the next motion command or a reconnect.
    // A newer motion command (supersede_park_or_home_locked()) ends the body
    // without sending anything. Park and home end with tracking off, so no
    // tracking restore is kept (#1218). The stops, the :GU# polls, :hP# and
    // :GE# (up to 1 s each without a reply) and :hC# hold mutex_, as every
    // OnStep exchange does; decision 0016 rule 1 calls that a defect, and
    // moving OnStep's I/O off mutex_ is #1449.
    void start_park_or_home_locked(std::unique_lock<std::mutex>& lock, const char* who, bool home) {
        supersede_park_or_home_locked();
        const std::uint64_t claim = park_home_claim_;
        const bool stopped = stop_motion_before_park_or_home_locked(who);
        int code = -1;
        if (!stopped && try_park_or_home_locked(who, home, code)) {
            finish_park_or_home_locked(home);
            return;
        }
        // Decision 0006 rule 3: claimed under mutex_ before the unlock, so a
        // newer motion command in the gap before start() still ends this one,
        // and Slewing reads true from here on. The body runs whatever request
        // is current when it starts, so a newer Park or FindHome whose start()
        // lands first is still carried out by the body started last.
        park_home_request_ = ParkHomeRequest{claim, who, home, stopped, code};
        // Test seam, read under mutex_.
        const auto before_start = park_home_before_start_;
        lock.unlock();  // start() without mutex_: decision 0006 rule 4
        if (before_start) {
            before_start();  // lands a call in the gap
        }
        try {
            park_home_.start([this](util::OperationContext& ctx) { run_park_or_home(ctx); });
        } catch (const AlpacaException&) {
            lock.lock();
            if (park_home_request_ && park_home_request_->claim == claim) {
                park_home_request_.reset();  // no body will finish it
            }
            throw;
        }
    }

    bool moving_locked() const {
        return manual_axis_slewing_[0] || manual_axis_slewing_[1] || cached_status_.is_slewing ||
               cached_status_.is_guide_active || cached_status_.is_pulse_guide_active || cached_status_.is_homing;
    }

    // Sends AbortSlew's stops when the mount moves: true when it did.
    bool stop_motion_before_park_or_home_locked(const char* who) {
        if (!moving_locked()) {
            return false;
        }
        auto& protocol = OnStepProtocolWrapper::instance();
        std::string stop_error;
        const auto try_stop = [&stop_error](const std::function<void()>& stop) {
            try {
                stop();
            } catch (const std::exception& ex) {
                if (stop_error.empty()) {
                    stop_error = ex.what();
                }
            }
        };
        try_stop([&protocol]() { protocol.abort_slew(); });
        for (int dir = 0; dir < 4; ++dir) {
            try_stop([&protocol, dir]() { protocol.move_axis_stop(dir); });
        }
        if (!stop_error.empty()) {
            // The mount may still be moving: leave Slewing as it was, but stop
            // re-sending a MoveAxis, as AbortSlew does (#781).
            manual_axis_resend_[0] = false;
            manual_axis_resend_[1] = false;
            throw AlpacaException(std::string(who) + ": could not stop the running motion first: " + stop_error,
                                  AlpacaError::DriverException);
        }
        clear_manual_motion_locked();
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        tracking_before_slew_ = false;
        return true;
    }

    // One park (:hP#, '1' or '0') or home (:hC#, no reply) try, with :GE#
    // read right after it, before any other command replaces the code
    // (OnStepX and OnStep 4.x have it). True when the mount took it. A
    // refusal a retry may change sets `code` (22 "in motion", or -1 for a
    // park on firmware without :GE#, whose reason is unknown) and returns
    // false; any other refusal throws InvalidOperation. Without a code a
    // home counts as started, as before.
    bool try_park_or_home_locked(const char* who, bool home, int& code) {
        auto& protocol = OnStepProtocolWrapper::instance();
        if (home) {
            protocol.find_home();
            code = last_command_error_locked(who).value_or(kCeNone);
        } else {
            if (protocol.park()) {
                return true;
            }
            code = last_command_error_locked(who).value_or(-1);
        }
        if (code == kCeNone || code == kCeTrue) {
            return true;  // for a park: the reply was lost or garbled, but the mount took it
        }
        if (code != kCeSlewInMotion && !(code < 0 && !home)) {
            throw mount_refused(who, code);
        }
        return false;
    }

    // :GE# with no code is firmware without it, or a mount that stopped
    // answering (#1414): a :GU# read tells them apart. A silent mount counts
    // as a failed read toward the link fault, and the park or home fails
    // instead of reading as accepted.
    std::optional<int> last_command_error_locked(const char* who) {
        auto& protocol = OnStepProtocolWrapper::instance();
        if (const auto code = protocol.get_last_command_error()) {
            return code;
        }
        try {
            cached_status_ = protocol.get_status();
            status_cache_valid_ = true;
            last_status_update_ = clock_.now();
            note_read_ok_locked();
        } catch (const std::exception& ex) {
            note_read_failure_locked(ex);  // throws once the fault latched or the link went
            if (const auto* alpaca = dynamic_cast<const AlpacaException*>(&ex);
                alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected) {
                throw;  // the link is gone: NotConnected, as for any other call
            }
            throw AlpacaException(std::string(who) + ": the mount did not answer after the command (" + ex.what() + ")",
                                  AlpacaError::DriverException);
        }
        return std::nullopt;
    }

    // A numeric command the mount answered '0' (or garbled): the reason is the :GE#
    // code. As for :hP# (#1430), a :GE# that then says 0 or 25 means it was taken.
    // Returns the refusal code (-1 when :GE# gave none), or nullopt when accepted.
    std::optional<int> refusal_code_locked(const char* who) {
        const int code = last_command_error_locked(who).value_or(-1);
        if (code == kCeNone || code == kCeTrue) {
            return std::nullopt;
        }
        return code;
    }

    static AlpacaException mount_refused(const char* who, int code) {
        return AlpacaException(std::string(who) + " refused by the mount (" +
                                   (code < 0 ? std::string("no reason given") : describe_onstepx_error(code)) + ")",
                               AlpacaError::InvalidOperation);
    }

    // The mount took the park or home: it now moves to it with tracking off.
    void finish_park_or_home_locked(bool home) {
        tracking_before_slew_ = false;  // never restart tracking afterwards (#1218)
        cached_status_.is_at_home = false;
        cached_status_.is_slewing = true;
        if (!home) {
            cached_status_.is_parked = false;
            clear_manual_motion_locked();
        }
        status_cache_valid_ = true;
        last_status_update_ = clock_.now();
    }

    // The park_home_ body: settle (when the call sent stops), then send and
    // retry. Waits on the task clock with mutex_ released (decision 0005).
    void run_park_or_home(util::OperationContext& ctx) {
        std::unique_lock<std::mutex> lock(mutex_);
        // A body stopped before it runs takes no request: a newer Park or
        // FindHome may have stored its own after cancelling this body and
        // before its start() marked this one superseded, and this body must
        // not end that request (#1430). Leaving it is safe because every way
        // this slot is cancelled also clears or replaces the request
        // (supersede_park_or_home_locked(), a newer start): keep it so, or a
        // request no body runs leaves Slewing true.
        if (!park_home_request_ || ctx.stop_reason() != util::StopReason::None) {
            return;  // superseded or cancelled, or carried out by an earlier body
        }
        const ParkHomeRequest request = *park_home_request_;
        run_park_or_home_locked(lock, ctx, request.who, request.home, request.stopped, request.code, request.claim);
        // Done, unless a newer start() superseded this body: the body it
        // started runs the request. A newer claim already replaced it.
        if (ctx.stop_reason() != util::StopReason::Superseded && park_home_request_ &&
            park_home_request_->claim == request.claim) {
            park_home_request_.reset();
        }
    }

    // The steps of the body; returns with `lock` held.
    void run_park_or_home_locked(std::unique_lock<std::mutex>& lock, util::OperationContext& ctx, const char* who,
                                 bool home, bool settle, int code, std::uint64_t claim) {
        const auto still_ours = [&] {
            return ctx.stop_reason() == util::StopReason::None && connected_ && park_home_claim_ == claim;
        };
        const auto wait = [&](std::chrono::milliseconds d) {
            lock.unlock();
            const bool woke = ctx.wait_for(d);
            lock.lock();
            return woke && still_ours();
        };
        try {
            if (!still_ours()) {
                return;
            }
            if (settle) {
                const auto settle_deadline = clock_.now() + kStopSettleTimeout;
                for (;;) {
                    refresh_status_cache_locked(true);
                    if (!moving_locked() || clock_.now() >= settle_deadline) {
                        break;  // still moving at the deadline: the refusal reports it
                    }
                    if (!wait(kStopSettlePoll)) {
                        return;
                    }
                }
            }
            const auto deadline = clock_.now() + kInMotionRetryWindow;
            bool try_now = settle;
            for (;;) {
                if (!try_now) {
                    if (clock_.now() >= deadline) {
                        throw mount_refused(who, code);
                    }
                    if (!wait(kInMotionRetryPoll)) {
                        return;
                    }
                }
                try_now = false;
                if (try_park_or_home_locked(who, home, code)) {
                    finish_park_or_home_locked(home);
                    return;
                }
            }
        } catch (const AlpacaException& ex) {
            if (still_ours()) {
                park_home_error_ = KeptParkHomeError{home, ex.what(), ex.error_code()};
            }
        }
    }

    // A newer motion command or a reconnect: a Park or FindHome still
    // finishing ends without sending anything, and its kept refusal is
    // dropped. cancel() never blocks, so mutex_ may be held (rule 10).
    void supersede_park_or_home_locked() {
        park_home_error_.reset();
        ++park_home_claim_;
        park_home_request_.reset();
        park_home_.cancel();
    }

    // Raises a refusal the park_home_ body kept: any for Slewing, only a
    // park's for AtPark and a home's for AtHome.
    void raise_park_home_error_locked(std::optional<bool> home) const {
        if (park_home_error_ && (!home || *home == park_home_error_->home)) {
            throw AlpacaException(park_home_error_->message, park_home_error_->code);
        }
    }

    // The per-session state a disconnect or a lost link leaves behind.
    void reset_session_state_locked() {
        supersede_park_or_home_locked();
        link_health_.reset();
        set_link_fault_text("");
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
        mount_clock_unreadable_.reset();
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
        util::validate_ra_dec(ra, dec, who);  // arguments first, even while disconnected (#1478)
        check_connected();
        check_not_parked_locked(who);
        refuse_while_faulted_locked(who);  // named, before the Tracking read (#1414)
        refresh_status_cache_locked(false);
        if (!cached_status_.is_tracking) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // (#1377). Same order as the equatorial check: arguments, connected, parked,
    // fault, then Tracking read as the getter reads it, before anything is sent.
    void check_alt_az_slew_locked(double altitude, double azimuth, double& ra_out, double& dec_out,
                                  const char* who) const {
        validate_alt_az(altitude, azimuth);  // arguments first, even while disconnected (#1478)
        check_connected();
        check_not_parked_locked(who);
        refuse_while_faulted_locked(who);  // named, before the site and Tracking reads (#1414)
        compute_alt_az_target_locked(altitude, azimuth, ra_out, dec_out);
        refresh_status_cache_locked(false);
        if (cached_status_.is_tracking) {
            throw AlpacaException(std::string(who) + " is not allowed while Tracking is true",
                                  AlpacaError::InvalidOperation);
        }
    }

    // A GOTO sent while one runs makes OnStepX stop the running one and refuse the
    // new one (Goto.cpp request(): CE_SLEW_IN_SLEW -> abort(), :MS# reply 5), so the
    // driver refuses first and leaves it running (#1379): its own GOTO (also in the
    // grace before :GU# shows it), a park or home already moving, or a handset GOTO.
    void refuse_while_goto_running_locked(const char* who) const {
        bool running = clock_.now() < slew_force_until_;
        if (!running) {
            refresh_status_cache_locked(false);
            running = cached_status_.is_slewing;
        }
        if (running) {
            throw AlpacaException(std::string(who) + " is not allowed while a GOTO is running",
                                  AlpacaError::InvalidOperation);
        }
    }

    // The :MS# refusal digit (OnStepX Goto.command.cpp) as the error the client sees (#1379).
    static AlpacaException slew_refused(const char* who, const std::string& reply) {
        return digit_refused(who, "GOTO", ":MS#", reply.empty() ? '?' : reply[0], reply);
    }

    // The :MS# digits, also behind the E of a :CM# refusal (OnStep Command.ino).
    static AlpacaException digit_refused(const char* who, const char* what, const char* command, char code,
                                         const std::string& reply) {
        const bool is_goto = std::string(what) == "GOTO";
        const char* why = "an unknown reason";
        switch (code) {
            case '1':
                why = "the target is below the horizon limit";
                break;
            case '2':
                why = "the target is above the overhead limit";
                break;
            case '3':
                why = "the controller is in standby";
                break;
            case '4':
                why = "the mount is parked";
                break;
            case '5':
                why = is_goto ? "a GOTO was already running, and the mount stopped it" : "a GOTO is running";
                break;
            case '6':
                why = "the target is outside the limits";
                break;
            case '7':
                why = "a hardware fault";
                break;
            case '8':
                why = "the mount is already moving";
                break;
            case '9':
                why = "an unspecified error";
                break;
            default:
                break;
        }
        return AlpacaException(std::string(who) + ": the mount refused the " + what + ": " + why + " (" + command +
                                   " reply '" + reply + "')",
                               code == '4' ? AlpacaError::InvalidWhileParked : AlpacaError::InvalidOperation);
    }

    void check_not_parked_locked(const char* operation) const {
        if (cached_status_.is_parked) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    static void validate_alt_az(double altitude, double azimuth) {
        if (!std::isfinite(altitude) || altitude < -90.0 || altitude > 90.0) {
            throw AlpacaException("Altitude out of range", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth > 360.0) {
            throw AlpacaException("Azimuth out of range", AlpacaError::InvalidValue);
        }
    }

    void compute_alt_az_target_locked(double altitude, double azimuth, double& ra_out, double& dec_out) const {
        validate_alt_az(altitude, azimuth);
        ensure_site_info_cached_locked();
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        alt_az_to_ra_dec(altitude, azimuth, site_latitude_cached_, lst, ra_out, dec_out);
    }

    void start_slew_locked(double ra, double dec, const char* context) {
        util::validate_ra_dec(ra, dec, context);
        refuse_while_faulted_locked(context);
        refuse_while_goto_running_locked(context);  // before anything is cancelled or sent (#1379)
        supersede_park_or_home_locked();            // a newer motion command wins (#1430)
        auto& protocol = OnStepProtocolWrapper::instance();
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        const std::string reply = protocol.slew_to_target();
        if (reply.empty() || reply[0] != '0') {
            throw slew_refused(context, reply);
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
            } catch (const AlpacaException& ex) {
                if (link_health_.faulted() || ex.error_code() == AlpacaError::NotConnected) {
                    throw;  // a latched fault or a lost link is reported, as by every read (#1414)
                }
                return true;  // keep reporting the move; one read error is not a stop
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

    // open-astro#1414, decision 0009: a mount that stops answering while its
    // adapter stays plugged in (mount unpowered, cable pulled at the mount)
    // was served from the caches for ever, each read waiting out the
    // response timeout. kLinkFaultThreshold failed reads in a row latch a
    // fault: Connected stays true, and reads throw DriverException with the
    // fault instead of serving the cache. A fault that has stood for
    // util::kLinkStalenessBound is a lost link: the wrapper records it and
    // closes the port as for a pulled adapter (#1241), so Connected reads
    // false and calls throw NotConnected. Only the client's Connect relinks.
    // While faulted no cache is served: every refresh skips its TTL and asks
    // the mount, so a read either clears the fault or throws; on a polled
    // link those reads are the only thing that can clear it. Time is the task
    // clock.
    void note_read_ok_locked() const {
        if (link_health_.on_reply()) {
            ALPACA_LOG_INFO("OnStep", "Mount answering again; link fault cleared");
            set_link_fault_text("");
        }
    }

    void note_read_failure_locked(const std::exception& ex) const {
        if (const auto* alpaca = dynamic_cast<const AlpacaException*>(&ex);
            alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected) {
            return;  // the link is already gone (#1241); not a silent mount
        }
        const auto now = clock_.now();
        if (const auto latched = link_health_.note_failure(ex.what(), kLinkFaultThreshold, now)) {
            ALPACA_LOG_ERROR("OnStep", "Mount not answering; link fault latched: " + *latched);
            set_link_fault_text(*latched);
        } else if (!link_health_.faulted()) {
            ALPACA_LOG_WARN("OnStep", "Mount read failed (" + std::to_string(link_health_.consecutive_failures()) +
                                          " in a row): " + ex.what());
        }
        lose_link_if_stale_locked(now);
        if (link_health_.faulted()) {
            throw AlpacaException("OnStep mount communications compromised: " + link_health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    // A fault that has stood past util::kLinkStalenessBound is a lost link:
    // the wrapper closes the port and this throws NotConnected.
    void lose_link_if_stale_locked(std::chrono::steady_clock::time_point now) const {
        if (!link_health_.fault_stale(now, util::kLinkStalenessBound)) {
            return;
        }
        const std::string reason =
            "no reply for " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
            " s: " + link_health_.fault();
        OnStepProtocolWrapper::instance().declare_link_lost(reason);  // Connected now reads false
        throw AlpacaException("OnStep link lost (" + reason + "); set Connected true to reconnect",
                              AlpacaError::NotConnected);
    }

    // Decision 0009 rule 1: while the fault is latched, writes throw as reads
    // do. Blind commands cannot fail on a healthy fd, so without this a
    // Tracking, Unpark or a slew would report success to a mount that is
    // not listening. AbortSlew and MoveAxis(axis, 0) send their stops first
    // and call this after, so the stop is still tried (#1414).
    void refuse_while_faulted_locked(const char* what) const {
        if (link_health_.faulted()) {
            // A client that only writes still sees the link lost once the
            // fault has stood past the bound, as a read would (#1414).
            lose_link_if_stale_locked(clock_.now());
            throw AlpacaException(
                std::string(what) + ": OnStep mount communications compromised: " + link_health_.fault(),
                AlpacaError::DriverException);
        }
    }

    void set_link_fault_text(const std::string& text) const {
        std::lock_guard<std::mutex> fault_lock(link_fault_mutex_);
        link_fault_text_ = text;
    }

    void refresh_status_cache_locked(bool force) const {
        auto now = clock_.now();
        if (!force && status_cache_valid_ && !link_health_.faulted() && (now - last_status_update_) < kStatusCacheTtl) {
            return;
        }
        try {
            cached_status_ = OnStepProtocolWrapper::instance().get_status();
            status_cache_valid_ = true;
            status_read_since_move_ = true;
            last_status_update_ = now;
            note_read_ok_locked();
        } catch (const std::exception& ex) {
            note_read_failure_locked(ex);
            if (!status_cache_valid_) {
                throw;
            }
        }
    }

    void refresh_equatorial_cache_locked() const {
        auto now = clock_.now();
        if (equatorial_cache_valid_ && !link_health_.faulted() && (now - last_equatorial_update_) < kPositionCacheTtl) {
            return;
        }
        try {
            Position pos = OnStepProtocolWrapper::instance().get_position();
            cached_ra_hours_ = pos.ra_hours;
            cached_dec_degrees_ = pos.dec_degrees;
            equatorial_cache_valid_ = true;
            last_equatorial_update_ = now;
            note_read_ok_locked();
        } catch (const std::exception& ex) {
            note_read_failure_locked(ex);
            if (!equatorial_cache_valid_) {
                throw;
            }
        }
    }

    void refresh_altaz_cache_locked() const {
        auto now = clock_.now();
        if (altaz_cache_valid_ && !link_health_.faulted() && (now - last_altaz_update_) < kPositionCacheTtl) {
            return;
        }
        try {
            AltAz altaz = OnStepProtocolWrapper::instance().get_alt_az();
            cached_alt_degrees_ = altaz.altitude_degrees;
            cached_az_degrees_ = altaz.azimuth_degrees;
            altaz_cache_valid_ = true;
            last_altaz_update_ = now;
            note_read_ok_locked();
        } catch (const std::exception& ex) {
            note_read_failure_locked(ex);
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
        mount_clock_unreadable_.reset();
        // The mount now runs on the client's clock and so does the LST above;
        // on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("OnStep", client_minus_host, client_disagreement_warned_);
    }

    // The UTC the LST computations use: a written or mount-read UTCDate, else
    // the host clock. The host clock is not stored in utc_offset_, so UTCDate
    // never reports it as the mount's (#1478).
    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (const auto utc = utc_offset_.current(clock_.now())) {
            return *utc;
        }
        return std::chrono::system_clock::now();
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
    std::atomic<double> site_elevation_m_;  // NaN until set or configured

    // Decision 0014: the UTCDate a client wrote, or the mount clock the connect
    // read (seed_utc_from_mount_locked()), and the task-clock time since.
    // Cleared on connect and disconnect. Under mutex_.
    mutable util::UtcOffset utc_offset_;
    // Why the connect could not read the mount clock (#1478); under mutex_,
    // cleared on connect, disconnect and a UTCDate write.
    std::optional<std::string> mount_clock_unreadable_;

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

    // Decision 0009 fault latch for a silent mount (#1414). Under mutex_.
    mutable util::PolledLinkHealth link_health_;
    // Its text for get_link_fault(), which takes no mutex_: own narrow mutex.
    mutable std::mutex link_fault_mutex_;
    mutable std::string link_fault_text_;

    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so
    // the get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;

    // Last, so it is destroyed (and its body joined) before every member the
    // body touches. The destructor joins it explicitly as well.
    util::OperationGeneration move_axis_generation_;
    mutable util::AsyncOperation move_axis_resend_{"OnStep MoveAxis re-send", move_axis_generation_, clock_};

    // #1430: a refusal the park_home_ body met, raised by Slewing and by
    // AtPark (a park's) or AtHome (a home's). Under mutex_.
    struct KeptParkHomeError {
        bool home;
        std::string message;
        int code;
    };
    std::optional<KeptParkHomeError> park_home_error_;
    // The current Park/FindHome's claim (decision 0006 rule 3), bumped by
    // every supersede, and the request its background part is still to
    // finish. Under mutex_.
    struct ParkHomeRequest {
        std::uint64_t claim;
        const char* who;
        bool home;
        bool stopped;
        int code;
    };
    std::uint64_t park_home_claim_ = 0;
    std::optional<ParkHomeRequest> park_home_request_;
    // Test seam: runs between the unlock and park_home_.start(). Set only
    // while no Park or FindHome is in flight.
    std::function<void()> park_home_before_start_;
    // The tail of a Park or FindHome that must first wait for motion to end
    // (#1430). Declared last with the re-send slot, for the same reason.
    util::OperationGeneration park_home_generation_;
    mutable util::AsyncOperation park_home_{"OnStep Park/FindHome", park_home_generation_, clock_};
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

void set_park_home_before_start_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_park_home_before_start_for_testing(std::move(hook));
    }
}

void set_move_axis_resend_spawn_for_testing(TelescopeDriver& driver,
                                            std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_resend_spawn_for_testing(std::move(spawn));
    }
}

void set_park_home_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_park_home_spawn_for_testing(std::move(spawn));
    }
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

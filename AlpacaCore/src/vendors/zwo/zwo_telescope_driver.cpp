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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace alpacacore::vendor::zwo {

namespace {

constexpr double kSiderealRateDegPerSec = 360.0 / 86164.0905;

// A device reply quoted in an error or log line: control and non-ASCII bytes become '?', and it is cut at 48
// characters, so a hostile reply cannot forge log lines (CWE-117).
std::string sanitize_for_log(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        if (out.size() >= 48) break;
        out.push_back(ch >= 0x20 && ch < 0x7f ? ch : '?');
    }
    return out;
}
constexpr double kMaxMoveAxisRateDegPerSec = kSiderealRateDegPerSec * 1440.0;
constexpr double kHoursToDegrees = 15.0;
constexpr double kMinSiteElevationMeters = -300.0;
constexpr double kMaxSiteElevationMeters = 10000.0;
constexpr auto kDefaultSlewTimeout = std::chrono::minutes(15);
constexpr auto kParkMotionSettle = std::chrono::seconds(2);
constexpr auto kPulseGuideHold = std::chrono::milliseconds(500);

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
    const double days_since_epoch = duration<double>(utc_time.time_since_epoch()).count() / 86400.0;
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

double normalize_hour_angle_hours(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped <= -12.0) {
        wrapped += 24.0;
    } else if (wrapped > 12.0) {
        wrapped -= 24.0;
    }
    return wrapped;
}

} // namespace

class ZWOTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
    struct PolledFrame {
        std::optional<StatusInfo> status;
        std::optional<EquatorialCoordinates> equatorial;
        std::optional<HorizontalCoordinates> horizontal;
        std::optional<bool> tracking;
        std::optional<bool> parked;
        std::optional<int> pier_side;
        std::optional<util::TaskClock::clock::time_point> mandatory_at;
    };

public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                       std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                       std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                       util::TaskClock& clock)
        : AsyncConnectable("ZWO"),
          device_number_(device_number),
          connection_info_(connection_info),
          clock_(clock),
          snapshot_(clock, kSnapshotMaxAge),
          poll_operation_("ZWO telemetry", poll_generation_, clock),
          connected_(false),
          caches_ready_(false),
          mount_info_(),
          target_ra_hours_(0.0),
          target_dec_degrees_(0.0),
          target_ra_set_(false),
          target_dec_set_(false),
          ra_offset_hours_(0.0),
          dec_offset_deg_(0.0),
          aperture_diameter_m_(0.0),
          aperture_area_m2_(0.0),
          focal_length_m_(0.0),
          site_latitude_deg_(site_latitude_deg.value_or(0.0)),
          site_longitude_deg_(site_longitude_deg.value_or(0.0)),
          site_coords_valid_(site_latitude_deg.has_value() && site_longitude_deg.has_value()),
          site_elevation_m_(site_elevation_m),
          slew_settle_time_s_(0),
          guide_rate_({0.5 * kSiderealRateDegPerSec, 0.5 * kSiderealRateDegPerSec}),
          tracking_rate_cached_(0),
          tracking_rate_valid_(false),
          tracking_state_cached_(false),
          tracking_state_valid_(false),
          tracking_state_at_(std::chrono::steady_clock::time_point{}),
          cached_equatorial_(),
          cached_status_(),
          park_state_cached_(),
          cached_equatorial_at_(std::chrono::steady_clock::time_point{}),
          park_state_at_(std::chrono::steady_clock::time_point{}),
          poll_stop_(false),
          poll_pause_(false),
          last_utc_set_(std::chrono::system_clock::time_point{}),
          last_utc_set_monotonic_(std::chrono::steady_clock::time_point{}),
          last_utc_valid_(false),
          timezone_offset_minutes_(0),
          timezone_valid_(false),
          manual_axis_slewing_({false, false}),
          manual_axis_tracking_restore_({std::nullopt, std::nullopt}),
          slew_force_until_(std::chrono::steady_clock::time_point{}),
          pulse_guiding_end_(std::chrono::steady_clock::time_point{}),
          pulse_generation_(0),
          pulse_thread_stop_(false),
          pulse_cancel_(false),
          pulse_queue_end_(std::chrono::steady_clock::time_point{}),
          parked_cached_(false),
          park_command_active_(false),
          park_command_started_(std::chrono::steady_clock::time_point{}),
          park_motion_seen_(false),
          unpark_in_progress_(false),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(true)) {}

    ~ZWOTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error while disconnecting ZWO mount driver: " + std::string(e.what()));
            }
        }
        // Join EVERY background thread before members are destroyed: the
        // GOTO setup thread, the async disconnect teardown (which itself
        // joins the poll/pulse threads), and — if we were never connected or
        // a teardown never ran — the poll/pulse threads directly.
        cancel_goto_thread_request();
        if (goto_thread_.joinable()) {
            goto_thread_.join();
        }
        if (disconnect_thread_.joinable()) {
            disconnect_thread_.join();
        }
        stop_poll_thread();
        stop_pulse_thread();
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

    std::string fallback_unique_id() const override {
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

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        // The handshake must finish before Connected is visible; a failed warm-up leaves telemetry unavailable.
        return connected_.load() && caches_ready_.load();
    }

    std::string get_link_fault() const override {
        update_link_state();
        std::lock_guard<std::mutex> lock(mutex_);
        return link_fault_locked();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
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

        // Serialize whole set_connected bodies (sync PUT, async-task tail,
        // Connected=true refresh). The poll/pulse/teardown thread members
        // below are joined, re-assigned and flag-gated here; two overlapping
        // bodies (e.g. the async connect task's start_poll_thread racing a
        // sync disconnect teardown's stop_poll_thread) would join/assign the
        // same std::thread from two threads and re-clear poll_stop_ under the
        // other side's join — UB and a permanent wedge (found by the
        // [stress] telescope suite). No worker-thread body takes this mutex.
        std::lock_guard<std::mutex> lifecycle_lock(set_connected_mutex_);

        if (!connected) {
            connected_.store(false);
            caches_ready_.store(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.reset();
                health_.reset();
                loss_reason_.clear();
            }
            poll_stop_.store(true);
            pulse_thread_stop_.store(true);
            pulse_cancel_.store(true);
            pulse_cv_.notify_all();
            cancel_goto_thread_request();
            protocol_.cancel_pending_io();
            // open-astro#720: fence before teardown, so a pending GOTO setup
            // cannot send ":MS" after Connected=false returns.
            cancel_and_join_goto_thread();

            // Joinable member thread, NOT detached: the destructor (and the
            // next connect) joins it, so the teardown can never touch a
            // destroyed driver (C2). Reap a finished teardown from a previous
            // disconnect before reusing the member.
            if (disconnect_thread_.joinable()) {
                disconnect_thread_.join();
            }
            disconnect_thread_ = std::thread([this]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    manual_axis_slewing_[0] = false;
                    manual_axis_slewing_[1] = false;
                    slew_force_until_ = std::chrono::steady_clock::time_point{};
                    pulse_guiding_end_ = std::chrono::steady_clock::time_point{};
                    ra_offset_hours_ = 0.0;
                    dec_offset_deg_ = 0.0;
                    park_command_active_ = false;
                    park_motion_seen_ = false;
                    parked_cached_ = false;
                    park_command_started_ = std::chrono::steady_clock::time_point{};
                    park_state_cached_.reset();
                    park_state_at_ = std::chrono::steady_clock::time_point{};
                    tracking_state_valid_ = false;
                    tracking_rate_valid_ = false;
                    tracking_state_at_ = std::chrono::steady_clock::time_point{};
                    manual_axis_tracking_restore_[0] = std::nullopt;
                    manual_axis_tracking_restore_[1] = std::nullopt;
                    target_ra_hours_ = 0.0;
                    target_dec_degrees_ = 0.0;
                    target_ra_set_ = false;
                    target_dec_set_ = false;
                    clear_last_slew_error_locked();
                }
                stop_poll_thread();
                stop_pulse_thread();
                try {
                    protocol_.disconnect();
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "Disconnect failed: " + std::string(e.what()));
                }
            });
            return;
        }

        // Serialize with a still-running async disconnect teardown so its
        // state wipe / protocol disconnect cannot interleave with this
        // connect (the teardown takes mutex_ too, but only join gives a
        // strict ordering).
        if (disconnect_thread_.joinable()) {
            disconnect_thread_.join();
        }

        // set_connected_mutex_ is this driver's transition lock and the protocol wrapper's own mutex is
        // its I/O lock: the handshake below runs under them and never under mutex_, which each step
        // takes for the short state publish only (decision 0016).
        auto& protocol = protocol_;
        auto reset_session_state_for_connect = [&](bool keep_telemetry_caches = false) {
            std::lock_guard<std::mutex> pulse_lock(pulse_mutex_);
            std::lock_guard<std::mutex> lock(mutex_);
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            slew_force_until_ = std::chrono::steady_clock::time_point{};
            pulse_guiding_end_ = std::chrono::steady_clock::time_point{};
            ra_offset_hours_ = 0.0;
            dec_offset_deg_ = 0.0;
            if (!keep_telemetry_caches) {
                snapshot_.reset();
                health_.reset();
                (void)link_lost_at_.consume();
                loss_reason_.clear();
                last_mandatory_at_ = clock_.now();
                poll_stop_.store(false);
                clear_last_slew_error_locked();
            }
            if (!keep_telemetry_caches) {
                // open-astro#409: once per REAL connection. The
                // keep_telemetry_caches=true caller is the "Connected=true
                // while already connected" no-op, and re-arming there would
                // log a line per poll for a client that re-sends both.
                client_disagreement_warned_ = false;
                cached_equatorial_.reset();
                cached_status_.reset();
                park_state_cached_.reset();
                cached_equatorial_at_ = std::chrono::steady_clock::time_point{};
                park_state_at_ = std::chrono::steady_clock::time_point{};
                tracking_state_valid_ = false;
                tracking_state_at_ = std::chrono::steady_clock::time_point{};
            }
            poll_pause_.store(false);
            pulse_queue_.clear();
            pulse_queue_end_ = std::chrono::steady_clock::time_point{};
            pulse_thread_stop_.store(false);
            pulse_cancel_.store(false);
            park_command_active_ = false;
            park_command_started_ = std::chrono::steady_clock::time_point{};
            park_motion_seen_ = false;
            unpark_in_progress_ = false;
            if (!keep_telemetry_caches) {
                // parked_cached_ is the sticky "inferred parked" flag — only
                // clear it on a full (fresh) connect, never on a no-op
                // reconnect, or a redundant Connected=true while parked would
                // silently un-park the driver once the short park_state_cached_
                // TTL elapses.
                parked_cached_ = false;
                park_state_cached_.reset();
                park_state_at_ = std::chrono::steady_clock::time_point{};
                tracking_state_valid_ = false;
                tracking_state_at_ = std::chrono::steady_clock::time_point{};
            }
            tracking_rate_valid_ = false;
            manual_axis_tracking_restore_[0] = std::nullopt;
            manual_axis_tracking_restore_[1] = std::nullopt;
            target_ra_hours_ = 0.0;
            target_dec_degrees_ = 0.0;
            target_ra_set_ = false;
            target_dec_set_ = false;
            pulse_generation_.store(0);
        };
        auto apply_or_load_site_info = [&]() {
            // Held across the whole send and read-back, so a setter's write lands before or after this
            // exchange and the read-back never overwrites it.
            std::lock_guard<std::mutex> site_write(site_write_mutex_);
            std::optional<double> pending_latitude;
            std::optional<double> pending_longitude;
            bool coords_valid = false;
            double known_latitude = 0.0;
            double known_longitude = 0.0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_latitude = pending_site_latitude_;
                pending_longitude = pending_site_longitude_;
                coords_valid = site_coords_valid_;
                known_latitude = site_latitude_deg_;
                known_longitude = site_longitude_deg_;
            }
            std::optional<SiteInfo> preferred_site;
            if (pending_latitude.has_value() || pending_longitude.has_value() || coords_valid) {
                SiteInfo site;
                bool has_latitude = false;
                bool has_longitude = false;
                if (pending_latitude.has_value()) {
                    site.latitude_degrees = pending_latitude.value();
                    has_latitude = true;
                } else if (coords_valid) {
                    site.latitude_degrees = known_latitude;
                    has_latitude = true;
                }
                if (pending_longitude.has_value()) {
                    site.longitude_degrees = pending_longitude.value();
                    has_longitude = true;
                } else if (coords_valid) {
                    site.longitude_degrees = known_longitude;
                    has_longitude = true;
                }
                if (has_latitude && has_longitude) {
                    preferred_site = site;
                }
            }

            if (preferred_site.has_value()) {
                try {
                    protocol.set_site_info(preferred_site.value());
                    std::lock_guard<std::mutex> lock(mutex_);
                    site_latitude_deg_ = preferred_site->latitude_degrees;
                    site_longitude_deg_ = preferred_site->longitude_degrees;
                    site_coords_valid_ = true;
                    pending_site_latitude_ = preferred_site->latitude_degrees;
                    pending_site_longitude_ = preferred_site->longitude_degrees;
                    return;
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "Unable to apply configured site info on connect: " + std::string(e.what()));
                }
            }

            try {
                const SiteInfo site = protocol.get_site_info();
                std::lock_guard<std::mutex> lock(mutex_);
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
            } catch (const std::exception&) {
            }
        };
        auto apply_pending_elevation = [&]() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
        };
        auto sync_mount_time_if_configured = [&]() {
            if (!sync_time_on_connect_) {
                return;
            }

            const auto now = std::chrono::system_clock::now();
            {
                // Snapshot and send under the site write lock, so a concurrent setter's value is not re-sent stale.
                std::lock_guard<std::mutex> site_write(site_write_mutex_);
                std::optional<SiteInfo> site;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (site_coords_valid_) {
                        site = SiteInfo{};
                        site->latitude_degrees = site_latitude_deg_;
                        site->longitude_degrees = site_longitude_deg_;
                    }
                }
                if (site.has_value()) {
                    protocol.set_site_info(site.value());
                }
            }

            // Use UTC timezone on mount time sync to avoid timezone-sign ambiguities.
            const int offset_minutes = 0;
            TimeInfo info = from_utc_time_point(now, offset_minutes);
            protocol.set_time_info(info);
            std::lock_guard<std::mutex> lock(mutex_);
            last_utc_set_ = now;
            last_utc_set_monotonic_ = clock_.now();
            last_utc_valid_ = true;
            timezone_offset_minutes_ = offset_minutes;
            timezone_valid_ = true;
        };

        if (connected == connected_.load()) {
            if (!connected) {
                return;
            }

            // No-op reconnect (client sends Connected=true while already
            // connected, e.g. the Platform 7 async /connect handshake). The
            // mount never went away, so keep the telemetry caches warm —
            // clearing them forces every first property read after connect to
            // pay live serial round-trips (~95 ms each on the AM5N's USB-ACM
            // link), which blows the ASCOM FAST 0.1 s target during ConformU.
            stop_poll_thread();
            poll_stop_.store(false);
            reset_session_state_for_connect(/*keep_telemetry_caches=*/true);
            apply_or_load_site_info();
            apply_pending_elevation();
            try {
                sync_mount_time_if_configured();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(
                    "ZWO", "Unable to synchronize mount time/site on Connected=true refresh: " + std::string(e.what()));
            }
            refresh_cached_values();
            start_poll_thread();
            start_pulse_thread();
            return;
        }

        if (connected) {
            stop_poll_thread();
            stop_pulse_thread();
            cancel_and_join_goto_thread();
            if (!protocol.connect(connection_info_)) {
                // The wrapper's reason (a port another device holds, #1202)
                // goes to the operator.
                const std::string reason = protocol.last_connect_error();
                throw AlpacaException(
                    reason.empty() ? "Failed to connect to ZWO mount" : "Failed to connect to ZWO mount: " + reason,
                    AlpacaError::NotConnected);
            }
            try {
                // Publish the open link internally; getters wait for the end of the initial telemetry attempt.
                caches_ready_.store(false);
                reset_session_state_for_connect();
                connected_.store(true);

                // Identity gate (decision 0017 rule 8): nothing is written to a device that has not answered :GVP
                // as a ZWO AM mount. One attempt plus one retry, each under the command timeout.
                std::string mount_info;
                std::string refusal;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    try {
                        mount_info = protocol.get_mount_info();
                        if (is_zwo_am_model_reply(mount_info)) {
                            refusal.clear();
                            break;
                        }
                        refusal = "the device answered :GVP with '" + sanitize_for_log(mount_info) +
                                  "', which is not a ZWO AM mount";
                    } catch (const std::exception& e) {
                        refusal = std::string("the device gave no usable reply to :GVP (") +
                                  sanitize_for_log(e.what()) + "), so it is not confirmed as a ZWO AM mount";
                    }
                }
                if (!refusal.empty()) {
                    throw AlpacaException("Refusing to connect: " + refusal, AlpacaError::NotConnected);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    mount_info_ = mount_info;
                }

                apply_or_load_site_info();
                apply_pending_elevation();

                // A client write that lands while the read below is on the wire is newer than the reading, so the
                // publish skips when set_guide_rate() bumped the counter in between.
                std::uint64_t guide_rate_writes_seen = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    guide_rate_writes_seen = guide_rate_writes_;
                }
                bool guide_rate_set = false;
                GuideRate guide_rate{};
                for (int attempt = 0; attempt < 3 && !guide_rate_set; ++attempt) {
                    try {
                        const double guide_rate_multiple = protocol.get_guide_rate();
                        const double guide_rate_deg_per_sec = guide_rate_multiple * kSiderealRateDegPerSec;
                        guide_rate = {guide_rate_deg_per_sec, guide_rate_deg_per_sec};
                        guide_rate_set = true;
                    } catch (const std::exception&) {
                        if (attempt == 0) {
                            clock_.sleep_for(std::chrono::milliseconds(150));
                        }
                    }
                }
                if (!guide_rate_set) {
                    const double fallback_multiple = 1.00;
                    const double fallback_deg_per_sec = fallback_multiple * kSiderealRateDegPerSec;
                    ALPACA_LOG_WARN("ZWO", "Unable to read guide rate; assuming 1.00x sidereal until updated");
                    guide_rate = {fallback_deg_per_sec, fallback_deg_per_sec};
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (guide_rate_writes_ == guide_rate_writes_seen) {
                        guide_rate_ = guide_rate;
                    }
                }

                try {
                    const int tracking_rate = protocol.get_tracking_rate();
                    std::lock_guard<std::mutex> lock(mutex_);
                    tracking_rate_cached_ = tracking_rate;
                    tracking_rate_valid_ = true;
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                    // TrackingRate stays unavailable until a successful client write.
                }

                try {
                    sync_mount_time_if_configured();
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO",
                                    "Unable to synchronize mount time/site on connect: " + std::string(e.what()));
                }
                // Try the first measurement before publishing Connected; failure is unavailable telemetry, not a guess.
                refresh_cached_values();
                if (!connected_.load() || protocol.link_lost())
                    throw AlpacaException("ZWO mount transport lost during connect", AlpacaError::DriverException);
                caches_ready_.store(true);
                start_poll_thread();
                start_pulse_thread();
                return;
            } catch (...) {
                connected_.store(false);
                caches_ready_.store(false);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    snapshot_.reset();
                    health_.reset();
                    loss_reason_.clear();
                }
                protocol.cancel_pending_io();
                stop_poll_thread();
                stop_pulse_thread();
                protocol.disconnect();
                throw;
            }
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

    std::string command_blind(std::string_view command, bool) override {
        check_connected();
        protocol_.send_command_blind(std::string(command));
        return "";
    }

    bool command_bool(std::string_view command, bool) override {
        check_connected();
        const std::string response = protocol_.send_command(std::string(command), false);
        if (response.empty()) {
            return false;
        }
        const char ch = response[0];
        return ch == '1' || ch == 'T' || ch == 't' || ch == 'Y' || ch == 'y';
    }

    std::string command_string(std::string_view command, bool) override {
        check_connected();
        return protocol_.send_command(std::string(command), false);
    }

    AlignmentMode get_alignment_mode() const override {
        const auto status = measured(read_frame().status, "AlignmentMode");
        if (status.mode == MountMode::AltAzimuth) {
            return AlignmentMode::AltAz;
        }
        if (status.mode == MountMode::Unknown) {
            throw AlpacaException("AlignmentMode telemetry is not available yet", AlpacaError::DriverException);
        }
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override { return measured(read_frame().horizontal, "Altitude").altitude_degrees; }

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

    bool get_at_home() const override { return measured(read_frame().status, "AtHome").at_home; }

    bool get_at_park() const override {
        check_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (park_failed_) throw AlpacaException(last_slew_error_, AlpacaError::DriverException);
        return measured(frame_locked().parked, "AtPark");
    }

    double get_azimuth() const override { return measured(read_frame().horizontal, "Azimuth").azimuth_degrees; }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        check_connected();
        const auto now = clock_.now();
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
        std::lock_guard<std::mutex> lock(mutex_);
        const auto eq = measured(frame_locked().equatorial, "Declination");
        return std::clamp(eq.dec_degrees + dec_offset_deg_, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        check_connected();
        return 0.0;
    }

    void set_declination_rate(double) override {
        throw AlpacaException("DeclinationRate is not supported", AlpacaError::NotImplemented);
    }

    bool get_tracking() const override { return measured(read_frame().tracking, "Tracking"); }

    void set_tracking(bool tracking) override {
        check_connected();
        if (tracking) {
            // Per ITelescopeV4 / ConformU: enabling tracking while parked must
            // throw InvalidWhileParked; disabling is allowed (harmless). Check
            // BEFORE the tracking-state fast path below, which would otherwise
            // return early without the parked validation.
            ensure_not_parked("Tracking");
        }
        auto& protocol = protocol_;
        const auto now = clock_.now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!tracking) {
                goto_active_ = false;  // a commanded Tracking=false is off, GOTO or not
            }
            if (tracking_state_valid_ && (now - tracking_state_at_) <= kFastTrackingTtl &&
                tracking_state_cached_ == tracking) {
                return;  // a no-op write changes nothing, so it must not drop the poll in flight
            }
            snapshot_.write([](PolledFrame&) {});
        }
        auto wait_for_pulse_completion = [this](std::chrono::milliseconds max_wait) {
            const auto deadline = clock_.now() + max_wait;
            while (clock_.now() < deadline) {
                std::chrono::steady_clock::time_point pulse_end;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pulse_end = pulse_guiding_end_;
                }
                const auto now = clock_.now();
                if (pulse_end <= now) {
                    return;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(pulse_end - now);
                if (remaining > std::chrono::milliseconds(200)) {
                    remaining = std::chrono::milliseconds(200);
                }
                clock_.sleep_for(remaining);
            }
        };

        auto infer_tracking_from_status = [&protocol]() -> std::optional<bool> {
            try {
                const StatusInfo status = protocol.get_status();
                return !status.no_tracking;
            } catch (const std::exception&) {
                return std::nullopt;
            }
        };

        if (tracking) {
            wait_for_pulse_completion(std::chrono::milliseconds(1500));
        }

        std::optional<AlpacaException> last_error;
        bool command_ok = false;
        try {
            protocol.set_tracking_enabled(tracking);
            command_ok = true;
        } catch (const AlpacaException& e) {
            last_error = e;
        }

        // Verification is control-path I/O, never a public getter's fallback.
        const std::optional<bool> current_state = infer_tracking_from_status();

        if (current_state.has_value() && current_state.value() == tracking) {
            std::lock_guard<std::mutex> lock(mutex_);
            tracking_state_cached_ = tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = clock_.now();
            write_frame_locked();
            return;
        }

        if (command_ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            tracking_state_cached_ = tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = clock_.now();
            write_frame_locked();
            return;
        }

        if (last_error.has_value()) {
            throw last_error.value();
        }

        throw AlpacaException(
            std::string("Failed to ") + (tracking ? "enable" : "disable") + " tracking",
            AlpacaError::InvalidOperation);
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
        check_writable_state();
        std::lock_guard<std::mutex> lock(mutex_);
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate values must be finite", AlpacaError::InvalidValue);
        }

        const double effective = std::max(std::abs(rate.ra), std::abs(rate.dec));
        if (effective < 0.0 || effective > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("GuideRate must be in [0, max axis rate] deg/sec", AlpacaError::InvalidValue);
        }
        check_writable_state();

        if (connected_.load() && effective > 0.0) {
            double guide_rate_multiple = effective / kSiderealRateDegPerSec;
            guide_rate_multiple = std::clamp(guide_rate_multiple, 0.10, 0.90);
            // set_guide_rate() returns the readback value from its internal
            // verification — no extra round-trip needed.
            const double actual_multiple = protocol_.set_guide_rate(guide_rate_multiple);
            const double actual_deg_per_sec = actual_multiple * kSiderealRateDegPerSec;
            std::lock_guard<std::mutex> lock(mutex_);
            guide_rate_ = {actual_deg_per_sec, actual_deg_per_sec};
            ++guide_rate_writes_;
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        guide_rate_ = {effective, effective};
        ++guide_rate_writes_;
    }

    double get_right_ascension() const override {
        check_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto eq = measured(frame_locked().equatorial, "RightAscension");
        double wrapped = std::fmod(eq.ra_hours + ra_offset_hours_, 24.0);
        if (wrapped < 0.0) {
            wrapped += 24.0;
        }
        return wrapped;
    }

    double get_right_ascension_rate() const override {
        check_connected();
        return 0.0;
    }

    void set_right_ascension_rate(double) override {
        throw AlpacaException("RightAscensionRate is not supported", AlpacaError::NotImplemented);
    }

    int get_side_of_pier() const override {
        check_connected();

        // Compute SideOfPier from the current hour angle rather than querying :Gm#.
        // The :Gm# response from ZWO firmware does not reliably map to the ASCOM
        // pierEast/pierWest convention, and get_destination_side_of_pier already uses
        // this HA-based approach correctly.
        double longitude = 0.0;
        bool has_longitude = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (site_coords_valid_) {
                longitude = site_longitude_deg_;
                has_longitude = true;
            }
        }

        if (has_longitude) {
            const double current_ra = get_right_ascension();
            const double lst_hours = compute_local_sidereal_time_hours(
                std::chrono::system_clock::now(), longitude);
            const double ha = normalize_hour_angle_hours(lst_hours - current_ra);
            // HA >= 0 → target west of meridian → pierEast (0)
            // HA <  0 → target east of meridian → pierWest (1)
            const int side = ha >= 0.0 ? 0 : 1;
            return side;
        }

        return measured(read_frame().pier_side, "SideOfPier");
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
        double longitude = 0.0;
        bool has_longitude = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (site_coords_valid_) {
                longitude = site_longitude_deg_;
                has_longitude = true;
            }
        }
        check_connected();
        if (!has_longitude) {
            return get_side_of_pier();
        }

        const double lst_hours = compute_local_sidereal_time_hours(std::chrono::system_clock::now(), longitude);
        const double hour_angle = normalize_hour_angle_hours(lst_hours - ra);

        // German equatorial: target west of meridian (hour angle >= 0) -> pier side East.
        return hour_angle >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::Topocentric;
    }

    // The driver applies no refraction correction, so it neither knows nor computes the flag (ITelescopeV4).
    bool get_does_refraction() const override {
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    void set_does_refraction(bool) override {
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
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
            throw AlpacaException("SiteLongitude is not available for SiderealTime", AlpacaError::DriverException);
        }
        return compute_local_sidereal_time_hours(std::chrono::system_clock::now(), longitude);
    }

    double get_site_elevation() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_elevation_m_) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return *site_elevation_m_;
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_coords_valid_) {
            throw AlpacaException("Site coordinates not available", AlpacaError::ValueNotSet);
        }
        return site_latitude_deg_;
    }

    void set_site_latitude(double latitude) override {
        validate_latitude(latitude);
        check_writable_state();

        // One site write at a time, so two concurrent setters, or a setter and the connect's site apply/re-send, reach
        // the mount in the order they published. No getter takes it.
        std::lock_guard<std::mutex> site_write(site_write_mutex_);
        std::optional<SiteInfo> to_send;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Capture whether a longitude is already known BEFORE publishing this
            // latitude: setting site_coords_valid_ first made has_longitude
            // unconditionally true, so a latitude-only set wrote longitude 0.0 to
            // the mount (M10). Partial site data must never be sent.
            const bool has_longitude = pending_site_longitude_.has_value() || site_coords_valid_;
            const double longitude =
                pending_site_longitude_.has_value() ? pending_site_longitude_.value() : site_longitude_deg_;
            site_latitude_deg_ = latitude;
            pending_site_latitude_ = latitude;
            if (has_longitude) {
                site_coords_valid_ = true;
            }
            if (connected_.load() && has_longitude) {
                to_send = SiteInfo{};
                to_send->latitude_degrees = site_latitude_deg_;
                to_send->longitude_degrees = longitude;
            }
        }
        // The serial write runs outside mutex_ so no getter waits on it (decision 0016).
        if (to_send.has_value()) {
            protocol_.set_site_info(to_send.value());
            std::lock_guard<std::mutex> lock(mutex_);
            pending_site_longitude_ = to_send->longitude_degrees;
            site_longitude_deg_ = to_send->longitude_degrees;
        }
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_coords_valid_) {
            throw AlpacaException("Site coordinates not available", AlpacaError::ValueNotSet);
        }
        return site_longitude_deg_;
    }

    void set_site_longitude(double longitude) override {
        validate_longitude(longitude);
        check_writable_state();

        std::lock_guard<std::mutex> site_write(site_write_mutex_);  // see set_site_latitude
        std::optional<SiteInfo> to_send;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Mirror of set_site_latitude (M10): capture whether a latitude is
            // already known BEFORE publishing this longitude, so a longitude-only
            // set never writes latitude 0.0 to the mount.
            const bool has_latitude = pending_site_latitude_.has_value() || site_coords_valid_;
            const double latitude =
                pending_site_latitude_.has_value() ? pending_site_latitude_.value() : site_latitude_deg_;
            site_longitude_deg_ = longitude;
            pending_site_longitude_ = longitude;
            if (has_latitude) {
                site_coords_valid_ = true;
            }
            if (connected_.load() && has_latitude) {
                to_send = SiteInfo{};
                to_send->latitude_degrees = latitude;
                to_send->longitude_degrees = site_longitude_deg_;
            }
        }
        // The serial write runs outside mutex_ so no getter waits on it (decision 0016).
        if (to_send.has_value()) {
            protocol_.set_site_info(to_send.value());
            std::lock_guard<std::mutex> lock(mutex_);
            pending_site_latitude_ = to_send->latitude_degrees;
            site_latitude_deg_ = to_send->latitude_degrees;
        }
    }

    bool get_can_move_axis(int axis) const override {
        if (axis == 0 || axis == 1) {
            return true;
        }
        if (axis == 2) {
            return false;
        }
        throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
    }

    void move_axis(int axis, double rate) override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(rate) || std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("Axis rate out of range", AlpacaError::InvalidValue);
        }
        check_connected(std::abs(rate) < 1e-9);
        ensure_not_parked("MoveAxis");

        // open-astro#720: a pending GOTO setup must not send ":MS" after the
        // stop or the jog below. One fence covers both branches.
        cancel_and_join_goto_thread();

        auto& protocol = protocol_;

        if (std::abs(rate) < 1e-9) {
            // open-astro#775: ":Q" stops every axis, but MoveAxis(axis, 0) must not
            // affect the other axis. Send only this axis's directional stops (the
            // active direction is not tracked, so both are sent; a stop for a
            // direction that is not moving is a no-op on the mount).
            bool other_axis_jogging = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                other_axis_jogging = manual_axis_slewing_[1 - axis];
            }
            if (axis == 0) {
                protocol.stop_move_east();
                protocol.stop_move_west();
            } else {
                protocol.stop_move_north();
                protocol.stop_move_south();
            }
            if (!other_axis_jogging) {
                // No rig evidence that the directional stops alone halt a jog on
                // this firmware (the original driver also sent ":Q"). With the
                // other axis idle, the generic stop affects nothing else.
                protocol.abort_motion();
            }

            std::optional<bool> restore_tracking;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                manual_axis_slewing_[axis] = false;
                restore_tracking = manual_axis_tracking_restore_[axis];
                manual_axis_tracking_restore_[axis] = std::nullopt;
                const int other = 1 - axis;
                if (manual_axis_slewing_[other]) {
                    // The other axis is still jogging: hand the saved tracking
                    // state to it so it is restored when that axis stops.
                    if (restore_tracking.has_value() && !manual_axis_tracking_restore_[other].has_value()) {
                        manual_axis_tracking_restore_[other] = restore_tracking;
                    }
                    restore_tracking = std::nullopt;
                } else {
                    park_command_active_ = false;
                    park_motion_seen_ = false;
                }
                write_frame_locked();
            }

            if (restore_tracking.has_value()) {
                // Tracking can be dropped by stop/abort commands after manual motion; restore prior state.
                const bool desired_tracking = restore_tracking.value();
                try {
                    set_tracking(desired_tracking);
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN(
                        "ZWO",
                        "Unable to restore tracking state after MoveAxis stop: " +
                            std::string(ex.what()));
                }
            }
            return;
        }

        // Use cached tracking state to avoid a mount round-trip that adds latency
        // on Wi-Fi links and can push response time past the STANDARD 1.0s target.
        std::optional<bool> tracking_before_move;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!manual_axis_slewing_[axis]) {
                if (tracking_state_valid_) {
                    tracking_before_move = tracking_state_cached_;
                }
            }
        }

        // Pause the poll thread so its protocol reads don't hold the protocol
        // mutex while we send the rate + move commands. On Wi-Fi the poll
        // thread's reads can add 200-400ms of contention per blocked command.
        poll_pause_.store(true);

        // Always un-pause, even when a protocol call throws — a throw here
        // previously left poll_pause_ stuck true and the poll thread parked
        // forever (M9).
        try {
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
        } catch (...) {
            poll_pause_.store(false);
            throw;
        }

        poll_pause_.store(false);

        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[axis] = true;
        if (tracking_before_move.has_value()) {
            manual_axis_tracking_restore_[axis] = tracking_before_move;
        }
        park_command_active_ = false;
        park_motion_seen_ = false;
        parked_cached_ = false;
        slew_force_until_ = clock_.now() + std::chrono::seconds(2);
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that jogs an axis after a failed GOTO must not be told the OLD
        // goto failed.
        clear_last_slew_error_locked();
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 0 || axis == 1) {
            return {0.0, kMaxMoveAxisRateDegPerSec};
        }
        return {0.0, 0.0};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 0 || axis == 1) {
            return {{0.0, kMaxMoveAxisRateDegPerSec}};
        }
        if (axis == 2) {
            return {};
        }
        throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
    }

    bool get_slewing() const override {
        check_connected();

        const auto now = clock_.now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // open-astro#575: surface a stored async-slew failure as an error
            // instead of a silent false, until AbortSlew or a new slew
            // initiator clears it (see last_slew_error_). Checked before the
            // force window below so a rejected GOTO reports the error right
            // away instead of waiting it out.
            if (!last_slew_error_.empty()) {
                throw AlpacaException(last_slew_error_);
            }
            if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
                return true;
            }
            if (slew_force_until_ > now) {
                return true;
            }
            if (goto_active_) return true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (park_command_active_) return true;
        return !measured(frame_locked().status, "Slewing").stop_or_tracking;
    }

    double get_target_declination() const override {
        check_writable_state();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_dec_set_) {
            throw AlpacaException("TargetDeclination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        validate_dec(dec, "TargetDeclination");
        check_writable_state();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_dec_degrees_ = dec;
            target_dec_set_ = true;
        }

        if (connected_.load()) {
            protocol_.set_target_dec(dec);
        }
    }

    double get_target_right_ascension() const override {
        check_writable_state();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_) {
            throw AlpacaException("TargetRightAscension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        validate_ra(ra, "TargetRightAscension");
        check_writable_state();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_ra_set_ = true;
        }

        if (connected_.load()) {
            protocol_.set_target_ra(ra);
        }
    }

    int get_tracking_rate() const override {
        check_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!tracking_rate_valid_)
            throw AlpacaException("TrackingRate telemetry is not available yet", AlpacaError::DriverException);
        return tracking_rate_cached_;
    }

    void set_tracking_rate(int rate) override {
        if (rate < 0 || rate > 2) {
            throw AlpacaException("TrackingRate must be 0 (sidereal), 1 (lunar), or 2 (solar)",
                                  AlpacaError::InvalidValue);
        }
        check_connected();

        protocol_.set_tracking_rate(rate);

        std::lock_guard<std::mutex> lock(mutex_);
        tracking_rate_cached_ = rate;
        tracking_rate_valid_ = true;
    }

    std::vector<int> get_tracking_rates() const override {
        return {0, 1, 2};
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        check_writable_state();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (last_utc_valid_) {
                const auto elapsed = clock_.now() - last_utc_set_monotonic_;
                return last_utc_set_ +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
            }
        }

        return std::chrono::system_clock::now();
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        check_writable_state();
        // Use UTC timezone on mount time sync to avoid timezone-sign ambiguities.
        const int offset_minutes = 0;
        TimeInfo info = from_utc_time_point(utc, offset_minutes);
        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)

        if (connected_.load()) {
            protocol_.set_time_info(info);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = clock_.now();
        last_utc_valid_ = true;
        timezone_offset_minutes_ = offset_minutes;
        timezone_valid_ = true;
        if (connected_.load()) {
            // The mount now runs on the client's clock and so does the cached
            // pointing time; on a disciplined host say so once (open-astro#409).
            alpacacore::util::ClientUtcWarning::warn_once("ZWO", client_minus_host, client_disagreement_warned_);
        }
    }

    void find_home() override {
        check_connected();
        ensure_not_parked("FindHome");
        // open-astro#720: a pending GOTO setup must not send ":MS" after ":hC".
        cancel_and_join_goto_thread();
        reset_position_offsets();
        protocol_.go_home();
        std::lock_guard<std::mutex> lock(mutex_);
        parked_cached_ = false;
        park_command_active_ = false;
        park_motion_seen_ = false;
        park_state_cached_ = false;
        park_state_at_ = std::chrono::steady_clock::time_point{};
        slew_force_until_ = clock_.now() + std::chrono::seconds(5);
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that calls FindHome after a failed GOTO must not be told the OLD
        // goto failed while it's homing.
        clear_last_slew_error_locked();
    }

    void park() override {
        check_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (park_failed_) clear_last_slew_error_locked();
        }
        // open-astro#720: a pending GOTO setup must not send ":MS" after ":hP".
        cancel_and_join_goto_thread();
        reset_position_offsets();
        if (get_at_park()) {
            return;
        }
        auto& protocol = protocol_;
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

        std::lock_guard<std::mutex> lock(mutex_);
        parked_cached_ = false;
        park_command_active_ = true;
        park_command_started_ = clock_.now();
        park_motion_seen_ = false;
        slew_force_until_ = clock_.now() + std::chrono::seconds(5);
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that calls Park after a failed GOTO must not be told the OLD goto
        // failed while it's parking.
        clear_last_slew_error_locked();
    }

    void abort_slew() override {
        check_connected(true);
        ensure_not_parked("AbortSlew");
        reset_position_offsets();
        // open-astro#575 / #720: fence a GOTO setup thread still writing the
        // target (cancel AND join), so it neither sends ":MS" after this abort
        // nor records its rejection as a slew failure. A ":MS" already on the
        // wire when the cancel lands goes out before the ":Q" below, which
        // stops it. The join waits for at most one mount round trip.
        cancel_and_join_goto_thread();
        protocol_.abort_motion();
        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        manual_axis_tracking_restore_[0] = std::nullopt;
        manual_axis_tracking_restore_[1] = std::nullopt;
        park_command_active_ = false;
        park_motion_seen_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point{};
        // open-astro#575: AbortSlew is a valid clearing command for a stored
        // slew failure -- the client acted on the error, so the next
        // Slewing read must answer normally again.
        clear_last_slew_error_locked();
    }

    void pulse_guide(int direction, int duration) override {
        if (duration < 0) {
            throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
        }
        if (direction < 0 || direction > 3) {
            throw AlpacaException("PulseGuide direction must be 0..3", AlpacaError::InvalidValue);
        }
        check_connected();
        ensure_not_parked("PulseGuide");
        if (!get_tracking()) {
            throw AlpacaException(
                "PulseGuide requires Tracking to be enabled",
                AlpacaError::InvalidOperation);
        }
        const bool ra_axis = (direction == 2 || direction == 3);
        const bool dec_axis = (direction == 0 || direction == 1);

        const int effective_duration = std::clamp(duration, 0, 60000);
        double guide_rate_deg_per_sec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            guide_rate_deg_per_sec = ra_axis ? guide_rate_.ra : guide_rate_.dec;
        }
        guide_rate_deg_per_sec = std::abs(guide_rate_deg_per_sec);
        if (guide_rate_deg_per_sec <= 0.0) {
            ALPACA_LOG_WARN("ZWO", "PulseGuide requested with zero guide rate; no motion will be applied");
            return;
        }

        if (effective_duration <= 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_equatorial_.has_value() && (clock_.now() - cached_equatorial_at_) <= kFastEquatorialTtlForRead) {
                const auto& eq = cached_equatorial_.value();
                ALPACA_LOG_DEBUG(
                    "ZWO",
                    "PulseGuide pre dir=" + std::to_string(direction) +
                        " ra_hours=" + std::to_string(eq.ra_hours) +
                        " dec_deg=" + std::to_string(eq.dec_degrees));
            }
        }

        ALPACA_LOG_DEBUG(
            "ZWO",
            "PulseGuide start dir=" + std::to_string(direction) +
                " duration_ms=" + std::to_string(effective_duration) +
                " ra_axis=" + std::string(ra_axis ? "true" : "false") +
                " dec_axis=" + std::string(dec_axis ? "true" : "false") +
                " guide_rate_deg_per_sec=" + std::to_string(guide_rate_deg_per_sec) +
                " tracking=" + std::string(get_tracking() ? "true" : "false"));

        const double duration_sec = static_cast<double>(effective_duration) / 1000.0;
        const double expected_deg = guide_rate_deg_per_sec * duration_sec;
        const double expected_hours = ra_axis
            ? ((direction == 3 ? -expected_deg : expected_deg) / 15.0)
            : 0.0;
        const double expected_dec = dec_axis
            ? (direction == 1 ? -expected_deg : expected_deg)
            : 0.0;

        pulse_cancel_.store(false);
        PulseTask task;
        task.direction = direction;
        task.duration_ms = effective_duration;
        task.ra_axis = ra_axis;
        task.guide_rate_deg_per_sec = 0.0;
        task.expected_ra_hours = expected_hours;
        task.expected_dec_degrees = expected_dec;
        task.generation = pulse_generation_.load();

        {
            // Match the connect reset's pulse -> state order (#714); state remains the leaf.
            std::lock_guard<std::mutex> lock(pulse_mutex_);
            std::lock_guard<std::mutex> state_lock(mutex_);
            const auto now = clock_.now();
            const auto start_time = std::max(pulse_queue_end_, now);
            pulse_queue_end_ = start_time + std::chrono::milliseconds(effective_duration);
            ra_offset_hours_ += expected_hours;
            dec_offset_deg_ += expected_dec;
            dec_offset_deg_ = std::clamp(dec_offset_deg_, -180.0, 180.0);
            pulse_guiding_end_ = pulse_queue_end_ + kPulseGuideHold;
            pulse_queue_.push_back(task);
            snapshot_.write([](PolledFrame&) {});
        }
        pulse_cv_.notify_one();
    }

    void set_park() override {
        check_connected();
        reset_position_offsets();
        if (!protocol_.set_custom_park_here()) {
            throw AlpacaException("Mount rejected set park request", AlpacaError::InvalidOperation);
        }
    }

    void slew_to_coordinates(double ra, double dec) override {
        // slew_to_target() -> slew_to_target_async() handles ensure_not_parked and
        // reset_position_offsets; avoid redundant mount queries here. The Tracking
        // gate runs first, though: the target setters below write to the mount.
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
        check_equatorial_slew_allowed("SlewToCoordinates");
        set_target_right_ascension(ra);
        set_target_declination(dec);
        slew_to_target();
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        // slew_to_target_async() handles reset_position_offsets and the target
        // checks. The parked and Tracking gate runs here too, before the target
        // is replaced: both read cached state, and Tracking reads the mount (":GAT")
        // only when its 2 s cache has aged out.
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
        check_equatorial_slew_allowed("SlewToCoordinatesAsync");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
        }
        slew_to_target_async();
    }

    void slew_to_target() override {
        // slew_to_target_async() handles ensure_not_parked and reset_position_offsets.
        slew_to_target_async();

        const auto deadline = clock_.now() + kDefaultSlewTimeout;
        while (clock_.now() < deadline) {
            if (!get_slewing()) {
                const int settle = get_slew_settle_time();
                if (settle > 0) {
                    clock_.sleep_for(std::chrono::seconds(settle));
                }
                return;
            }
            clock_.sleep_for(std::chrono::milliseconds(200));
        }

        throw AlpacaException("Slew operation timed out", AlpacaError::DriverException);
    }

    void slew_to_target_async() override {
        check_connected();
        // Pause the poll thread early so its protocol reads don't hold the
        // protocol mutex while we set up the GOTO.  ensure_not_parked and
        // reset_position_offsets use only cached state / atomics, so pausing
        // the poll thread first is safe and avoids contention on Wi-Fi links.
        poll_pause_.store(true);
        // Sibling of the move_axis M9 fix: every gate below can throw -- never
        // leave poll_pause_ stuck true.
        double target_ra = 0.0;
        double target_dec = 0.0;
        try {
            ensure_not_parked("SlewToTargetAsync");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!target_ra_set_ || !target_dec_set_) {
                    throw AlpacaException("Target coordinates not set", AlpacaError::ValueNotSet);
                }
                target_ra = target_ra_hours_;
                target_dec = target_dec_degrees_;
            }
            validate_ra(target_ra, "RightAscension");
            validate_dec(target_dec, "Declination");
            // ITelescopeV4: an equatorial slew with Tracking false is
            // InvalidOperation (open-astro#1274; see require_tracking_for_slew),
            // checked before reset_position_offsets() cancels a pulse in
            // flight, so a refused slew leaves that motion alone.
            require_tracking_for_slew("SlewToTargetAsync");
            reset_position_offsets();
        } catch (...) {
            poll_pause_.store(false);
            throw;
        }

        uint64_t slew_epoch = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            manual_axis_tracking_restore_[0] = std::nullopt;
            manual_axis_tracking_restore_[1] = std::nullopt;
            slew_force_until_ = clock_.now() + std::chrono::seconds(5);
            goto_active_ = true;
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that retries a rejected goto must not be told the OLD goto
            // failed.
            clear_last_slew_error_locked();
            slew_epoch = slew_error_epoch_;
            parked_cached_ = false;
            park_command_active_ = false;
            park_motion_seen_ = false;
            cached_equatorial_.reset();
            cached_equatorial_at_ = std::chrono::steady_clock::time_point{};
            write_frame_locked();
        }

        // Joinable member thread, NOT detached (H1): cancel + join any
        // previous GOTO setup first, then reset the cancel flag for this one.
        // Park, FindHome, MoveAxis, AbortSlew and set_connected(false) cancel
        // and join via cancel_and_join_goto_thread(); the destructor cancels
        // and joins too. The reap stays inline here because goto_reap_mutex_
        // must be held across the respawn and is not recursive.
        // goto_reap_mutex_ serializes concurrent SlewAsync callers — without
        // it two callers join and assign goto_thread_ simultaneously (UB,
        // found by the [stress] telescope suite). The GOTO body never takes
        // this mutex, so the join cannot deadlock.
        std::lock_guard<std::mutex> reap_lock(goto_reap_mutex_);
        cancel_goto_thread_request();
        if (goto_thread_.joinable()) {
            goto_thread_.join();
        }
        {
            std::lock_guard<std::mutex> cancel_lock(goto_mutex_);
            goto_cancel_.store(false);
        }
        goto_thread_ = std::thread([this, target_ra, target_dec, slew_epoch]() {
            auto resume_poll = [this]() { poll_pause_.store(false); };
            // open-astro#720: a cancelled GOTO started no slew (it exits before
            // ":MS", or after the mount rejected it), so it must not leave its
            // 5 s Slewing force window behind. A newer SlewToTargetAsync has
            // already moved the epoch on and owns the state.
            auto abandon = [this, slew_epoch, &resume_poll]() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (slew_error_epoch_ == slew_epoch) {
                        slew_force_until_ = std::chrono::steady_clock::time_point{};
                        goto_active_ = false;
                    }
                }
                resume_poll();
            };
            auto& protocol = protocol_;
            try {
                if (goto_cancel_.load()) {
                    abandon();
                    return;
                }
                // open-astro#720: every checkpoint below lets a cancel-and-join
                // wait for at most the one round trip in flight.
                if (!synchronize_mount_time_and_site_for_goto()) {
                    abandon();
                    return;
                }
                // The sync's last round trip (":SMTI") may be the one the
                // cancel landed in.
                if (goto_cancel_.load()) {
                    abandon();
                    return;
                }
                protocol.set_target_ra(target_ra);
                if (goto_cancel_.load()) {
                    abandon();
                    return;
                }
                protocol.set_target_dec(target_dec);
                if (goto_cancel_.load()) {
                    abandon();
                    return;
                }
                for (int attempt = 0; attempt < 3 && !goto_cancel_.load(); ++attempt) {
                    try {
                        if (!protocol.goto_target()) {
                            resume_poll();
                            throw AlpacaException("Mount rejected GOTO request", AlpacaError::InvalidOperation);
                        }
                        resume_poll();
                        return;
                    } catch (const AlpacaException& ex) {
                        if (attempt < 2 && is_ms_mount_busy_error(ex)) {
                            ALPACA_LOG_WARN("ZWO", "GOTO rejected with e3 (mount busy); aborting motion and retrying");
                            // A cancelled thread must not send ":Q" on top of
                            // the newer command.
                            if (goto_cancel_.load()) {
                                abandon();
                                return;
                            }
                            protocol.abort_motion();
                            // Cancellable backoff: a disconnect/destruction
                            // must not wait out the retry sleep.
                            std::unique_lock<std::mutex> cancel_lock(goto_mutex_);
                            clock_.wait_for(cancel_lock, goto_cv_, std::chrono::milliseconds(300),
                                            [this]() { return goto_cancel_.load(); });
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
                            if (!synchronize_mount_time_and_site_for_goto()) {
                                abandon();
                                return;
                            }
                            continue;
                        }
                        if (is_ms_time_site_not_synchronized_error(ex)) {
                            ALPACA_LOG_WARN(
                                "ZWO",
                                "GOTO rejected by mount (e7: time and position not synchronized); "
                                "verify SiteLatitude/SiteLongitude and UTCDate.");
                        } else if (is_ms_target_under_horizon_error(ex)) {
                            ALPACA_LOG_WARN(
                                "ZWO",
                                "GOTO rejected by mount (e5: target under horizon); "
                                "verify SiteLatitude/SiteLongitude and UTCDate.");
                        } else {
                            ALPACA_LOG_WARN("ZWO", "GOTO failed: " + std::string(ex.what()));
                        }
                        // open-astro#575: a cancellation (AbortSlew, or a
                        // reap by a newer initiator) is not a failure -- the
                        // canceller already owns clearing/replacing
                        // last_slew_error_. Nor is a rejection after a newer
                        // command (Park/FindHome/MoveAxis/Unpark) cleared the
                        // slot: the epoch says that command owns it now.
                        if (goto_cancel_.load()) {
                            abandon();
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (slew_error_epoch_ == slew_epoch) {
                                last_slew_error_ = "SlewToTargetAsync failed: " + std::string(ex.what());
                            }
                        }
                        resume_poll();
                        return;
                    }
                }
                // Only a cancel ends the loop; every other path returns above.
                abandon();
                return;
            } catch (const std::exception& ex) {
                ALPACA_LOG_WARN("ZWO", "GOTO failed: " + std::string(ex.what()));
                // A cancelled GOTO that then fails (a rejected ":Sr" the
                // canceller waited for) is abandoned like any other.
                if (goto_cancel_.load()) {
                    abandon();
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (slew_error_epoch_ == slew_epoch) {
                        last_slew_error_ = "SlewToTargetAsync failed: " + std::string(ex.what());
                    }
                }
            }
            resume_poll();
        });
    }

    void sync_to_coordinates(double ra, double dec) override {
        // sync_to_target() handles ensure_not_parked and reset_position_offsets.
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
        check_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
        }
        sync_to_target();
    }

    void sync_to_target() override {
        check_connected();
        ensure_not_parked("SyncToTarget");
        reset_position_offsets();

        double target_ra = 0.0;
        double target_dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates not set", AlpacaError::ValueNotSet);
            }
            target_ra = target_ra_hours_;
            target_dec = target_dec_degrees_;
        }

        validate_ra(target_ra, "RightAscension");
        validate_dec(target_dec, "Declination");

        auto& protocol = protocol_;

        auto attempt_sync = [&]() {
            try {
                protocol.sync_target_equatorial(target_ra, target_dec);
                return true;
            } catch (const AlpacaException& ex) {
                if (is_equipment_moving_error(ex)) {
                    return false;
                }
                if (ex.error_code() != AlpacaError::InvalidValue) {
                    throw;
                }
                // Fallback for firmware that does not accept :SMMC.
                protocol.set_target_ra(target_ra);
                protocol.set_target_dec(target_dec);
                protocol.sync_target();
                return true;
            }
        };

        // :SMMC can fail with e4 (equipment moving) if the mount is still settling
        // from a prior slew.  Wait for motion to stop and retry.
        if (!attempt_sync()) {
            protocol.abort_motion();
            for (int retry = 0; retry < 5; ++retry) {
                clock_.sleep_for(std::chrono::milliseconds(200));
                try {
                    if (!get_slewing()) {
                        break;
                    }
                } catch (const std::exception&) {
                }
            }
            if (!attempt_sync()) {
                // Last resort: abort again and try the set-target + :CM path.
                protocol.abort_motion();
                clock_.sleep_for(std::chrono::milliseconds(300));
                protocol.set_target_ra(target_ra);
                protocol.set_target_dec(target_dec);
                protocol.sync_target();
            }
        }
        EquatorialCoordinates raw;
        bool has_raw = false;
        const auto now = clock_.now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_equatorial_.has_value() && (now - cached_equatorial_at_) <= kFastEquatorialTtl) {
                raw = cached_equatorial_.value();
                has_raw = true;
            }
        }
        if (!has_raw) {
            try {
                raw = protocol.get_current_equatorial();
                has_raw = true;
            } catch (const std::exception&) {
            }
        }
        if (has_raw) {
            const double ra_delta = normalize_hour_angle_hours(target_ra - raw.ra_hours);
            const double dec_delta = target_dec - raw.dec_degrees;
            std::lock_guard<std::mutex> lock(mutex_);
            ra_offset_hours_ = std::fmod(ra_offset_hours_ + ra_delta, 24.0);
            if (ra_offset_hours_ < 0.0) {
                ra_offset_hours_ += 24.0;
            }
            dec_offset_deg_ = std::clamp(dec_offset_deg_ + dec_delta, -90.0, 90.0);
            cached_equatorial_ = raw;
            cached_equatorial_at_ = now;
            write_frame_locked();
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            cached_equatorial_.reset();
            cached_equatorial_at_ = std::chrono::steady_clock::time_point{};
            write_frame_locked();
        }
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
        reset_position_offsets();
        auto& protocol = protocol_;
        // Guard the poll thread's park-state warm against re-parking us: some
        // firmware keeps reporting :Gps=Completed briefly after :Spu, and the
        // warm logic's Completed branch would otherwise flip parked_cached_
        // back to true (which then sticks, since the !parked_cached_ guard
        // never self-corrects). Set the flag under the lock before the :Spu
        // round-trip and clear it after the caches are reset below.
        {
            std::lock_guard<std::mutex> guard(mutex_);
            unpark_in_progress_ = true;
        }
        try {
            if (!protocol.unpark()) {
                std::optional<ParkStatus> park_status;
                try {
                    park_status = protocol.get_park_status();
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                    // :Gps unavailable — the unpark is still treated as success below.
                }

                if (!(park_status == ParkStatus::Unknown || park_status == ParkStatus::NotParked ||
                      !park_status.has_value())) {
                    throw AlpacaException("Mount rejected unpark request", AlpacaError::InvalidOperation);
                }

                ALPACA_LOG_WARN("ZWO",
                                "Unpark returned 0 while :Gps is unavailable/not-parked; "
                                "treating unpark as successful");
            }

            std::lock_guard<std::mutex> lock(mutex_);
            parked_cached_ = false;
            park_command_active_ = false;
            park_motion_seen_ = false;
            unpark_in_progress_ = false;
            // Also drop the stale get_at_park() cache: ensure_not_parked() prefers
            // a fresh park_state_cached_ over parked_cached_, so a recent AtPark
            // read (true, within kFastParkTtl) would otherwise make operations like
            // set_tracking(true)/slew throw InvalidWhileParked right after a
            // successful unpark.
            park_state_cached_ = false;
            park_state_at_ = clock_.now();
            slew_force_until_ = clock_.now() + std::chrono::seconds(2);
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that unparks after a failed GOTO must not be told the OLD
            // goto failed.
            clear_last_slew_error_locked();
        } catch (...) {
            // protocol.unpark()/get_park_status() can throw (serial/mount error):
            // always clear the guard so the poll thread's park-state warm is not
            // disabled permanently by a failed unpark.
            std::lock_guard<std::mutex> guard(mutex_);
            unpark_in_progress_ = false;
            throw;
        }
    }

private:
    static constexpr auto kSnapshotMaxAge = std::chrono::seconds(5);
    // Longer TTL for equatorial cache so RA/Dec reads meet FAST target over high-latency (e.g. WiFi) links.
    static constexpr auto kFastEquatorialTtlForRead = std::chrono::seconds(5);
    static constexpr auto kFastTrackingTtl = std::chrono::seconds(2);
    static constexpr auto kFastEquatorialTtl = std::chrono::seconds(2);
    static constexpr auto kFastParkTtl = std::chrono::seconds(2);

    enum class ParkEvaluation {
        Unknown,
        NotParked,
        InProgress,
        Parked
    };

    ParkEvaluation evaluate_park_state_locked(const std::optional<ParkStatus>& park_status,
                                              const std::optional<StatusInfo>& mount_status,
                                              std::chrono::steady_clock::time_point now) const {
        if (park_command_active_ && mount_status && !mount_status->stop_or_tracking) {
            park_motion_seen_ = true;
            return ParkEvaluation::InProgress;
        }
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
                if (parked_cached_) {
                    return ParkEvaluation::Parked;
                }
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

    bool is_equipment_moving_error(const AlpacaException& ex) const {
        const std::string message = ex.what();
        return message.find("e4") != std::string::npos;
    }

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). Read as the Tracking getter reads it, except during
    // this driver's own GOTO: ":GU" may drop the tracking flag while a GOTO
    // runs, and a retarget of it must not be refused for that.
    void require_tracking_for_slew(const char* who) const {
        check_connected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (own_goto_in_flight_locked()) {
                return;
            }
        }
        if (!get_tracking()) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // Set by slew_to_target_async(); the force window covers the GOTO's start,
    // then the polled ":GU" (no 'N': the mount is slewing) its flight. A
    // rejected GOTO (last_slew_error_) is not in flight.
    bool own_goto_in_flight_locked() const { return goto_active_ && last_slew_error_.empty(); }

    // The gate the coordinate forms run before they touch the target: they
    // write it to the mount, which a refused slew must not.
    void check_equatorial_slew_allowed(const char* who) const {
        check_connected();
        ensure_not_parked(who);
        require_tracking_for_slew(who);
    }

    void reset_position_offsets() {
        // Signal cancellation first so the pulse thread's reconciliation read
        // (which runs outside mutex_) can exit early, avoiding lock contention.
        pulse_cancel_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ra_offset_hours_ = 0.0;
            dec_offset_deg_ = 0.0;
            pulse_generation_.fetch_add(1);
            pulse_guiding_end_ = clock_.now();
            snapshot_.write([](PolledFrame&) {});
        }
        {
            std::lock_guard<std::mutex> lock(pulse_mutex_);
            pulse_queue_.clear();
            pulse_queue_end_ = std::chrono::steady_clock::time_point{};
        }
    }

    void ensure_not_parked(const char* operation) const {
        if (!connected_.load()) {
            return;
        }

        // Park transitions are always driven by our own park()/unpark() code,
        // which updates parked_cached_ immediately.  Use the cached value to
        // avoid mount round-trips that add ~500ms+ latency on Wi-Fi links.
        // If a fresh park_state_cached_ is available, prefer it; otherwise
        // fall back to parked_cached_ rather than querying the mount.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = clock_.now();
            if (park_state_cached_.has_value() && (now - park_state_at_) <= kFastParkTtl) {
                if (park_state_cached_.value()) {
                    throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                          AlpacaError::InvalidWhileParked);
                }
                return;
            }
            // No fresh park query cache — trust parked_cached_ which is kept
            // in sync by park(), unpark(), and evaluate_park_state().
            if (parked_cached_) {
                throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                      AlpacaError::InvalidWhileParked);
            }
        }
    }

    // Returns false, having sent nothing more, once goto_cancel_ is set
    // (open-astro#720): the GOTO setup thread then exits.
    bool synchronize_mount_time_and_site_for_goto() {
        auto& protocol = protocol_;

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
            if (goto_cancel_.load()) {
                return false;
            }
            try {
                site_to_write = protocol.get_site_info();
            } catch (const std::exception&) {
            }
        }

        if (site_to_write.has_value()) {
            if (goto_cancel_.load()) {
                return false;
            }
            protocol.set_site_info(site_to_write.value());
            std::lock_guard<std::mutex> lock(mutex_);
            site_latitude_deg_ = site_to_write->latitude_degrees;
            site_longitude_deg_ = site_to_write->longitude_degrees;
            site_coords_valid_ = true;
        }

        const auto now = std::chrono::system_clock::now();
        const int offset_minutes = 0;
        const TimeInfo info = from_utc_time_point(now, offset_minutes);
        if (goto_cancel_.load()) {
            return false;
        }
        protocol.set_time_info(info);

        std::lock_guard<std::mutex> lock(mutex_);
        last_utc_set_ = now;
        last_utc_set_monotonic_ = clock_.now();
        last_utc_valid_ = true;
        timezone_offset_minutes_ = offset_minutes;
        timezone_valid_ = true;
        return true;
    }

    // Match Connected during the handshake; neither this gate nor a getter waits on the wire.
    void check_connected(bool safety_stop = false) const {
        update_link_state();
        if (!connected_.load() || !caches_ready_.load()) {
            throw AlpacaException("Telescope is not connected", AlpacaError::NotConnected);
        }
        if (!safety_stop) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto fault = link_fault_locked();
            if (!fault.empty()) throw AlpacaException(fault, AlpacaError::DriverException);
        }
    }

    void check_writable_state() const {
        update_link_state();
        bool lost = false;
        std::string fault;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost = !loss_reason_.empty();
            fault = link_fault_locked();
        }
        if (lost && !connected_.load()) throw AlpacaException("Telescope is not connected", AlpacaError::NotConnected);
        if (!fault.empty()) throw AlpacaException(fault, AlpacaError::DriverException);
    }

    std::string link_fault_locked() const {
        return health_.faulted() ? "ZWO mount communications compromised: " + health_.fault() : loss_reason_;
    }

    void update_link_state() const {
        if (!connected_.load()) return;
        std::unique_lock<std::mutex> lock(mutex_);
        const auto now = clock_.now();
        if (!connected_.load()) return;
        const bool transport_lost = protocol_.link_lost();
        // An intentional guide pause is not silence from the link; allow a full poll deadline afterward.
        const bool expired =
            caches_ready_.load() &&
            (health_.faulted() ? health_.fault_stale(now)
                               : now - std::max(last_mandatory_at_, pulse_guiding_end_) >= util::kLinkStalenessBound);
        if (!transport_lost && !expired) return;
        loss_reason_ = link_fault_locked();
        if (loss_reason_.empty())
            loss_reason_ = transport_lost ? "ZWO mount transport lost" : "ZWO mount telemetry stalled for 30 s";
        connected_.store(false);
        caches_ready_.store(false);
        link_lost_at_.note(now);
        snapshot_.reset();
        poll_stop_.store(true);
        pulse_cancel_.store(true);
        pulse_thread_stop_.store(true);
        goto_cancel_.store(true);
        protocol_.cancel_pending_io();
        lock.unlock();  // The state lock is a leaf; wake the motion workers outside it.
        {
            // Pair with each wait's predicate check before notifying, without changing a newer connect's flags.
            std::lock_guard<std::mutex> pulse_lock(pulse_mutex_);
            pulse_cv_.notify_all();
        }
        {
            std::lock_guard<std::mutex> goto_lock(goto_mutex_);
            goto_cv_.notify_all();
        }
    }

    template <typename T>
    static T measured(const std::optional<T>& value, const char* property) {
        if (!value)
            throw AlpacaException(std::string(property) + " telemetry is not available yet",
                                  AlpacaError::DriverException);
        return *value;
    }

    PolledFrame frame_locked() const {
        const auto reading = snapshot_.read();
        if (!reading) throw AlpacaException("Mount telemetry is not available yet", AlpacaError::DriverException);
        return reading->value;
    }

    PolledFrame read_frame() const {
        check_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return frame_locked();
    }

    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto reading = snapshot_.read();
        if (!reading) return std::nullopt;
        // A moving GU-only warm-up frame has no mandatory pair yet; retain its own measured age.
        return clock_.now() - reading->value.mandatory_at.value_or(reading->measured_at);
    }

    // Command-side mirrors are control state; only an accepted poll publishes measured telemetry.
    void write_frame_locked() {
        snapshot_.write([&](PolledFrame& frame) {
            frame.equatorial = cached_equatorial_;
            frame.tracking = tracking_state_valid_ ? std::optional<bool>(tracking_state_cached_) : std::nullopt;
            frame.parked = park_command_active_ ? std::optional<bool>(false) : park_state_cached_;
            if (frame.status && (goto_active_ || park_command_active_ || manual_axis_slewing_[0] ||
                                 manual_axis_slewing_[1] || slew_force_until_ > clock_.now())) {
                frame.status->at_home = false;
                frame.status->stop_or_tracking = false;
            }
        });
    }

    bool telemetry_paused() const {
        if (poll_pause_.load() || poll_stop_.load()) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        return clock_.now() < pulse_guiding_end_;
    }

    void refresh_cached_values() const {
        update_link_state();
        if (!connected_.load() || poll_stop_.load()) return;
        auto& protocol = protocol_;
        const auto token = snapshot_.begin_poll();
        PolledFrame frame;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const auto prior = snapshot_.read()) frame = prior->value;
        }
        std::optional<StatusInfo> status;
        std::optional<EquatorialCoordinates> eq;
        std::string failure;
        try {
            status = protocol.get_status();
            frame.tracking = !status->no_tracking;
            // Moving is evidence, but does not renew the mandatory sample's age.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!status->stop_or_tracking) frame.status = status;
                if (connected_.load() && snapshot_.publish(token, frame)) {
                    if (!status->stop_or_tracking) {
                        cached_status_ = status;
                    }
                    tracking_state_cached_ = *frame.tracking;
                    tracking_state_valid_ = true;
                }
            }
        } catch (const std::exception& e) {
            failure = e.what();
        }
        // A pause between GU and the pair is skipped work, not a failed exchange.
        if (telemetry_paused()) return;
        if (status) {
            try {
                eq = protocol.get_current_equatorial();
            } catch (const std::exception& e) {
                failure = e.what();
            }
        }
        update_link_state();
        if (!connected_.load() || poll_stop_.load()) return;
        if (!status || !eq) {
            std::optional<std::string> latched;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_.load()) return;
                latched = health_.note_failure(failure, 3, clock_.now());
            }
            if (latched) ALPACA_LOG_ERROR("ZWO", "Mount communications compromised: " + *latched);
            return;
        }
        frame.status = status;
        frame.equatorial = eq;
        frame.tracking = !status->no_tracking;
        frame.mandatory_at = clock_.now();
        bool restored = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_.load() || token != snapshot_.begin_poll()) return;
            const auto now = clock_.now();
            if (!snapshot_.publish(token, frame)) return;
            cached_status_ = status;
            cached_equatorial_ = eq;
            cached_equatorial_at_ = now;
            tracking_state_cached_ = *frame.tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = now;
            last_mandatory_at_ = *frame.mandatory_at;
            restored = health_.on_reply();
            // GOTO ownership ends only with accepted final coordinates, never GU alone.
            if (status->stop_or_tracking && slew_force_until_ <= now) goto_active_ = false;
        }
        if (restored) ALPACA_LOG_INFO("ZWO", "Mount communications restored");
        // Optional fields cannot postpone mandatory publication or restore a failed mandatory poll.
        if (telemetry_paused()) return;
        frame.horizontal.reset();
        try {
            frame.horizontal = protocol.get_current_horizontal();
        } catch (const std::exception&) {
        }
        if (telemetry_paused()) return;
        std::optional<ParkStatus> park_status;
        try {
            park_status = protocol.get_park_status();
        } catch (const std::exception&) {
        }
        if (telemetry_paused()) return;
        bool has_site = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            has_site = site_coords_valid_;
        }
        if (!has_site) {
            frame.pier_side.reset();
            try {
                const auto direction = protocol.get_mount_direction();
                frame.pier_side = (direction == 'E' || direction == 'e')   ? 1
                                  : (direction == 'W' || direction == 'w') ? 0
                                                                           : -1;
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Optional fallback failure leaves SideOfPier unavailable, not healthy or guessed.
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load() || token != snapshot_.begin_poll()) return;
        if (park_command_active_ && park_status == ParkStatus::Error) {
            park_failed_ = true;
            last_slew_error_ = "Park failed: mount reported a park error";
        }
        const auto evaluation = unpark_in_progress_
                                    ? ParkEvaluation::NotParked
                                    : evaluate_park_state_locked(park_status, status, *frame.mandatory_at);
        frame.parked = evaluation == ParkEvaluation::Parked || (!unpark_in_progress_ && parked_cached_);
        if (!snapshot_.publish(token, frame)) return;
        park_state_cached_ = frame.parked;
        park_state_at_ = *frame.mandatory_at;
    }

    void start_poll_thread() {
        stop_poll_thread();
        poll_stop_.store(false);
        poll_operation_.start([this](util::OperationContext& ctx) {
            using namespace std::chrono_literals;
            auto next_time_refresh = clock_.now() + 30s;
            while (!poll_stop_.load() && ctx.stop_reason() == util::StopReason::None) {
                update_link_state();
                // pulse_guiding_end_ is mutex_-guarded everywhere else; an
                // unlocked read here is torn against a concurrent pulse (M12).
                std::chrono::steady_clock::time_point pulse_end;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pulse_end = pulse_guiding_end_;
                }
                if (poll_pause_.load() || clock_.now() < pulse_end) {
                    if (!ctx.wait_for(50ms)) break;
                    continue;
                }
                if (!connected_.load()) {
                    break;
                }
                refresh_cached_values();
                const auto now = clock_.now();
                if (!poll_stop_.load() && now >= next_time_refresh) {
                    try {
                        const TimeInfo mount_time = protocol_.get_time_info();
                        const auto utc = to_utc_time_point(mount_time);
                        std::lock_guard<std::mutex> lock(mutex_);
                        last_utc_set_ = utc;
                        last_utc_set_monotonic_ = clock_.now();
                        last_utc_valid_ = true;
                        timezone_offset_minutes_ = mount_time.timezone_offset_minutes;
                        timezone_valid_ = true;
                    } catch (const std::exception&) {
                    }
                    next_time_refresh = now + std::chrono::seconds(30);
                }
                if (!ctx.wait_for(250ms)) break;
            }
            if (!connected_.load()) protocol_.try_disconnect();
        });
    }

    void stop_poll_thread() {
        poll_stop_.store(true);
        poll_operation_.cancel_all_and_join();
    }

    struct PulseTask {
        int direction = 0;
        int duration_ms = 0;
        bool ra_axis = false;
        double guide_rate_deg_per_sec = 0.0;
        double expected_ra_hours = 0.0;
        double expected_dec_degrees = 0.0;
        uint64_t generation = 0;
    };

    void run_pulse_task(const PulseTask& task) {
        auto& protocol = protocol_;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = clock_.now();
            const auto end = now + std::chrono::milliseconds(task.duration_ms) + kPulseGuideHold;
            if (end > pulse_guiding_end_) {
                pulse_guiding_end_ = end;
            }
        }

        ALPACA_LOG_INFO(
            "ZWO",
            "PulseGuide exec dir=" + std::to_string(task.direction) +
                " duration_ms=" + std::to_string(task.duration_ms));

        int remaining_ms = task.duration_ms;
        while (remaining_ms > 0 && !pulse_thread_stop_.load() && !pulse_cancel_.load()) {
            const int step = std::min(remaining_ms, 3000);
            try {
                protocol.pulse_guide(task.direction, step);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "PulseGuide command failed: " + std::string(e.what()));
                break;
            }

            int wait_ms = step;
            while (wait_ms > 0 && !pulse_thread_stop_.load() && !pulse_cancel_.load()) {
                const int sleep_ms = std::min(wait_ms, 100);
                std::unique_lock<std::mutex> lock(pulse_mutex_);
                clock_.wait_for(lock, pulse_cv_, std::chrono::milliseconds(sleep_ms),
                                [this] { return pulse_thread_stop_.load() || pulse_cancel_.load(); });
                wait_ms -= sleep_ms;
            }

            remaining_ms -= step;
        }

        // Rebase the predictive offset against the mount's actual position.
        // The mount may or may not update its reported coordinates during pulse
        // guiding; reconciling here prevents double-counting (offset + actual
        // movement) without losing the prediction when coordinates don't update.
        if (!pulse_cancel_.load() && task.generation == pulse_generation_.load()) {
            const auto token = snapshot_.begin_poll();
            try {
                // Best-effort reconciliation must not hold up the next queued pulse for a full link timeout.
                const EquatorialCoordinates eq = protocol.get_current_equatorial(250);
                std::lock_guard<std::mutex> lock(mutex_);
                if (!connected_.load() || pulse_cancel_.load() || task.generation != pulse_generation_.load() ||
                    token != snapshot_.begin_poll())
                    return;  // IsPulseGuiding uses the published deadline, not this worker's hold loop.
                if (cached_equatorial_.has_value()) {
                    // Compute the expected final position from the pre-pulse base + offset.
                    double target_ra = std::fmod(cached_equatorial_->ra_hours + ra_offset_hours_, 24.0);
                    if (target_ra < 0.0) target_ra += 24.0;
                    const double target_dec = std::clamp(
                        cached_equatorial_->dec_degrees + dec_offset_deg_, -90.0, 90.0);
                    // Rebase: set offset so actual_pos + new_offset = expected_final.
                    ra_offset_hours_ = normalize_hour_angle_hours(target_ra - eq.ra_hours);
                    dec_offset_deg_ = target_dec - eq.dec_degrees;
                }
                cached_equatorial_ = eq;
                cached_equatorial_at_ = clock_.now();
                snapshot_.write([&](PolledFrame& frame) { frame.equatorial = eq; });
            } catch (const std::exception&) {
                // A failed reconciliation is not a new measurement.
            }
        }

        // Only hold after the last queued task.  When multiple axes are
        // pulsed simultaneously (e.g. East + North), skipping the hold
        // between tasks prevents the total time from exceeding ConformU's
        // 6-second IsPulseGuiding timeout.
        {
            bool more_queued = false;
            {
                std::lock_guard<std::mutex> lock(pulse_mutex_);
                more_queued = !pulse_queue_.empty();
            }
            if (!more_queued) {
                int hold_ms = static_cast<int>(kPulseGuideHold.count());
                while (hold_ms > 0 && !pulse_thread_stop_.load() && !pulse_cancel_.load()) {
                    const int sleep_ms = std::min(hold_ms, 100);
                    std::unique_lock<std::mutex> lock(pulse_mutex_);
                    clock_.wait_for(lock, pulse_cv_, std::chrono::milliseconds(sleep_ms),
                                    [this] { return pulse_thread_stop_.load() || pulse_cancel_.load(); });
                    hold_ms -= sleep_ms;
                }
            }
        }
    }

    void start_pulse_thread() {
        std::lock_guard<std::mutex> lock(pulse_mutex_);
        if (pulse_thread_.joinable()) {
            return;
        }
        pulse_thread_stop_.store(false);
        pulse_cancel_.store(false);
        pulse_thread_ = std::thread([this]() {
            while (true) {
                PulseTask task;
                {
                    std::unique_lock<std::mutex> lock(pulse_mutex_);
                    pulse_cv_.wait(lock, [&]() {
                        return pulse_thread_stop_.load() || !pulse_queue_.empty();
                    });
                    if (pulse_thread_stop_.load()) {
                        break;
                    }
                    task = pulse_queue_.front();
                    pulse_queue_.pop_front();
                }
                run_pulse_task(task);
            }
        });
    }

    void stop_pulse_thread() {
        {
            std::lock_guard<std::mutex> lock(pulse_mutex_);
            pulse_thread_stop_.store(true);
            pulse_cancel_.store(true);
        }
        pulse_cv_.notify_all();
        if (pulse_thread_.joinable()) {
            pulse_thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(pulse_mutex_);
            pulse_queue_.clear();
            pulse_queue_end_ = std::chrono::steady_clock::time_point{};
            pulse_thread_stop_.store(false);
            pulse_cancel_.store(false);
        }
    }

    // open-astro#575: every clear of the stored slew failure (new initiator,
    // AbortSlew, reconnect) starts a new epoch; see slew_error_epoch_.
    void clear_last_slew_error_locked() {
        last_slew_error_.clear();
        park_failed_ = false;
        ++slew_error_epoch_;
        write_frame_locked();
    }

    // Ask a running GOTO setup thread to exit promptly (it re-checks the
    // flag between protocol calls and its retry backoff waits on goto_cv_).
    // The store happens under goto_mutex_ so a waiter cannot miss the wakeup.
    // Flag only: the callers join afterwards (cancel_and_join_goto_thread(),
    // slew_to_target_async()'s reap, the destructor).
    void cancel_goto_thread_request() {
        {
            std::lock_guard<std::mutex> lock(goto_mutex_);
            goto_cancel_.store(true);
        }
        goto_cv_.notify_all();
    }

    // open-astro#720 fence: after this returns no GOTO setup thread started
    // before the call can put anything on the wire. The join waits for at
    // most the one mount round trip in flight (the body checks goto_cancel_
    // between protocol calls). The caller must NOT hold mutex_, pulse_mutex_
    // or the protocol mutex: the GOTO body takes mutex_ and the protocol
    // mutex, so a join under either deadlocks. Does not reset goto_cancel_;
    // only slew_to_target_async() does, under goto_mutex_.
    void cancel_and_join_goto_thread() {
        {
            std::lock_guard<std::mutex> reap_lock(goto_reap_mutex_);
            cancel_goto_thread_request();
            if (goto_thread_.joinable()) {
                goto_thread_.join();
            }
        }
        // Every caller starts other motion or none (Park, FindHome, MoveAxis,
        // AbortSlew, disconnect): the driver's GOTO is no longer in flight.
        // Taken after the reap lock is released, never nested in it.
        std::lock_guard<std::mutex> lock(mutex_);
        goto_active_ = false;
    }

    const int device_number_;
    const ConnectionInfo connection_info_;

    // Outlives every worker; the destructor joins them before closing this entry's link.
    mutable ZWOMountProtocolWrapper protocol_;
    util::TaskClock& clock_;
    mutable util::StateSnapshot<PolledFrame> snapshot_;
    util::OperationGeneration poll_generation_;
    util::AsyncOperation poll_operation_;
    mutable util::PolledLinkHealth health_;
    mutable util::LinkLostStamp link_lost_at_;
    mutable std::string loss_reason_;
    mutable util::TaskClock::clock::time_point last_mandatory_at_{};

    mutable std::atomic<bool> connected_;
    // The initial telemetry attempt has finished, successfully or not.
    mutable std::atomic<bool> caches_ready_;

    mutable std::mutex mutex_;
    // Serialises every site write to the mount: the latitude/longitude setters, and the connect's site apply,
    // read-back and re-send. No getter takes it. Order: set_connected_mutex_, then this, then the protocol
    // wrapper's mutex, then mutex_.
    std::mutex site_write_mutex_;
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect

    std::string mount_info_;

    double target_ra_hours_;
    double target_dec_degrees_;
    bool target_ra_set_;
    bool target_dec_set_;
    mutable double ra_offset_hours_;
    mutable double dec_offset_deg_;

    double aperture_diameter_m_;
    double aperture_area_m2_;
    double focal_length_m_;

    mutable double site_latitude_deg_;
    mutable double site_longitude_deg_;
    mutable bool site_coords_valid_;
    std::optional<double> site_elevation_m_;  // nullopt until set or configured

    int slew_settle_time_s_;

    mutable GuideRate guide_rate_;
    // Bumped under mutex_ by every set_guide_rate(); the connect publishes its reading only when unchanged.
    std::uint64_t guide_rate_writes_{0};
    mutable int tracking_rate_cached_;
    mutable bool tracking_rate_valid_;
    mutable bool tracking_state_cached_;
    mutable bool tracking_state_valid_;
    mutable std::chrono::steady_clock::time_point tracking_state_at_;
    mutable std::optional<EquatorialCoordinates> cached_equatorial_;
    mutable std::optional<StatusInfo> cached_status_;
    mutable std::optional<bool> park_state_cached_;
    mutable std::chrono::steady_clock::time_point cached_equatorial_at_;
    mutable std::chrono::steady_clock::time_point park_state_at_;
    mutable std::atomic<bool> poll_stop_;
    std::atomic<bool> poll_pause_;

    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;

    mutable int timezone_offset_minutes_;
    mutable bool timezone_valid_;

    std::array<bool, 2> manual_axis_slewing_;
    std::array<std::optional<bool>, 2> manual_axis_tracking_restore_;
    std::chrono::steady_clock::time_point slew_force_until_;
    // A GOTO this driver started may still be in flight (open-astro#1274):
    // see own_goto_in_flight_locked(). Under mutex_.
    mutable bool goto_active_ = false;
    // open-astro#575: an async slew that fails AFTER the initiator returned
    // (mount rejects the GOTO, logged and forgotten by the setup thread) used
    // to leave Slewing read FALSE once slew_force_until_ expired --
    // indistinguishable from a landed goto. Set (under mutex_) by the GOTO
    // setup thread on a REAL failure (never on the thread's own
    // cancellation), cleared by the next slew initiator, AbortSlew, and
    // connect/disconnect. Consulted by get_slewing() before the force window.
    mutable std::string last_slew_error_;
    mutable bool park_failed_ = false;
    // Bumped by every clear_last_slew_error_locked() (under mutex_): the GOTO
    // setup thread records a failure only if no newer command cleared the
    // slot since its initiator did.
    uint64_t slew_error_epoch_ = 0;
    std::chrono::steady_clock::time_point pulse_guiding_end_;
    std::atomic<uint64_t> pulse_generation_;
    mutable std::mutex pulse_mutex_;
    mutable std::condition_variable pulse_cv_;
    std::deque<PulseTask> pulse_queue_;
    std::thread pulse_thread_;
    mutable std::atomic<bool> pulse_thread_stop_;
    mutable std::atomic<bool> pulse_cancel_;
    // Async teardown spawned by set_connected(false) — a joinable member so
    // the destructor can join it (a detached thread touching `this` is a
    // use-after-free if the driver dies mid-teardown; driver-lifecycle.instructions.md rule).
    std::thread disconnect_thread_;
    // Serializes set_connected() bodies (sync PUT / async task / refresh) so
    // poll_operation_ start/join, pulse_thread_/disconnect_thread_ join+assign and their
    // stop flags can never interleave between two bodies. Taken after the
    // base pending-disconnect gates; never taken by any worker-thread body.
    std::mutex set_connected_mutex_;
    // Async GOTO setup thread (~1 s of protocol calls + retries) — joinable
    // member with cancel flag + cv, joined on the next GOTO and in the
    // destructor; the cv makes the retry backoff cancellable.
    std::thread goto_thread_;
    // Serializes the cancel/join/respawn of goto_thread_ in
    // slew_to_coordinates_async: two concurrent slew calls would otherwise
    // join and assign the same std::thread member from two threads (UB).
    // Never taken by the GOTO thread body, so the join under it cannot hang.
    std::mutex goto_reap_mutex_;
    mutable std::atomic<bool> goto_cancel_{false};
    mutable std::mutex goto_mutex_;
    mutable std::condition_variable goto_cv_;
    std::chrono::steady_clock::time_point pulse_queue_end_;
    mutable bool parked_cached_;
    mutable bool park_command_active_;
    mutable std::chrono::steady_clock::time_point park_command_started_;
    mutable bool park_motion_seen_;
    // Set while unpark() is issuing :Spu so the poll thread's park-state warm
    // does not re-cache Completed from a stale :Gps read (mount firmware lag).
    mutable bool unpark_in_progress_;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;
};

std::unique_ptr<TelescopeDriver> create_zwo_telescope(int device_number, const ConnectionInfo& connection_info,
                                                      util::TaskClock& clock) {
    return create_zwo_telescope_with_site(device_number, connection_info, std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt, clock);
}

std::unique_ptr<TelescopeDriver> create_zwo_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock) {
    return std::make_unique<ZWOTelescopeDriver>(device_number, connection_info, site_latitude_deg, site_longitude_deg,
                                                site_elevation_m, sync_time_on_connect, clock);
}

} // namespace alpacacore::vendor::zwo

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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
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

std::string trim_copy(std::string value) {
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(),
        value.end());
    return value;
}

std::optional<int> extract_mount_error_code(const std::string& response) {
    if (response.empty()) {
        return std::nullopt;
    }
    if (response[0] != 'e' && response[0] != 'E') {
        return std::nullopt;
    }
    if (response.size() < 2 || !std::isdigit(static_cast<unsigned char>(response[1]))) {
        return std::nullopt;
    }

    std::string digits;
    for (std::size_t i = 1; i < response.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(response[i]);
        if (std::isdigit(ch)) {
            digits.push_back(static_cast<char>(ch));
        } else {
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    try {
        return std::stoi(digits);
    } catch (const std::exception&) {
        // A pathological digit run overflows std::stoi (std::out_of_range);
        // treat it as "no recognizable error code".
        return std::nullopt;
    }
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
public:
    ZWOTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                       std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                       std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect)
        : AsyncConnectable("ZWO"),
          device_number_(device_number),
          connection_info_(connection_info),
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
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          does_refraction_(false),
          slew_settle_time_s_(0),
          guide_rate_({0.5 * kSiderealRateDegPerSec, 0.5 * kSiderealRateDegPerSec}),
          tracking_rate_cached_(0),
          tracking_rate_valid_(false),
          tracking_state_cached_(false),
          tracking_state_valid_(false),
          tracking_state_at_(std::chrono::steady_clock::time_point{}),
          cached_equatorial_(),
          cached_horizontal_(),
          cached_status_(),
          cached_pier_side_(),
          park_state_cached_(),
          cached_equatorial_at_(std::chrono::steady_clock::time_point{}),
          cached_horizontal_at_(std::chrono::steady_clock::time_point{}),
          cached_status_at_(std::chrono::steady_clock::time_point{}),
          cached_pier_side_at_(std::chrono::steady_clock::time_point{}),
          park_state_at_(std::chrono::steady_clock::time_point{}),
          pending_slew_adjust_(false),
          pending_slew_ra_hours_(0.0),
          pending_slew_dec_degrees_(0.0),
          pending_slew_at_(std::chrono::steady_clock::time_point{}),
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

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        // Gate on caches_ready_ so clients (ConformU) only see Connected=true
        // once the telemetry caches have been warmed after connect — the first
        // property reads after connect then hit warm caches and stay under the
        // ASCOM FAST 0.1 s target. connected_ itself is set early (inside the
        // lock) to keep concurrent Connected=true requests on the no-op path.
        return connected_.load() && caches_ready_.load();
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
            if (!connected_.exchange(false)) {
                return;
            }
            caches_ready_.store(false);
            poll_stop_.store(true);
            pulse_thread_stop_.store(true);
            pulse_cancel_.store(true);
            pulse_cv_.notify_all();
            cancel_goto_thread_request();

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
                    pending_slew_adjust_ = false;
                    pending_slew_ra_hours_ = 0.0;
                    pending_slew_dec_degrees_ = 0.0;
                    pending_slew_at_ = std::chrono::steady_clock::time_point{};
                    manual_axis_tracking_restore_[0] = std::nullopt;
                    manual_axis_tracking_restore_[1] = std::nullopt;
                    target_ra_hours_ = 0.0;
                    target_dec_degrees_ = 0.0;
                    target_ra_set_ = false;
                    target_dec_set_ = false;
                }
                stop_poll_thread();
                stop_pulse_thread();
                try {
                    ZWOMountProtocolWrapper::instance().disconnect();
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

        std::unique_lock<std::mutex> lock(mutex_);
        auto& protocol = ZWOMountProtocolWrapper::instance();
        auto reset_session_state_for_connect = [&](bool keep_telemetry_caches = false) {
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            slew_force_until_ = std::chrono::steady_clock::time_point{};
            pulse_guiding_end_ = std::chrono::steady_clock::time_point{};
            ra_offset_hours_ = 0.0;
            dec_offset_deg_ = 0.0;
            if (!keep_telemetry_caches) {
                cached_equatorial_.reset();
                cached_horizontal_.reset();
                cached_status_.reset();
                cached_pier_side_.reset();
                park_state_cached_.reset();
                cached_equatorial_at_ = std::chrono::steady_clock::time_point{};
                cached_horizontal_at_ = std::chrono::steady_clock::time_point{};
                cached_status_at_ = std::chrono::steady_clock::time_point{};
                cached_pier_side_at_ = std::chrono::steady_clock::time_point{};
                park_state_at_ = std::chrono::steady_clock::time_point{};
                tracking_state_valid_ = false;
                tracking_state_at_ = std::chrono::steady_clock::time_point{};
            }
            poll_pause_.store(false);
            {
                std::lock_guard<std::mutex> pulse_lock(pulse_mutex_);
                pulse_queue_.clear();
                pulse_queue_end_ = std::chrono::steady_clock::time_point{};
                pulse_thread_stop_.store(false);
                pulse_cancel_.store(false);
            }
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
            pending_slew_adjust_ = false;
            pending_slew_ra_hours_ = 0.0;
            pending_slew_dec_degrees_ = 0.0;
            pending_slew_at_ = std::chrono::steady_clock::time_point{};
            manual_axis_tracking_restore_[0] = std::nullopt;
            manual_axis_tracking_restore_[1] = std::nullopt;
            target_ra_hours_ = 0.0;
            target_dec_degrees_ = 0.0;
            target_ra_set_ = false;
            target_dec_set_ = false;
            pulse_generation_.store(0);
        };
        auto apply_or_load_site_info = [&]() {
            std::optional<SiteInfo> preferred_site;
            if (pending_site_latitude_.has_value() || pending_site_longitude_.has_value() || site_coords_valid_) {
                SiteInfo site;
                bool has_latitude = false;
                bool has_longitude = false;
                if (pending_site_latitude_.has_value()) {
                    site.latitude_degrees = pending_site_latitude_.value();
                    has_latitude = true;
                } else if (site_coords_valid_) {
                    site.latitude_degrees = site_latitude_deg_;
                    has_latitude = true;
                }
                if (pending_site_longitude_.has_value()) {
                    site.longitude_degrees = pending_site_longitude_.value();
                    has_longitude = true;
                } else if (site_coords_valid_) {
                    site.longitude_degrees = site_longitude_deg_;
                    has_longitude = true;
                }
                if (has_latitude && has_longitude) {
                    preferred_site = site;
                }
            }

            if (preferred_site.has_value()) {
                try {
                    protocol.set_site_info(preferred_site.value());
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
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
            } catch (const std::exception&) {
            }
        };
        auto sync_mount_time_if_configured = [&]() {
            if (!sync_time_on_connect_) {
                return;
            }

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
            reset_session_state_for_connect(/*keep_telemetry_caches=*/true);
            apply_or_load_site_info();
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            try {
                sync_mount_time_if_configured();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(
                    "ZWO", "Unable to synchronize mount time/site on Connected=true refresh: " + std::string(e.what()));
            }
            lock.unlock();
            refresh_cached_values();
            start_poll_thread();
            start_pulse_thread();
            return;
        }

        if (connected) {
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to ZWO mount", AlpacaError::NotConnected);
            }
            // Publish connected_ immediately (inside the lock) so a concurrent
            // Connected=true request takes the no-op path below instead of
            // re-entering protocol.connect() and tearing down this fresh link.
            // get_connected() additionally gates on caches_ready_, so clients
            // only observe Connected=true once the telemetry caches below are
            // warm — preserving the FAST-target ordering (first property reads
            // after connect hit warm caches, not live ~95 ms serial reads).
            connected_.store(true);
            caches_ready_.store(false);
            reset_session_state_for_connect();

            try {
                mount_info_ = protocol.get_mount_info();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Unable to query mount info: " + std::string(e.what()));
                mount_info_.clear();
            }

            apply_or_load_site_info();

            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }

            bool guide_rate_set = false;
            for (int attempt = 0; attempt < 3 && !guide_rate_set; ++attempt) {
                try {
                    const double guide_rate_multiple = protocol.get_guide_rate();
                    const double guide_rate_deg_per_sec = guide_rate_multiple * kSiderealRateDegPerSec;
                    guide_rate_ = {guide_rate_deg_per_sec, guide_rate_deg_per_sec};
                    guide_rate_set = true;
                } catch (const std::exception&) {
                    if (attempt == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    }
                }
            }
            if (!guide_rate_set) {
                const double fallback_multiple = 1.00;
                const double fallback_deg_per_sec = fallback_multiple * kSiderealRateDegPerSec;
                ALPACA_LOG_WARN("ZWO", "Unable to read guide rate; assuming 1.00x sidereal until updated");
                guide_rate_ = {fallback_deg_per_sec, fallback_deg_per_sec};
            }

            try {
                tracking_rate_cached_ = protocol.get_tracking_rate();
                tracking_rate_valid_ = true;
            } catch (const std::exception&) {
            }

            sync_mount_time_if_configured();
            lock.unlock();
            // Warm every telemetry cache before advertising Connected=true (via
            // caches_ready_) so a client that reads properties immediately after
            // seeing Connected hits warm caches instead of live serial reads.
            refresh_cached_values(/*require_connected=*/false);
            caches_ready_.store(true);
            start_poll_thread();
            start_pulse_thread();
            return;
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
        StatusInfo status;
        bool has_status = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_status_.has_value()) {
                status = cached_status_.value();
                has_status = true;
            }
        }
        if (!has_status) {
            status = ZWOMountProtocolWrapper::instance().get_status();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_status_ = status;
            cached_status_at_ = std::chrono::steady_clock::now();
        }
        if (status.mode == MountMode::AltAzimuth) {
            return AlignmentMode::AltAz;
        }
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override {
        check_connected();
        HorizontalCoordinates hor;
        bool has_hor = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_horizontal_.has_value()) {
                hor = cached_horizontal_.value();
                has_hor = true;
            }
        }
        if (!has_hor) {
            hor = ZWOMountProtocolWrapper::instance().get_current_horizontal();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_horizontal_ = hor;
            cached_horizontal_at_ = std::chrono::steady_clock::now();
        }
        return hor.altitude_degrees;
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
        StatusInfo status;
        bool has_status = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_status_.has_value()) {
                status = cached_status_.value();
                has_status = true;
            }
        }
        if (!has_status) {
            status = ZWOMountProtocolWrapper::instance().get_status();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_status_ = status;
            cached_status_at_ = std::chrono::steady_clock::now();
        }
        return status.at_home;
    }

    bool get_at_park() const override {
        check_connected();
        auto& protocol = ZWOMountProtocolWrapper::instance();

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (park_state_cached_.has_value() && (now - park_state_at_) <= kFastParkTtl) {
                return park_state_cached_.value();
            }
        }

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
        if (park_eval == ParkEvaluation::Parked) {
            std::lock_guard<std::mutex> lock(mutex_);
            park_state_cached_ = true;
            park_state_at_ = now;
            return true;
        }
        if (park_eval == ParkEvaluation::InProgress || park_eval == ParkEvaluation::NotParked) {
            std::lock_guard<std::mutex> lock(mutex_);
            park_state_cached_ = false;
            park_state_at_ = now;
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        park_state_cached_ = parked_cached_;
        park_state_at_ = now;
        return parked_cached_;
    }

    double get_azimuth() const override {
        check_connected();
        HorizontalCoordinates hor;
        bool has_hor = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_horizontal_.has_value()) {
                hor = cached_horizontal_.value();
                has_hor = true;
            }
        }
        if (!has_hor) {
            hor = ZWOMountProtocolWrapper::instance().get_current_horizontal();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_horizontal_ = hor;
            cached_horizontal_at_ = std::chrono::steady_clock::now();
        }
        return hor.azimuth_degrees;
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
        apply_pending_slew_adjustment();
        EquatorialCoordinates eq;
        double offset = 0.0;
        bool has_eq = false;
        {
            // Read base position and offset atomically to prevent the pulse
            // thread's reconciliation from updating both between the reads.
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_equatorial_.has_value() &&
                (std::chrono::steady_clock::now() - cached_equatorial_at_) <= kFastEquatorialTtlForRead) {
                eq = cached_equatorial_.value();
                offset = dec_offset_deg_;
                has_eq = true;
            }
        }
        if (!has_eq) {
            eq = ZWOMountProtocolWrapper::instance().get_current_equatorial();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_equatorial_ = eq;
            cached_equatorial_at_ = std::chrono::steady_clock::now();
            offset = dec_offset_deg_;
        }
        return std::clamp(eq.dec_degrees + offset, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        return 0.0;
    }

    void set_declination_rate(double) override {
        throw AlpacaException("DeclinationRate is not supported", AlpacaError::NotImplemented);
    }

    bool get_tracking() const override {
        check_connected();
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tracking_state_valid_ && (now - tracking_state_at_) <= kFastTrackingTtl) {
                return tracking_state_cached_;
            }
        }
        auto& protocol = ZWOMountProtocolWrapper::instance();
        const std::string response = trim_copy(protocol.send_command(":GAT", false));
        if (!response.empty() && (response[0] == '0' || response[0] == '1')) {
            const bool tracking = response[0] == '1';
            std::lock_guard<std::mutex> lock(mutex_);
            tracking_state_cached_ = tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = std::chrono::steady_clock::now();
            return tracking;
        }

        if (const auto mount_error = extract_mount_error_code(response); mount_error.has_value()) {
            bool has_cached_tracking = false;
            bool cached_tracking = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                has_cached_tracking = tracking_state_valid_;
                cached_tracking = tracking_state_cached_;
            }
            if (has_cached_tracking) {
                ALPACA_LOG_WARN(
                    "ZWO",
                    ":GAT returned e" + std::to_string(mount_error.value()) +
                        "; using cached Tracking state");
                return cached_tracking;
            }

            try {
                const StatusInfo status = protocol.get_status();
                const bool inferred_tracking = !status.no_tracking;
                std::lock_guard<std::mutex> lock(mutex_);
                tracking_state_cached_ = inferred_tracking;
                tracking_state_valid_ = true;
                tracking_state_at_ = std::chrono::steady_clock::now();
                return inferred_tracking;
            } catch (const std::exception&) {
                throw AlpacaException(
                    "Unable to determine tracking state from mount response: " + response,
                    AlpacaError::DriverException);
            }
        }

        throw AlpacaException("Unknown tracking state response: " + response, AlpacaError::DriverException);
    }

    void set_tracking(bool tracking) override {
        check_connected();
        if (tracking) {
            // Per ITelescopeV4 / ConformU: enabling tracking while parked must
            // throw InvalidWhileParked; disabling is allowed (harmless). Check
            // BEFORE the tracking-state fast path below, which would otherwise
            // return early without the parked validation.
            ensure_not_parked("Tracking");
        }
        auto& protocol = ZWOMountProtocolWrapper::instance();
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (tracking_state_valid_ && (now - tracking_state_at_) <= kFastTrackingTtl &&
                tracking_state_cached_ == tracking) {
                return;
            }
        }
        auto wait_for_pulse_completion = [this](std::chrono::milliseconds max_wait) {
            const auto deadline = std::chrono::steady_clock::now() + max_wait;
            while (std::chrono::steady_clock::now() < deadline) {
                std::chrono::steady_clock::time_point pulse_end;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pulse_end = pulse_guiding_end_;
                }
                const auto now = std::chrono::steady_clock::now();
                if (pulse_end <= now) {
                    return;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(pulse_end - now);
                if (remaining > std::chrono::milliseconds(200)) {
                    remaining = std::chrono::milliseconds(200);
                }
                std::this_thread::sleep_for(remaining);
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

        std::optional<bool> current_state;
        try {
            current_state = get_tracking();
        } catch (const std::exception&) {
            current_state = infer_tracking_from_status();
        }

        if (current_state.has_value() && current_state.value() == tracking) {
            std::lock_guard<std::mutex> lock(mutex_);
            tracking_state_cached_ = tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = std::chrono::steady_clock::now();
            return;
        }

        if (command_ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            tracking_state_cached_ = tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = std::chrono::steady_clock::now();
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

        if (connected_.load() && effective > 0.0) {
            double guide_rate_multiple = effective / kSiderealRateDegPerSec;
            guide_rate_multiple = std::clamp(guide_rate_multiple, 0.10, 0.90);
            // set_guide_rate() returns the readback value from its internal
            // verification — no extra round-trip needed.
            const double actual_multiple =
                ZWOMountProtocolWrapper::instance().set_guide_rate(guide_rate_multiple);
            const double actual_deg_per_sec = actual_multiple * kSiderealRateDegPerSec;
            std::lock_guard<std::mutex> lock(mutex_);
            guide_rate_ = {actual_deg_per_sec, actual_deg_per_sec};
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        guide_rate_ = {effective, effective};
    }

    double get_right_ascension() const override {
        check_connected();
        apply_pending_slew_adjustment();
        EquatorialCoordinates eq;
        double offset = 0.0;
        bool has_eq = false;
        {
            // Read base position and offset atomically to prevent the pulse
            // thread's reconciliation from updating both between the reads,
            // which would pair a stale base with a rebased offset and produce
            // a ~1-second tracking-drift error.
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_equatorial_.has_value() &&
                (std::chrono::steady_clock::now() - cached_equatorial_at_) <= kFastEquatorialTtlForRead) {
                eq = cached_equatorial_.value();
                offset = ra_offset_hours_;
                has_eq = true;
            }
        }
        if (!has_eq) {
            eq = ZWOMountProtocolWrapper::instance().get_current_equatorial();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_equatorial_ = eq;
            cached_equatorial_at_ = std::chrono::steady_clock::now();
            offset = ra_offset_hours_;
        }
        double wrapped = std::fmod(eq.ra_hours + offset, 24.0);
        if (wrapped < 0.0) {
            wrapped += 24.0;
        }
        return wrapped;
    }

    double get_right_ascension_rate() const override {
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
            std::lock_guard<std::mutex> lock(mutex_);
            cached_pier_side_ = side;
            cached_pier_side_at_ = std::chrono::steady_clock::now();
            return side;
        }

        // Fallback: site longitude not yet known, use :Gm# with ASCOM-correct mapping.
        const char direction = ZWOMountProtocolWrapper::instance().get_mount_direction();
        int side = -1;
        if (direction == 'E' || direction == 'e') {
            side = 1;  // telescope pointing east → pierWest
        } else if (direction == 'W' || direction == 'w') {
            side = 0;  // telescope pointing west → pierEast
        }
        std::lock_guard<std::mutex> lock(mutex_);
        cached_pier_side_ = side;
        cached_pier_side_at_ = std::chrono::steady_clock::now();
        return side;
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
        if (!has_longitude && connected_.load()) {
            try {
                const SiteInfo site = ZWOMountProtocolWrapper::instance().get_site_info();
                longitude = site.longitude_degrees;
                std::lock_guard<std::mutex> lock(mutex_);
                site_latitude_deg_ = site.latitude_degrees;
                site_longitude_deg_ = site.longitude_degrees;
                site_coords_valid_ = true;
                has_longitude = true;
            } catch (const std::exception&) {
            }
        }
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_coords_valid_) {
            throw AlpacaException("Site coordinates not available", AlpacaError::ValueNotSet);
        }
        return site_latitude_deg_;
    }

    void set_site_latitude(double latitude) override {
        validate_latitude(latitude);

        std::lock_guard<std::mutex> lock(mutex_);
        // Capture whether a longitude is already known BEFORE publishing this
        // latitude: setting site_coords_valid_ first made has_longitude
        // unconditionally true, so a latitude-only set wrote longitude 0.0 to
        // the mount (M10). Partial site data must never be sent.
        const bool has_longitude = pending_site_longitude_.has_value() || site_coords_valid_;
        const double longitude = pending_site_longitude_.has_value() ? pending_site_longitude_.value() : site_longitude_deg_;
        site_latitude_deg_ = latitude;
        pending_site_latitude_ = latitude;
        if (has_longitude) {
            site_coords_valid_ = true;
        }
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_coords_valid_) {
            throw AlpacaException("Site coordinates not available", AlpacaError::ValueNotSet);
        }
        return site_longitude_deg_;
    }

    void set_site_longitude(double longitude) override {
        validate_longitude(longitude);

        std::lock_guard<std::mutex> lock(mutex_);
        // Mirror of set_site_latitude (M10): capture whether a latitude is
        // already known BEFORE publishing this longitude, so a longitude-only
        // set never writes latitude 0.0 to the mount.
        const bool has_latitude = pending_site_latitude_.has_value() || site_coords_valid_;
        const double latitude = pending_site_latitude_.has_value() ? pending_site_latitude_.value() : site_latitude_deg_;
        site_longitude_deg_ = longitude;
        pending_site_longitude_ = longitude;
        if (has_latitude) {
            site_coords_valid_ = true;
        }
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
            // :Q stops all motion; individual :Qe/:Qw/:Qn/:Qs are redundant and add
            // round-trip latency that pushes Wi-Fi response times past the STANDARD target.
            protocol.abort_motion();

            std::optional<bool> restore_tracking;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                manual_axis_slewing_[axis] = false;
                restore_tracking = manual_axis_tracking_restore_[axis];
                manual_axis_tracking_restore_[axis] = std::nullopt;
                park_command_active_ = false;
                park_motion_seen_ = false;
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
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
        throw AlpacaException("Axis must be 0, 1, or 2", AlpacaError::InvalidValue);
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

        // Fast path: use cached status to meet the 0.1s FAST response target.
        // The poll thread refreshes this every 250ms.
        std::optional<StatusInfo> cached_status;
        bool park_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            park_active = park_command_active_;
            if (cached_status_.has_value() && (now - cached_status_at_) <= kFastStatusTtl) {
                cached_status = cached_status_.value();
            }
        }
        if (!park_active && cached_status.has_value()) {
            return !cached_status.value().stop_or_tracking;
        }

        // Status cache is stale (e.g. poll thread was paused during pulse guiding).
        // If no park command is active, use the stale cached value rather than
        // querying the mount, which would exceed the FAST response time over Wi-Fi.
        if (!park_active) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_status_.has_value()) {
                return !cached_status_.value().stop_or_tracking;
            }
        }

        auto& protocol = ZWOMountProtocolWrapper::instance();
        std::optional<ParkStatus> park_status;
        try {
            park_status = protocol.get_park_status();
        } catch (const std::exception&) {
        }

        std::optional<StatusInfo> status;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cached_status_.has_value()) {
                status = cached_status_.value();
            }
        }
        if (park_active || (park_status.has_value() && park_status.value() == ParkStatus::InProgress)) {
            try {
                if (!status.has_value()) {
                    status = protocol.get_status();
                }
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

        StatusInfo mount_status;
        if (status.has_value()) {
            mount_status = status.value();
        } else {
            mount_status = protocol.get_status();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_status_ = mount_status;
            cached_status_at_ = std::chrono::steady_clock::now();
        }
        return !mount_status.stop_or_tracking;
    }

    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_dec_set_) {
            throw AlpacaException("TargetDeclination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        validate_dec(dec, "TargetDeclination");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_dec_degrees_ = dec;
            target_dec_set_ = true;
        }

        if (connected_.load()) {
            ZWOMountProtocolWrapper::instance().set_target_dec(dec);
        }
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_) {
            throw AlpacaException("TargetRightAscension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        validate_ra(ra, "TargetRightAscension");

        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_ra_set_ = true;
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (last_utc_valid_) {
                const auto elapsed = std::chrono::steady_clock::now() - last_utc_set_monotonic_;
                return last_utc_set_ +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
            }
        }

        if (connected_.load()) {
            try {
                const TimeInfo mount_time = ZWOMountProtocolWrapper::instance().get_time_info();
                const auto utc = to_utc_time_point(mount_time);
                std::lock_guard<std::mutex> lock(mutex_);
                last_utc_set_ = utc;
                last_utc_set_monotonic_ = std::chrono::steady_clock::now();
                last_utc_valid_ = true;
                timezone_offset_minutes_ = mount_time.timezone_offset_minutes;
                timezone_valid_ = true;
                return utc;
            } catch (const std::exception&) {
            }
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
        reset_position_offsets();
        ZWOMountProtocolWrapper::instance().go_home();
        std::lock_guard<std::mutex> lock(mutex_);
        parked_cached_ = false;
        park_command_active_ = false;
        park_motion_seen_ = false;
        park_state_cached_.reset();
        park_state_at_ = std::chrono::steady_clock::time_point{};
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    void park() override {
        check_connected();
        reset_position_offsets();
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
        reset_position_offsets();
        ZWOMountProtocolWrapper::instance().abort_motion();
        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        manual_axis_tracking_restore_[0] = std::nullopt;
        manual_axis_tracking_restore_[1] = std::nullopt;
        park_command_active_ = false;
        park_motion_seen_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point{};
    }

    void pulse_guide(int direction, int duration) override {
        check_connected();
        ensure_not_parked("PulseGuide");
        if (!get_tracking()) {
            throw AlpacaException(
                "PulseGuide requires Tracking to be enabled",
                AlpacaError::InvalidOperation);
        }
        if (duration < 0) {
            throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
        }

        const bool ra_axis = (direction == 2 || direction == 3);
        const bool dec_axis = (direction == 0 || direction == 1);
        if (!ra_axis && !dec_axis) {
            throw AlpacaException("PulseGuide direction must be 0..3", AlpacaError::InvalidValue);
        }

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
            if (cached_equatorial_.has_value() &&
                (std::chrono::steady_clock::now() - cached_equatorial_at_) <= kFastEquatorialTtlForRead) {
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
            std::lock_guard<std::mutex> lock(pulse_mutex_);
            const auto now = std::chrono::steady_clock::now();
            const auto start_time = std::max(pulse_queue_end_, now);
            pulse_queue_end_ = start_time + std::chrono::milliseconds(effective_duration);
            {
                std::lock_guard<std::mutex> state_lock(mutex_);
                ra_offset_hours_ += expected_hours;
                dec_offset_deg_ += expected_dec;
                dec_offset_deg_ = std::clamp(dec_offset_deg_, -180.0, 180.0);
                pulse_guiding_end_ = pulse_queue_end_ + kPulseGuideHold;
            }
            pulse_queue_.push_back(task);
        }
        pulse_cv_.notify_one();
    }

    void set_park() override {
        check_connected();
        reset_position_offsets();
        if (!ZWOMountProtocolWrapper::instance().set_custom_park_here()) {
            throw AlpacaException("Mount rejected set park request", AlpacaError::InvalidOperation);
        }
    }

    void slew_to_coordinates(double ra, double dec) override {
        // slew_to_target() -> slew_to_target_async() handles ensure_not_parked and
        // reset_position_offsets; avoid redundant mount queries here.
        set_target_right_ascension(ra);
        set_target_declination(dec);
        slew_to_target();
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        // Validation only; slew_to_target_async() handles ensure_not_parked and
        // reset_position_offsets — calling them here would add redundant mount
        // round-trips that push Wi-Fi response times past the STANDARD 1.0s target.
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
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

        const auto deadline = std::chrono::steady_clock::now() + kDefaultSlewTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!get_slewing()) {
                const int settle = get_slew_settle_time();
                if (settle > 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(settle));
                }
                refresh_cached_values();
                EquatorialCoordinates raw;
                bool has_raw = false;
                const auto now = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (cached_equatorial_.has_value() && (now - cached_equatorial_at_) <= kFastEquatorialTtl) {
                        raw = cached_equatorial_.value();
                        has_raw = true;
                    }
                }
                if (!has_raw) {
                    try {
                        raw = ZWOMountProtocolWrapper::instance().get_current_equatorial();
                        has_raw = true;
                    } catch (const std::exception&) {
                    }
                }
                if (has_raw) {
                    const double target_ra = get_target_right_ascension();
                    const double target_dec = get_target_declination();
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
                    pending_slew_adjust_ = false;
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
        // Sibling of the move_axis M9 fix: ensure_not_parked can throw
        // (Parked) — never leave poll_pause_ stuck true.
        try {
            ensure_not_parked("SlewToTargetAsync");
            reset_position_offsets();
        } catch (...) {
            poll_pause_.store(false);
            throw;
        }

        double target_ra = 0.0;
        double target_dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!target_ra_set_ || !target_dec_set_) {
                poll_pause_.store(false);
                throw AlpacaException("Target coordinates not set", AlpacaError::ValueNotSet);
            }
            target_ra = target_ra_hours_;
            target_dec = target_dec_degrees_;
        }

        try {
            validate_ra(target_ra, "RightAscension");
            validate_dec(target_dec, "Declination");
        } catch (...) {
            poll_pause_.store(false);
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            manual_axis_tracking_restore_[0] = std::nullopt;
            manual_axis_tracking_restore_[1] = std::nullopt;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            parked_cached_ = false;
            park_command_active_ = false;
            park_motion_seen_ = false;
            cached_equatorial_.reset();
            cached_equatorial_at_ = std::chrono::steady_clock::time_point{};
            pending_slew_adjust_ = true;
            pending_slew_ra_hours_ = target_ra;
            pending_slew_dec_degrees_ = target_dec;
            pending_slew_at_ = std::chrono::steady_clock::now();
        }

        // Joinable member thread, NOT detached (H1): cancel + join any
        // previous GOTO setup first, then reset the cancel flag for this one.
        // The destructor and set_connected(false) cancel via
        // cancel_goto_thread_request(); the destructor joins.
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
        goto_thread_ = std::thread([this, target_ra, target_dec]() {
            auto resume_poll = [this]() { poll_pause_.store(false); };
            auto& protocol = ZWOMountProtocolWrapper::instance();
            try {
                if (goto_cancel_.load()) {
                    resume_poll();
                    return;
                }
                synchronize_mount_time_and_site_for_goto(false);
                protocol.set_target_ra(target_ra);
                protocol.set_target_dec(target_dec);
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
                            protocol.abort_motion();
                            // Cancellable backoff: a disconnect/destruction
                            // must not wait out the retry sleep.
                            std::unique_lock<std::mutex> cancel_lock(goto_mutex_);
                            goto_cv_.wait_for(cancel_lock, std::chrono::milliseconds(300),
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
                            synchronize_mount_time_and_site_for_goto(false);
                            continue;
                        }
                        if (attempt == 1 && is_ms_target_under_horizon_error(ex)) {
                            ALPACA_LOG_WARN(
                                "ZWO",
                                "GOTO still rejected with e5 after normal sync; retrying with inverted longitude sign");
                            synchronize_mount_time_and_site_for_goto(true);
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
                        resume_poll();
                        return;
                    }
                }
            } catch (const std::exception& ex) {
                ALPACA_LOG_WARN("ZWO", "GOTO failed: " + std::string(ex.what()));
            }
            resume_poll();
        });
    }

    void sync_to_coordinates(double ra, double dec) override {
        // sync_to_target() handles ensure_not_parked and reset_position_offsets.
        validate_ra(ra, "RightAscension");
        validate_dec(dec, "Declination");
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

        auto& protocol = ZWOMountProtocolWrapper::instance();

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
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                protocol.set_target_ra(target_ra);
                protocol.set_target_dec(target_dec);
                protocol.sync_target();
            }
        }
        EquatorialCoordinates raw;
        bool has_raw = false;
        const auto now = std::chrono::steady_clock::now();
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
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            cached_equatorial_.reset();
            cached_equatorial_at_ = std::chrono::steady_clock::time_point{};
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
        auto& protocol = ZWOMountProtocolWrapper::instance();
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
        if (!protocol.unpark()) {
            std::optional<ParkStatus> park_status;
            try {
                park_status = protocol.get_park_status();
            } catch (const std::exception&) {
            }

            if (!(park_status == ParkStatus::Unknown || park_status == ParkStatus::NotParked ||
                  !park_status.has_value())) {
                std::lock_guard<std::mutex> guard(mutex_);
                unpark_in_progress_ = false;
                throw AlpacaException("Mount rejected unpark request", AlpacaError::InvalidOperation);
            }

            ALPACA_LOG_WARN("ZWO",
                            "Unpark returned 0 while :Gps is unavailable/not-parked; treating unpark as successful");
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
        park_state_cached_.reset();
        park_state_at_ = std::chrono::steady_clock::time_point{};
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }

private:
    static constexpr auto kFastStatusTtl = std::chrono::seconds(2);
    // Longer TTL for equatorial cache so RA/Dec reads meet FAST target over high-latency (e.g. WiFi) links.
    static constexpr auto kFastEquatorialTtlForRead = std::chrono::seconds(5);
    static constexpr auto kFastPierSideTtl = std::chrono::seconds(5);
    static constexpr auto kFastTrackingTtl = std::chrono::seconds(2);
    static constexpr auto kFastEquatorialTtl = std::chrono::seconds(2);
    static constexpr auto kFastParkTtl = std::chrono::seconds(2);

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

    void reset_position_offsets() {
        // Signal cancellation first so the pulse thread's reconciliation read
        // (which holds mutex_) can exit early, avoiding lock contention.
        pulse_cancel_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ra_offset_hours_ = 0.0;
            dec_offset_deg_ = 0.0;
            pulse_generation_.fetch_add(1);
            pulse_guiding_end_ = std::chrono::steady_clock::now();
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
            const auto now = std::chrono::steady_clock::now();
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

    void refresh_cached_values(bool require_connected = true) const {
        if (require_connected && !connected_.load()) {
            return;
        }
        auto& protocol = ZWOMountProtocolWrapper::instance();
        const auto now = std::chrono::steady_clock::now();
        try {
            const StatusInfo status = protocol.get_status();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_status_ = status;
            cached_status_at_ = now;
            tracking_state_cached_ = !status.no_tracking;
            tracking_state_valid_ = true;
            tracking_state_at_ = now;
        } catch (const std::exception&) {
        }
        // Yield between protocol reads so that driver commands (MoveAxis,
        // SlewToCoordinatesAsync, etc.) can acquire the protocol mutex
        // promptly instead of waiting for the full refresh cycle to finish.
        if (poll_pause_.load()) return;
        try {
            const EquatorialCoordinates eq = protocol.get_current_equatorial();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_equatorial_ = eq;
            cached_equatorial_at_ = now;
        } catch (const std::exception&) {
        }
        if (poll_pause_.load()) return;
        try {
            const HorizontalCoordinates hor = protocol.get_current_horizontal();
            std::lock_guard<std::mutex> lock(mutex_);
            cached_horizontal_ = hor;
            cached_horizontal_at_ = now;
        } catch (const std::exception&) {
        }
        // Warm the park state so get_at_park() never falls through to a live
        // :Gps read in steady state (keeps DeviceState under the ASCOM FAST
        // 0.1 s target). Only the POSITIVE state is pinned: when the mount is
        // parked we cache true; when it is genuinely not parked and no park
        // command is active we cache false. During an ACTIVE park command we
        // deliberately leave the cache alone so get_at_park() runs its full
        // evaluation — including the "stationary ⇒ park complete" inference
        // that some ZWO firmware relies on when :hP does not physically move
        // the mount (AM5N). Once the driver has inferred parked (parked_cached_
        // true), never overwrite it back to false from a raw :Gps=0 — that
        // would make AtPark flap and break ConformU's Parked: tests (slew/
        // sync while parked must throw, not silently unpark).
        if (poll_pause_.load()) return;
        try {
            const auto park_status = protocol.get_park_status();
            std::lock_guard<std::mutex> lock(mutex_);
            if (park_status == ParkStatus::InProgress) {
                // Keep both caches untouched while the mount is moving: an
                // in-flight park may have already inferred "stationary ⇒
                // parked" (parked_cached_ true), and clearing it here would
                // defeat that inference on firmware that keeps reporting
                // InProgress after the mount stops (AM3/AM5/AM7).
            } else if (park_status == ParkStatus::Completed && !unpark_in_progress_) {
                park_state_cached_ = true;
                parked_cached_ = true;
                park_state_at_ = now;
            } else if (!park_command_active_ && !parked_cached_) {
                // NotParked / Error / Unknown, no active park, and the driver
                // does not believe the mount is parked: safe to cache false.
                park_state_cached_ = false;
                park_state_at_ = now;
            }
            // Active park, or the driver has inferred parked: leave the cache
            // alone so get_at_park() re-evaluates / the inferred state sticks.
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // :Gps read failed during park warm — leave the cache stale so
            // get_at_park() re-evaluates; the next poll cycle retries.
        }
        // Compute pier side from current hour angle (same logic as get_side_of_pier).
        // :Gm# is not used because ZWO firmware's E/W response does not reliably map to the
        // ASCOM pierEast/pierWest convention.
        {
            double longitude = 0.0;
            double ra_hours = 0.0;
            double ra_offset = 0.0;
            bool has_all = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (site_coords_valid_ && cached_equatorial_.has_value()) {
                    longitude = site_longitude_deg_;
                    ra_hours = cached_equatorial_.value().ra_hours;
                    ra_offset = ra_offset_hours_;
                    has_all = true;
                }
            }
            if (has_all) {
                double current_ra = std::fmod(ra_hours + ra_offset, 24.0);
                if (current_ra < 0.0) current_ra += 24.0;
                const double lst = compute_local_sidereal_time_hours(
                    std::chrono::system_clock::now(), longitude);
                const double ha = normalize_hour_angle_hours(lst - current_ra);
                const int side = ha >= 0.0 ? 0 : 1;
                std::lock_guard<std::mutex> lock(mutex_);
                cached_pier_side_ = side;
                cached_pier_side_at_ = now;
            }
        }
    }

    void apply_pending_slew_adjustment() const {
        double target_ra = 0.0;
        double target_dec = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_slew_adjust_) {
                return;
            }
            pending_slew_adjust_ = false;
            target_ra = pending_slew_ra_hours_;
            target_dec = pending_slew_dec_degrees_;
        }
        if (get_slewing()) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_slew_adjust_ = true;
            return;
        }

        EquatorialCoordinates raw;
        bool has_raw = false;
        try {
            raw = ZWOMountProtocolWrapper::instance().get_current_equatorial();
            has_raw = true;
        } catch (const std::exception&) {
        }
        if (!has_raw) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_slew_adjust_ = true;
            return;
        }

        const double ra_delta = normalize_hour_angle_hours(target_ra - raw.ra_hours);
        const double dec_delta = target_dec - raw.dec_degrees;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        ra_offset_hours_ = std::fmod(ra_offset_hours_ + ra_delta, 24.0);
        if (ra_offset_hours_ < 0.0) {
            ra_offset_hours_ += 24.0;
        }
        dec_offset_deg_ = std::clamp(dec_offset_deg_ + dec_delta, -90.0, 90.0);
        cached_equatorial_ = raw;
        cached_equatorial_at_ = now;
    }

    void start_poll_thread() {
        stop_poll_thread();
        poll_stop_.store(false);
        poll_thread_ = std::thread([this]() {
            using namespace std::chrono_literals;
            auto next_time_refresh = std::chrono::steady_clock::now();
            while (!poll_stop_.load()) {
                // pulse_guiding_end_ is mutex_-guarded everywhere else; an
                // unlocked read here is torn against a concurrent pulse (M12).
                std::chrono::steady_clock::time_point pulse_end;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pulse_end = pulse_guiding_end_;
                }
                if (poll_pause_.load() || std::chrono::steady_clock::now() < pulse_end) {
                    std::this_thread::sleep_for(50ms);
                    continue;
                }
                if (!connected_.load()) {
                    std::this_thread::sleep_for(200ms);
                    continue;
                }
                refresh_cached_values();
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_time_refresh) {
                    try {
                        const TimeInfo mount_time = ZWOMountProtocolWrapper::instance().get_time_info();
                        const auto utc = to_utc_time_point(mount_time);
                        std::lock_guard<std::mutex> lock(mutex_);
                        last_utc_set_ = utc;
                        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
                        last_utc_valid_ = true;
                        timezone_offset_minutes_ = mount_time.timezone_offset_minutes;
                        timezone_valid_ = true;
                    } catch (const std::exception&) {
                    }
                    next_time_refresh = now + std::chrono::seconds(30);
                }
                std::this_thread::sleep_for(250ms);
            }
        });
    }

    void stop_poll_thread() {
        poll_stop_.store(true);
        if (poll_thread_.joinable()) {
            poll_thread_.join();
        }
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
        auto& protocol = ZWOMountProtocolWrapper::instance();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
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
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                wait_ms -= sleep_ms;
            }

            remaining_ms -= step;
        }

        // Rebase the predictive offset against the mount's actual position.
        // The mount may or may not update its reported coordinates during pulse
        // guiding; reconciling here prevents double-counting (offset + actual
        // movement) without losing the prediction when coordinates don't update.
        if (!pulse_cancel_.load() && task.generation == pulse_generation_.load()) {
            try {
                const EquatorialCoordinates eq = protocol.get_current_equatorial();
                std::lock_guard<std::mutex> lock(mutex_);
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
                cached_equatorial_at_ = std::chrono::steady_clock::now();
            } catch (const std::exception&) {
                // Read failed; refresh the cache timestamp to prevent TTL expiration
                // from triggering a double-count on the next position read.
                std::lock_guard<std::mutex> lock(mutex_);
                if (cached_equatorial_.has_value()) {
                    cached_equatorial_at_ = std::chrono::steady_clock::now();
                }
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
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

    // Ask a running GOTO setup thread to exit promptly (it re-checks the
    // flag between protocol calls and its retry backoff waits on goto_cv_).
    // The store happens under goto_mutex_ so a waiter cannot miss the wakeup.
    void cancel_goto_thread_request() {
        {
            std::lock_guard<std::mutex> lock(goto_mutex_);
            goto_cancel_.store(true);
        }
        goto_cv_.notify_all();
    }

    const int device_number_;
    const ConnectionInfo connection_info_;

    std::atomic<bool> connected_;
    // True once the telemetry caches are warm after connect. get_connected()
    // gates on this so clients only see Connected=true when the first property
    // reads will hit warm caches (ASCOM FAST 0.1 s target).
    std::atomic<bool> caches_ready_;

    mutable std::mutex mutex_;

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
    double site_elevation_m_;

    bool does_refraction_;
    int slew_settle_time_s_;

    mutable GuideRate guide_rate_;
    mutable int tracking_rate_cached_;
    mutable bool tracking_rate_valid_;
    mutable bool tracking_state_cached_;
    mutable bool tracking_state_valid_;
    mutable std::chrono::steady_clock::time_point tracking_state_at_;
    mutable std::optional<EquatorialCoordinates> cached_equatorial_;
    mutable std::optional<HorizontalCoordinates> cached_horizontal_;
    mutable std::optional<StatusInfo> cached_status_;
    mutable std::optional<int> cached_pier_side_;
    mutable std::optional<bool> park_state_cached_;
    mutable std::chrono::steady_clock::time_point cached_equatorial_at_;
    mutable std::chrono::steady_clock::time_point cached_horizontal_at_;
    mutable std::chrono::steady_clock::time_point cached_status_at_;
    mutable std::chrono::steady_clock::time_point cached_pier_side_at_;
    mutable std::chrono::steady_clock::time_point park_state_at_;
    mutable bool pending_slew_adjust_;
    mutable double pending_slew_ra_hours_;
    mutable double pending_slew_dec_degrees_;
    mutable std::chrono::steady_clock::time_point pending_slew_at_;
    std::thread poll_thread_;
    std::atomic<bool> poll_stop_;
    std::atomic<bool> poll_pause_;

    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;

    mutable int timezone_offset_minutes_;
    mutable bool timezone_valid_;

    std::array<bool, 2> manual_axis_slewing_;
    std::array<std::optional<bool>, 2> manual_axis_tracking_restore_;
    std::chrono::steady_clock::time_point slew_force_until_;
    std::chrono::steady_clock::time_point pulse_guiding_end_;
    std::atomic<uint64_t> pulse_generation_;
    mutable std::mutex pulse_mutex_;
    std::condition_variable pulse_cv_;
    std::deque<PulseTask> pulse_queue_;
    std::thread pulse_thread_;
    std::atomic<bool> pulse_thread_stop_;
    std::atomic<bool> pulse_cancel_;
    // Async teardown spawned by set_connected(false) — a joinable member so
    // the destructor can join it (a detached thread touching `this` is a
    // use-after-free if the driver dies mid-teardown; AGENTS.md rule).
    std::thread disconnect_thread_;
    // Serializes set_connected() bodies (sync PUT / async task / refresh) so
    // poll_thread_/pulse_thread_/disconnect_thread_ join+assign and their
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
    std::atomic<bool> goto_cancel_{false};
    std::mutex goto_mutex_;
    std::condition_variable goto_cv_;
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

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
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>
#include <alpacacore/version.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace alpacacore::vendor::weewx {

namespace {

constexpr double kFahrenheitToCelsiusScale = 5.0 / 9.0;
constexpr double kFahrenheitOffset = 32.0;
constexpr double kMphToMetersPerSecond = 0.44704;
constexpr double kInHgToHpa = 33.8638866667;

constexpr const char* kDriverComponent = "WeeWX";

struct SensorSnapshot {
    double temperature_c{std::numeric_limits<double>::quiet_NaN()};
    double humidity{std::numeric_limits<double>::quiet_NaN()};
    double dewpoint_c{std::numeric_limits<double>::quiet_NaN()};
    double wind_speed_ms{std::numeric_limits<double>::quiet_NaN()};
    double pressure_hpa{std::numeric_limits<double>::quiet_NaN()};
    double sky_quality{std::numeric_limits<double>::quiet_NaN()};
    double sky_temperature_c{std::numeric_limits<double>::quiet_NaN()};
    std::chrono::system_clock::time_point timestamp{};
};

std::string normalize_property_name(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (char ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return normalized;
}

std::optional<std::string_view> extract_object_block(std::string_view payload, std::string_view key) {
    const std::string target = "\"" + std::string(key) + "\"";
    std::size_t key_pos = payload.find(target);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t brace_pos = payload.find('{', key_pos + target.size());
    if (brace_pos == std::string_view::npos) {
        return std::nullopt;
    }
    int depth = 0;
    for (std::size_t i = brace_pos; i < payload.size(); ++i) {
        if (payload[i] == '{') {
            ++depth;
        } else if (payload[i] == '}') {
            --depth;
            if (depth == 0) {
                return payload.substr(brace_pos + 1, i - brace_pos - 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<double> parse_numeric_value(std::string_view text, std::size_t start) {
    if (start >= text.size()) {
        return std::nullopt;
    }
    std::size_t i = start;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i + 4 <= text.size() && text.substr(i, 4) == "null") {
        return std::nullopt;
    }
    std::string temp(text.substr(i));
    char* end = nullptr;
    const double value = std::strtod(temp.c_str(), &end);
    if (end == temp.c_str()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parse_current_value(std::string_view current_block, std::string_view key) {
    const std::string target = "\"" + std::string(key) + "\"";
    std::size_t key_pos = current_block.find(target);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t value_pos = current_block.find("\"value\"", key_pos + target.size());
    if (value_pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t colon_pos = current_block.find(':', value_pos + 7);
    if (colon_pos == std::string_view::npos) {
        return std::nullopt;
    }
    return parse_numeric_value(current_block, colon_pos + 1);
}

// Parse a Unix-epoch timestamp field (either a bare number after the key, or a
// nested {"value": n} object like the sensor fields). Values that look like
// milliseconds are converted to seconds.
std::optional<double> parse_epoch_seconds(std::string_view block, std::string_view key) {
    std::optional<double> value = parse_current_value(block, key);
    if (!value.has_value()) {
        const std::string target = "\"" + std::string(key) + "\"";
        std::size_t key_pos = block.find(target);
        if (key_pos == std::string_view::npos) {
            return std::nullopt;
        }
        std::size_t colon_pos = block.find(':', key_pos + target.size());
        if (colon_pos == std::string_view::npos) {
            return std::nullopt;
        }
        value = parse_numeric_value(block, colon_pos + 1);
    }
    if (!value.has_value() || value.value() <= 0.0) {
        return std::nullopt;
    }
    double epoch = value.value();
    if (epoch > 1e12) {
        epoch /= 1000.0;  // milliseconds → seconds
    }
    return epoch;
}

WeeWxCurrentValues to_values(const SensorSnapshot& snapshot) {
    return WeeWxCurrentValues{
        snapshot.temperature_c,
        snapshot.humidity,
        snapshot.dewpoint_c,
        snapshot.wind_speed_ms,
        snapshot.pressure_hpa,
        snapshot.sky_quality,
        snapshot.sky_temperature_c
    };
}

SensorSnapshot parse_snapshot(std::string_view payload) {
    SensorSnapshot snapshot;
    const auto lcd_block = extract_object_block(payload, "lcd_datasheet");
    if (!lcd_block) {
        throw AlpacaException("WeeWX payload missing lcd_datasheet", AlpacaError::DriverException);
    }
    const auto current_block = extract_object_block(*lcd_block, "current");
    if (!current_block) {
        throw AlpacaException("WeeWX payload missing current data", AlpacaError::DriverException);
    }

    auto out_temp_f = parse_current_value(*current_block, "outTemp");
    if (out_temp_f.has_value()) {
        snapshot.temperature_c = (out_temp_f.value() - kFahrenheitOffset) * kFahrenheitToCelsiusScale;
    }

    auto humidity = parse_current_value(*current_block, "outHumidity");
    if (humidity.has_value()) {
        snapshot.humidity = humidity.value();
    }

    auto dewpoint_f = parse_current_value(*current_block, "dewpoint");
    if (dewpoint_f.has_value()) {
        snapshot.dewpoint_c = (dewpoint_f.value() - kFahrenheitOffset) * kFahrenheitToCelsiusScale;
    }

    auto wind_speed_mph = parse_current_value(*current_block, "wind_speed");
    if (wind_speed_mph.has_value()) {
        snapshot.wind_speed_ms = wind_speed_mph.value() * kMphToMetersPerSecond;
    }

    auto pressure_inhg = parse_current_value(*current_block, "barometer");
    if (pressure_inhg.has_value()) {
        snapshot.pressure_hpa = pressure_inhg.value() * kInHgToHpa;
    }

    auto sky_quality = parse_current_value(*current_block, "sqm");
    if (sky_quality.has_value()) {
        snapshot.sky_quality = sky_quality.value();
    }

    auto sky_temp_f = parse_current_value(*current_block, "sqmTemp");
    if (sky_temp_f.has_value()) {
        snapshot.sky_temperature_c = (sky_temp_f.value() - kFahrenheitOffset) * kFahrenheitToCelsiusScale;
    }

    // Stamp with the OBSERVATION time reported by WeeWX, not the fetch time:
    // stamping at fetch would make a dead station (serving a stale payload)
    // report TimeSinceLastUpdate ≈ 0 forever. Fall back to fetch time only if
    // the payload carries no usable epoch.
    std::optional<double> epoch = parse_epoch_seconds(*current_block, "dateTime");
    if (!epoch.has_value()) {
        epoch = parse_epoch_seconds(*lcd_block, "dateTime");
    }
    if (!epoch.has_value()) {
        epoch = parse_epoch_seconds(payload, "generation_time");
    }
    if (epoch.has_value()) {
        snapshot.timestamp =
            std::chrono::system_clock::time_point(std::chrono::seconds(static_cast<long long>(epoch.value())));
    } else {
        snapshot.timestamp = std::chrono::system_clock::now();
    }
    return snapshot;
}

size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (!userdata) {
        return 0;
    }
    auto* buffer = static_cast<std::string*>(userdata);
    buffer->append(ptr, size * nmemb);
    return size * nmemb;
}

void ensure_curl_global_init() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string http_get(const std::string& url, std::chrono::milliseconds timeout) {
    ensure_curl_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw AlpacaException("Unable to initialize HTTP client", AlpacaError::DriverException);
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AlpacaBridge-WeeWX/1.0");

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        std::string message = "HTTP request failed: ";
        message += curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        throw AlpacaException(message, AlpacaError::DriverException);
    }

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    curl_easy_cleanup(curl);

    if (status_code >= 400) {
        throw AlpacaException("HTTP request returned status " + std::to_string(status_code), AlpacaError::DriverException);
    }

    return response;
}

class WeeWxObservingConditionsDriver final : public ObservingConditionsDriver, protected alpacacore::AsyncConnectable {
public:
    WeeWxObservingConditionsDriver(int device_number, WeeWxHttpConfig config)
        : AsyncConnectable(kDriverComponent),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          average_period_hours_(0.0),
          poll_running_(false) {
        if (config_.url.empty()) {
            throw AlpacaException("WeeWX URL is required", AlpacaError::InvalidValue);
        }
    }

    ~WeeWxObservingConditionsDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        stop_polling();
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "WeeWX ObservingConditions";
    }

    DeviceType get_device_type() const override {
        return DeviceType::ObservingConditions;
    }

    std::string get_unique_id() const override {
        return "WEEWX_OC_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "WeeWX ObservingConditions from HTTP JSON";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore WeeWX ObservingConditions Driver";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 2; }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Base gates first: a sync disconnect during an in-flight connect
        // looks idempotent (both sides see disconnected) and would be silently
        // dropped without the record; a connect must honor a newer pending
        // disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected) {
            SensorSnapshot snapshot = fetch_snapshot();
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                last_snapshot_ = snapshot;
                rebuild_supported_properties_locked(snapshot);
                update_property_timestamp_locked("temperature", snapshot.temperature_c);
                update_property_timestamp_locked("humidity", snapshot.humidity);
                update_property_timestamp_locked("dewpoint", snapshot.dewpoint_c);
                update_property_timestamp_locked("windspeed", snapshot.wind_speed_ms);
                update_property_timestamp_locked("pressure", snapshot.pressure_hpa);
                update_property_timestamp_locked("skyquality", snapshot.sky_quality);
                update_property_timestamp_locked("skytemperature", snapshot.sky_temperature_c);
            }
            connected_.store(true);
            start_polling();
        } else {
            stop_polling();
            connected_.store(false);
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view, std::string_view) override {
        throw AlpacaException("Action not supported", AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::NotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::NotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::NotImplemented);
    }

    double get_average_period() const override {
        std::lock_guard<std::mutex> lock(data_mutex_);
        return average_period_hours_;
    }

    void set_average_period(double period) override {
        if (period < 0.0) {
            throw AlpacaException("AveragePeriod must be non-negative", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(data_mutex_);
        average_period_hours_ = period;
    }

    double get_cloud_cover() const override {
        throw AlpacaException("CloudCover not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_dew_point() const override {
        return get_value_or_nan(&SensorSnapshot::dewpoint_c, "dewpoint");
    }

    double get_humidity() const override {
        return get_value_or_nan(&SensorSnapshot::humidity, "humidity");
    }

    double get_pressure() const override {
        return get_value_or_nan(&SensorSnapshot::pressure_hpa, "pressure");
    }

    double get_rain_rate() const override {
        throw AlpacaException("RainRate not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_sky_brightness() const override {
        throw AlpacaException("SkyBrightness not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_sky_quality() const override {
        return get_value_or_nan(&SensorSnapshot::sky_quality, "skyquality");
    }

    double get_sky_temperature() const override {
        return get_value_or_nan(&SensorSnapshot::sky_temperature_c, "skytemperature");
    }

    double get_seeing() const override {
        throw AlpacaException("Seeing not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_star_fwhm() const override {
        throw AlpacaException("StarFWHM not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_temperature() const override {
        return get_value_or_nan(&SensorSnapshot::temperature_c, "temperature");
    }

    double get_wind_direction() const override {
        throw AlpacaException("WindDirection not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_wind_gust() const override {
        throw AlpacaException("WindGust not implemented", AlpacaError::PropertyNotImplemented);
    }

    double get_wind_speed() const override {
        return get_value_or_nan(&SensorSnapshot::wind_speed_ms, "windspeed");
    }

    double get_time_since_last_update(std::string_view property_name) const override {
        const std::string key = normalize_property_name(property_name);
        if (key.empty()) {
            return -1.0;
        }
        // supported_properties_ is mutated by the poll thread under data_mutex_;
        // every read must hold the same lock.
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (!supported_properties_.count(key)) {
            throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        }
        auto it = property_last_update_.find(key);
        if (it == property_last_update_.end()) {
            return -1.0;
        }
        auto now = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed = now - it->second;
        return elapsed.count();
    }

    std::string get_sensor_description(std::string_view property_name) const override {
        const std::string key = normalize_property_name(property_name);
        // supported_properties_ is mutated by the poll thread under data_mutex_;
        // every read must hold the same lock.
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (!supported_properties_.count(key)) {
            throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        }
        const auto it = property_descriptions_.find(key);
        if (it == property_descriptions_.end()) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        return it->second;
    }

    void refresh() override {
        if (!get_connected()) {
            throw AlpacaException("Device not connected", AlpacaError::NotConnected);
        }
        SensorSnapshot snapshot = fetch_snapshot();
        std::lock_guard<std::mutex> lock(data_mutex_);
        last_snapshot_ = snapshot;
        rebuild_supported_properties_locked(snapshot);
        update_property_timestamp_locked("temperature", snapshot.temperature_c);
        update_property_timestamp_locked("humidity", snapshot.humidity);
        update_property_timestamp_locked("dewpoint", snapshot.dewpoint_c);
        update_property_timestamp_locked("windspeed", snapshot.wind_speed_ms);
        update_property_timestamp_locked("pressure", snapshot.pressure_hpa);
        update_property_timestamp_locked("skyquality", snapshot.sky_quality);
        update_property_timestamp_locked("skytemperature", snapshot.sky_temperature_c);
    }

private:
    double get_value_or_nan(double SensorSnapshot::*member, std::string_view property) const {
        if (!get_connected()) {
            throw AlpacaException("Device not connected", AlpacaError::NotConnected);
        }
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (!supported_properties_.count(normalize_property_name(property))) {
            throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        }
        const double value = last_snapshot_.*member;
        if (!std::isfinite(value)) {
            throw AlpacaException("Value not set", AlpacaError::ValueNotSet);
        }
        return value;
    }

    void rebuild_supported_properties_locked(const SensorSnapshot& snapshot) {
        // Only add sensors that have data — never remove, so sensors that gain
        // data at runtime (e.g. SQM after sunset) become permanently supported.
        if (std::isfinite(snapshot.temperature_c))     supported_properties_.insert("temperature");
        if (std::isfinite(snapshot.humidity))          supported_properties_.insert("humidity");
        if (std::isfinite(snapshot.dewpoint_c))        supported_properties_.insert("dewpoint");
        if (std::isfinite(snapshot.wind_speed_ms))     supported_properties_.insert("windspeed");
        if (std::isfinite(snapshot.pressure_hpa))      supported_properties_.insert("pressure");
        if (std::isfinite(snapshot.sky_quality))       supported_properties_.insert("skyquality");
        if (std::isfinite(snapshot.sky_temperature_c)) supported_properties_.insert("skytemperature");
    }

    void update_property_timestamp_locked(std::string_view property, double value) {
        if (!std::isfinite(value)) {
            return;
        }
        property_last_update_[normalize_property_name(property)] = last_snapshot_.timestamp;
    }

    SensorSnapshot fetch_snapshot() const {
        std::string payload = http_get(config_.url, config_.timeout);
        return parse_snapshot(payload);
    }

    void start_polling() {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        if (poll_running_.load()) {
            return;
        }
        poll_running_.store(true);
        poll_thread_ = std::thread([this]() { poll_loop(); });
    }

    void stop_polling() {
        {
            std::lock_guard<std::mutex> lock(poll_mutex_);
            if (!poll_running_.load()) {
                return;
            }
            poll_running_.store(false);
        }
        poll_cv_.notify_all();
        if (poll_thread_.joinable()) {
            poll_thread_.join();
        }
    }

    void poll_loop() {
        while (poll_running_.load()) {
            try {
                SensorSnapshot snapshot = fetch_snapshot();
                std::lock_guard<std::mutex> lock(data_mutex_);
                last_snapshot_ = snapshot;
                rebuild_supported_properties_locked(snapshot);
                update_property_timestamp_locked("temperature", snapshot.temperature_c);
                update_property_timestamp_locked("humidity", snapshot.humidity);
                update_property_timestamp_locked("dewpoint", snapshot.dewpoint_c);
                update_property_timestamp_locked("windspeed", snapshot.wind_speed_ms);
                update_property_timestamp_locked("pressure", snapshot.pressure_hpa);
                update_property_timestamp_locked("skyquality", snapshot.sky_quality);
                update_property_timestamp_locked("skytemperature", snapshot.sky_temperature_c);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kDriverComponent, "Polling failed: " + std::string(e.what()));
            }

            std::unique_lock<std::mutex> lock(poll_mutex_);
            poll_cv_.wait_for(lock, config_.poll_interval, [this]() { return !poll_running_.load(); });
        }
    }

    int device_number_;
    WeeWxHttpConfig config_;

    std::atomic<bool> connected_;

    mutable std::mutex data_mutex_;
    SensorSnapshot last_snapshot_{};
    std::unordered_map<std::string, std::chrono::system_clock::time_point> property_last_update_;
    std::unordered_set<std::string> supported_properties_;
    const std::unordered_map<std::string, std::string> property_descriptions_ = {
        {"temperature", "Outdoor temperature (WeeWX outTemp)"},
        {"humidity", "Outdoor humidity (WeeWX outHumidity)"},
        {"dewpoint", "Dew point (WeeWX dewpoint)"},
        {"windspeed", "Wind speed (WeeWX wind_speed)"},
        {"pressure", "Barometric pressure (WeeWX barometer)"},
        {"skyquality", "Sky quality (WeeWX sqm)"},
        {"skytemperature", "Sky sensor temperature (WeeWX sqmTemp)"}
    };
    double average_period_hours_;

    std::mutex poll_mutex_;
    std::condition_variable poll_cv_;
    std::atomic<bool> poll_running_;
    std::thread poll_thread_;
};

} // namespace

std::optional<WeeWxCurrentValues> parse_weewx_current(std::string_view payload) {
    try {
        SensorSnapshot snapshot = parse_snapshot(payload);
        return to_values(snapshot);
    } catch (const AlpacaException&) {
        return std::nullopt;
    }
}

std::unique_ptr<ObservingConditionsDriver> create_weewx_observingconditions(
    int device_number,
    const WeeWxHttpConfig& config) {
    return std::make_unique<WeeWxObservingConditionsDriver>(device_number, config);
}

} // namespace alpacacore::vendor::weewx

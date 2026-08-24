// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <optional>

namespace alpacahttp {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class Config {
public:
    Config() = default;
    ~Config() = default;

    // Load configuration from file
    bool load(const std::string& config_path);
    bool load_default();

    // Getters
    std::uint16_t http_port() const { return http_port_; }
    bool discovery_enabled() const { return discovery_enabled_; }
    LogLevel log_level() const { return log_level_; }
    const std::string& server_name() const { return server_name_; }
    const std::string& manufacturer() const { return manufacturer_; }
    const std::string& location() const { return location_; }
    const std::string& profile_name() const { return profile_name_; }
    std::size_t thread_pool_size() const { return thread_pool_size_; }
    const std::string& log_directory() const { return log_directory_; }
    bool file_logging_enabled() const { return file_logging_enabled_; }
    int log_retention_days() const { return log_retention_days_; }
    const std::string& config_path() const { return config_path_; }

    // Device enable/disable
    bool is_device_enabled(const std::string& device_type, std::uint32_t device_number) const;

    // Set values (for testing/overrides)
    void set_http_port(std::uint16_t port) { http_port_ = port; }
    void set_discovery_enabled(bool enabled) { discovery_enabled_ = enabled; }
    void set_log_level(LogLevel level) { log_level_ = level; }
    void set_server_name(const std::string& name) { server_name_ = name; }
    void set_manufacturer(const std::string& mfg) { manufacturer_ = mfg; }
    void set_location(const std::string& loc) { location_ = loc; }
    void set_profile_name(const std::string& name) { profile_name_ = name; }
    void set_thread_pool_size(std::size_t size) {
        if (size < 1) size = 1;
        if (size > 256) size = 256;
        thread_pool_size_ = size;
    }
    void set_log_directory(const std::string& dir) { log_directory_ = dir; }
    void set_file_logging_enabled(bool enabled) { file_logging_enabled_ = enabled; }
    void set_log_retention_days(int days) { log_retention_days_ = days; }

private:
    std::uint16_t http_port_ = 6800;
    bool discovery_enabled_ = true;
    LogLevel log_level_ = LogLevel::WARNING;
    static constexpr const char* kDefaultServerName = "AlpacaHTTP";
    std::string server_name_ = kDefaultServerName;
    std::string manufacturer_ = "OpenAstro.net";
    std::string location_ = "";
    std::string profile_name_ = "";
    std::size_t thread_pool_size_ = 32;  // Default: 32 concurrent requests (supports multiple devices + clients)
    std::string log_directory_ = "/var/log/AlpacaBridge";
    bool file_logging_enabled_ = true;
    int log_retention_days_ = 90;  // 0 = forever
    std::string config_path_;

    // Device enable/disable map: "devicetype:number" -> enabled
    std::unordered_map<std::string, bool> device_enable_map_;

    LogLevel parse_log_level(const std::string& level_str);
    void apply_environment_overrides();
    void load_config_from_yaml(const std::string& config_path);
};

} // namespace alpacahttp

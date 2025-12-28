// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

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
    std::size_t thread_pool_size() const { return thread_pool_size_; }

    // Device enable/disable
    bool is_device_enabled(const std::string& device_type, std::uint32_t device_number) const;

    // Set values (for testing/overrides)
    void set_http_port(std::uint16_t port) { http_port_ = port; }
    void set_discovery_enabled(bool enabled) { discovery_enabled_ = enabled; }
    void set_log_level(LogLevel level) { log_level_ = level; }
    void set_server_name(const std::string& name) { server_name_ = name; }
    void set_manufacturer(const std::string& mfg) { manufacturer_ = mfg; }
    void set_location(const std::string& loc) { location_ = loc; }
    void set_thread_pool_size(std::size_t size) { thread_pool_size_ = size; }

private:
    std::uint16_t http_port_ = 6800;
    bool discovery_enabled_ = true;
    LogLevel log_level_ = LogLevel::WARNING;
    std::string server_name_ = "AlpacaHTTP";
    std::string manufacturer_ = "AlpacaHTTP";
    std::string location_ = "";
    std::size_t thread_pool_size_ = 32;  // Default: 32 concurrent requests (supports multiple devices + clients)

    // Device enable/disable map: "devicetype:number" -> enabled
    std::unordered_map<std::string, bool> device_enable_map_;

    LogLevel parse_log_level(const std::string& level_str);
    void apply_environment_overrides();
};

} // namespace alpacahttp

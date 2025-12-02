// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

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

    // Device enable/disable
    bool is_device_enabled(const std::string& device_type, std::uint32_t device_number) const;

    // Set values (for testing/overrides)
    void set_http_port(std::uint16_t port) { http_port_ = port; }
    void set_discovery_enabled(bool enabled) { discovery_enabled_ = enabled; }
    void set_log_level(LogLevel level) { log_level_ = level; }
    void set_server_name(const std::string& name) { server_name_ = name; }
    void set_manufacturer(const std::string& mfg) { manufacturer_ = mfg; }
    void set_location(const std::string& loc) { location_ = loc; }

private:
    std::uint16_t http_port_ = 6800;
    bool discovery_enabled_ = true;
    LogLevel log_level_ = LogLevel::INFO;
    std::string server_name_ = "AlpacaHTTP";
    std::string manufacturer_ = "AlpacaHTTP";
    std::string location_ = "";

    // Device enable/disable map: "devicetype:number" -> enabled
    std::unordered_map<std::string, bool> device_enable_map_;

    LogLevel parse_log_level(const std::string& level_str);
    void apply_environment_overrides();
};

} // namespace alpacahttp


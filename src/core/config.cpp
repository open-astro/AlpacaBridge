// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#include <alpacahttp/config.h>
#include <fstream>
#include <cstdlib>
#include <algorithm>

namespace alpacahttp {

bool Config::load(const std::string& config_path) {
    // TODO: Implement YAML parsing (using yaml-cpp if available)
    // For now, use default values and environment overrides
    apply_environment_overrides();
    return true;
}

bool Config::load_default() {
    // Try to load from default location
    return load("config/default.yaml");
}

LogLevel Config::parse_log_level(const std::string& level_str) {
    std::string lower = level_str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "debug") return LogLevel::DEBUG;
    if (lower == "info") return LogLevel::INFO;
    if (lower == "warning") return LogLevel::WARNING;
    if (lower == "error") return LogLevel::ERROR;

    return LogLevel::INFO;
}

bool Config::is_device_enabled(const std::string& device_type, std::uint32_t device_number) const {
    std::string key = device_type + ":" + std::to_string(device_number);
    auto it = device_enable_map_.find(key);
    if (it != device_enable_map_.end()) {
        return it->second;
    }
    // Default to enabled if not specified
    return true;
}

void Config::apply_environment_overrides() {
    const char* port_env = std::getenv("ALPACAHTTP_PORT");
    if (port_env) {
        try {
            http_port_ = static_cast<std::uint16_t>(std::stoul(port_env));
        } catch (...) {
            // Invalid port, use default
        }
    }

    const char* discovery_env = std::getenv("ALPACAHTTP_DISCOVERY");
    if (discovery_env) {
        std::string discovery_str = discovery_env;
        std::transform(discovery_str.begin(), discovery_str.end(), discovery_str.begin(), ::tolower);
        discovery_enabled_ = (discovery_str == "true" || discovery_str == "1");
    }

    const char* log_level_env = std::getenv("ALPACAHTTP_LOG_LEVEL");
    if (log_level_env) {
        log_level_ = parse_log_level(log_level_env);
    }

    const char* server_name_env = std::getenv("ALPACAHTTP_SERVER_NAME");
    if (server_name_env) {
        server_name_ = server_name_env;
    }

    const char* manufacturer_env = std::getenv("ALPACAHTTP_MANUFACTURER");
    if (manufacturer_env) {
        manufacturer_ = manufacturer_env;
    }

    const char* location_env = std::getenv("ALPACAHTTP_LOCATION");
    if (location_env) {
        location_ = location_env;
    }
}

} // namespace alpacahttp


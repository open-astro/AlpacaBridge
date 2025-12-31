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

    const char* thread_pool_env = std::getenv("ALPACAHTTP_THREAD_POOL_SIZE");
    if (thread_pool_env) {
        try {
            thread_pool_size_ = static_cast<std::size_t>(std::stoul(thread_pool_env));
            // Clamp to reasonable range (1-256)
            if (thread_pool_size_ < 1) thread_pool_size_ = 1;
            if (thread_pool_size_ > 256) thread_pool_size_ = 256;
        } catch (...) {
            // Invalid value, use default
        }
    }

    const char* log_history_env = std::getenv("ALPACAHTTP_LOG_HISTORY_LIMIT");
    if (log_history_env) {
        std::string limit_str = log_history_env;
        std::transform(limit_str.begin(), limit_str.end(), limit_str.begin(), ::tolower);
        if (limit_str == "unlimited" || limit_str == "none") {
            log_history_limit_ = 0;
        } else {
            try {
                log_history_limit_ = static_cast<std::size_t>(std::stoull(limit_str));
            } catch (...) {
                // Invalid value, use default
            }
        }
    }
}

} // namespace alpacahttp

// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
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
#include <cctype>
#include <limits>
#include <string_view>

namespace alpacahttp {

namespace {

int count_leading_spaces(const std::string& line) {
    int count = 0;
    for (char ch : line) {
        if (ch != ' ') {
            break;
        }
        ++count;
    }
    return count;
}

std::string trim_copy(std::string_view input) {
    std::size_t start = 0;
    std::size_t end = input.size();
    while (start < end && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return std::string(input.substr(start, end - start));
}

std::string strip_inline_comment(const std::string& line) {
    auto pos = line.find('#');
    if (pos == std::string::npos) {
        return line;
    }
    return line.substr(0, pos);
}

std::string unquote_string(const std::string& value) {
    if (value.size() < 2) {
        return value;
    }
    char first = value.front();
    char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool parse_bool_value(const std::string& value, bool& result) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1" || lower == "yes") {
        result = true;
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no") {
        result = false;
        return true;
    }
    return false;
}

bool parse_uint16_value(const std::string& value, std::uint16_t& result) {
    try {
        auto parsed = std::stoul(value);
        if (parsed > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        result = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_size_value(const std::string& value, std::size_t& result) {
    try {
        auto parsed = std::stoull(value);
        result = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

void Config::load_config_from_yaml(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return;
    }

    std::string current_section;
    std::string line;
    while (std::getline(file, line)) {
        std::string no_comment = strip_inline_comment(line);
        std::string trimmed = trim_copy(no_comment);
        if (trimmed.empty()) {
            continue;
        }

        int indent = count_leading_spaces(line);
        if (indent == 0) {
            if (!trimmed.empty() && trimmed.back() == ':') {
                current_section = trimmed.substr(0, trimmed.size() - 1);
            } else {
                current_section.clear();
            }
            continue;
        }

        auto delimiter = trimmed.find(':');
        if (delimiter == std::string::npos) {
            continue;
        }

        std::string key = trim_copy(trimmed.substr(0, delimiter));
        std::string value = trim_copy(trimmed.substr(delimiter + 1));
        value = unquote_string(value);

        if (current_section == "http") {
            if (key == "port") {
                parse_uint16_value(value, http_port_);
            } else if (key == "thread_pool_size") {
                parse_size_value(value, thread_pool_size_);
            }
        } else if (current_section == "discovery") {
            if (key == "enabled") {
                bool enabled = discovery_enabled_;
                if (parse_bool_value(value, enabled)) {
                    discovery_enabled_ = enabled;
                }
            }
        } else if (current_section == "logging") {
            if (key == "level") {
                log_level_ = parse_log_level(value);
            } else if (key == "directory") {
                if (!value.empty()) {
                    log_directory_ = value;
                }
            } else if (key == "file_enabled") {
                bool enabled = file_logging_enabled_;
                if (parse_bool_value(value, enabled)) {
                    file_logging_enabled_ = enabled;
                }
            } else if (key == "retention_days") {
                try {
                    int parsed = std::stoi(value);
                    if (parsed < 0) parsed = 0;
                    log_retention_days_ = parsed;
                } catch (...) {
                    // Keep default
                }
            }
        } else if (current_section == "server") {
            if (key == "name") {
                server_name_ = value;
            } else if (key == "manufacturer") {
                manufacturer_ = value;
            } else if (key == "location") {
                location_ = value;
            }
        }
    }
}

bool Config::load(const std::string& config_path) {
    config_path_ = config_path;
    load_config_from_yaml(config_path_);
    apply_environment_overrides();
    return true;
}

bool Config::load_default() {
    // Try to load from default location
    const std::string primary = "config/default.yaml";
    {
        std::ifstream file(primary);
        if (file.is_open()) {
            return load(primary);
        }
    }

    const std::string fallback = "AlpacaHTTP/config/default.yaml";
    {
        std::ifstream file(fallback);
        if (file.is_open()) {
            return load(fallback);
        }
    }

    return load(primary);
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

    const char* log_dir_env = std::getenv("ALPACAHTTP_LOG_DIRECTORY");
    if (log_dir_env && *log_dir_env) {
        log_directory_ = log_dir_env;
    }

    const char* file_log_env = std::getenv("ALPACAHTTP_FILE_LOGGING");
    if (file_log_env) {
        bool enabled = file_logging_enabled_;
        if (parse_bool_value(file_log_env, enabled)) {
            file_logging_enabled_ = enabled;
        }
    }

    const char* retention_env = std::getenv("ALPACAHTTP_LOG_RETENTION_DAYS");
    if (retention_env) {
        try {
            int parsed = std::stoi(retention_env);
            if (parsed < 0) parsed = 0;
            log_retention_days_ = parsed;
        } catch (...) {
            // Keep value from yaml/default
        }
    }
}

} // namespace alpacahttp

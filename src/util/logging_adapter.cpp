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

#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/config.h>
#include <alpacacore/util/logging.h>
#include <string>
#include <deque>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sstream>

namespace alpacahttp::util {

namespace {
    std::mutex g_sink_mutex;
    std::mutex g_history_mutex;
    std::deque<std::string> g_log_history;
    std::size_t g_log_history_limit = 2000;
    alpacacore::logging::LogSink g_external_sink;

    const char* level_to_string(alpacacore::logging::LogLevel level) {
        switch (level) {
            case alpacacore::logging::LogLevel::Trace: return "TRACE";
            case alpacacore::logging::LogLevel::Debug: return "DEBUG";
            case alpacacore::logging::LogLevel::Info: return "INFO";
            case alpacacore::logging::LogLevel::Warn: return "WARN";
            case alpacacore::logging::LogLevel::Error: return "ERROR";
            case alpacacore::logging::LogLevel::Critical: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }

    std::string format_log_line(alpacacore::logging::LogLevel level,
                                std::string_view component,
                                std::string_view message) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::ostringstream stream;
        stream << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
               << "." << std::setfill('0') << std::setw(3) << ms.count()
               << " [" << level_to_string(level) << "] "
               << "[" << component << "] "
               << message;
        return stream.str();
    }

void append_log_history(std::string line) {
        std::lock_guard<std::mutex> lock(g_history_mutex);
        g_log_history.push_back(std::move(line));
        if (g_log_history_limit > 0) {
            while (g_log_history.size() > g_log_history_limit) {
                g_log_history.pop_front();
            }
        }
    }

    void log_sink(alpacacore::logging::LogLevel level,
                  std::string_view component,
                  std::string_view message) {
        const std::string line = format_log_line(level, component, message);
        {
            std::lock_guard<std::mutex> lock(g_sink_mutex);
            if (g_external_sink) {
                g_external_sink(level, component, message);
            } else {
                std::cerr << line << std::endl;
            }
        }
        append_log_history(line);
    }
}

alpacacore::logging::LogLevel convert_log_level(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return alpacacore::logging::LogLevel::Debug;
        case LogLevel::INFO: return alpacacore::logging::LogLevel::Info;
        case LogLevel::WARNING: return alpacacore::logging::LogLevel::Warn;
        case LogLevel::ERROR: return alpacacore::logging::LogLevel::Error;
        default: return alpacacore::logging::LogLevel::Info;
    }
}

void init_logging(const Config& config) {
    // Apply the configured log level before any components start logging
    alpacacore::logging::set_log_level(convert_log_level(config.log_level()));
    alpacacore::logging::set_log_sink(log_sink);
    set_log_history_limit(config.log_history_limit());
}

void log_debug(const std::string& message) {
    alpacacore::logging::log(alpacacore::logging::LogLevel::Debug, "AlpacaHTTP", message);
}

void log_info(const std::string& message) {
    alpacacore::logging::log(alpacacore::logging::LogLevel::Info, "AlpacaHTTP", message);
}

void log_warning(const std::string& message) {
    alpacacore::logging::log(alpacacore::logging::LogLevel::Warn, "AlpacaHTTP", message);
}

void log_error(const std::string& message) {
    alpacacore::logging::log(alpacacore::logging::LogLevel::Error, "AlpacaHTTP", message);
}

std::string get_log_history_text() {
    std::lock_guard<std::mutex> lock(g_history_mutex);
    if (g_log_history.empty()) {
        return "";
    }

    std::ostringstream stream;
    for (std::size_t i = 0; i < g_log_history.size(); ++i) {
        stream << g_log_history[i];
        if (i + 1 < g_log_history.size()) {
            stream << "\n";
        }
    }
    stream << "\n";
    return stream.str();
}

void set_external_log_sink(alpacacore::logging::LogSink sink) {
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    g_external_sink = std::move(sink);
}

void set_log_history_limit(std::size_t limit) {
    std::lock_guard<std::mutex> lock(g_history_mutex);
    g_log_history_limit = limit;
    if (g_log_history_limit > 0) {
        while (g_log_history.size() > g_log_history_limit) {
            g_log_history.pop_front();
        }
    }
}

std::size_t get_log_history_limit() {
    std::lock_guard<std::mutex> lock(g_history_mutex);
    return g_log_history_limit;
}

} // namespace alpacahttp::util

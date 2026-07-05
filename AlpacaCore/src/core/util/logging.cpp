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

#include <alpacacore/util/logging.h>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace alpacacore::logging {

namespace {
    std::mutex g_log_mutex;
    LogSink g_log_sink;
    LogLevel g_min_log_level = LogLevel::Info;
    
    const char* level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::Trace:    return "TRACE";
            case LogLevel::Debug:    return "DEBUG";
            case LogLevel::Info:     return "INFO ";
            case LogLevel::Warn:     return "WARN ";
            case LogLevel::Error:    return "ERROR";
            case LogLevel::Critical: return "CRIT ";
            default:                 return "UNKNW";
        }
    }
    
    void default_stderr_sink(LogLevel level,
                             std::string_view component,
                             std::string_view message) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::tm local_tm{};
#if defined(_WIN32)
        localtime_s(&local_tm, &time_t);
#else
        localtime_r(&time_t, &local_tm);
#endif
        std::lock_guard<std::mutex> lock(g_log_mutex);
        std::cerr << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S")
                  << "." << std::setfill('0') << std::setw(3) << ms.count()
                  << " [" << level_to_string(level) << "] "
                  << "[" << component << "] "
                  << message << std::endl;
    }
}

void set_log_sink(LogSink sink) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_sink = std::move(sink);
}

LogSink get_log_sink() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_log_sink;
}

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_min_log_level = level;
}

LogLevel get_log_level() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_min_log_level;
}

void log(LogLevel level,
         std::string_view component,
         std::string_view message) {
    LogSink sink;
    LogLevel min_level;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        min_level = g_min_log_level;
        sink = g_log_sink;
    }

    if (level < min_level) {
        return;
    }

    if (sink) {
        sink(level, component, message);
    } else {
        default_stderr_sink(level, component, message);
    }
}

} // namespace alpacacore::logging

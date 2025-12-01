// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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
        
        std::lock_guard<std::mutex> lock(g_log_mutex);
        std::cerr << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
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

void log(LogLevel level,
         std::string_view component,
         std::string_view message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_sink) {
        g_log_sink(level, component, message);
    } else {
        default_stderr_sink(level, component, message);
    }
}

} // namespace alpacacore::logging


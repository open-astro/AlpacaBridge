// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/config.h>
#include <atomic>
#include <mutex>

namespace alpacahttp::util {

namespace {
    LogSink g_log_sink;
    LogLevel g_log_level = LogLevel::INFO;
    std::mutex g_log_mutex;
}

void init_logging(const Config& config, LogSink sink) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_sink = std::move(sink);
    g_log_level = config.log_level();
}

int log_level_to_int(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return 0;
        case LogLevel::INFO: return 1;
        case LogLevel::WARNING: return 2;
        case LogLevel::ERROR: return 3;
        default: return 1;
    }
}

void log_debug(const std::string& message) {
    if (g_log_level <= LogLevel::DEBUG) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log_sink) {
            g_log_sink(log_level_to_int(LogLevel::DEBUG), message);
        }
    }
}

void log_info(const std::string& message) {
    if (g_log_level <= LogLevel::INFO) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log_sink) {
            g_log_sink(log_level_to_int(LogLevel::INFO), message);
        }
    }
}

void log_warning(const std::string& message) {
    if (g_log_level <= LogLevel::WARNING) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log_sink) {
            g_log_sink(log_level_to_int(LogLevel::WARNING), message);
        }
    }
}

void log_error(const std::string& message) {
    if (g_log_level <= LogLevel::ERROR) {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log_sink) {
            g_log_sink(log_level_to_int(LogLevel::ERROR), message);
        }
    }
}

} // namespace alpacahttp::util


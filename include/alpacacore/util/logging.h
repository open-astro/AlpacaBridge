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

#pragma once

#include <functional>
#include <string_view>

namespace alpacacore::logging {

/**
 * @brief Log severity levels.
 */
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

/**
 * @brief Log sink function type.
 *
 * Called for each log message with the level, component name, and message.
 */
using LogSink = std::function<void(LogLevel level,
                                   std::string_view component,
                                   std::string_view message)>;

/**
 * @brief Set the global log sink.
 *
 * @param sink The log sink function to use. If null, uses default stderr sink.
 */
void set_log_sink(LogSink sink);

/**
 * @brief Get the current log sink.
 *
 * @return The current log sink function.
 */
LogSink get_log_sink();

/**
 * @brief Log a message.
 *
 * @param level Log severity level
 * @param component Component name (e.g., "CameraDriver")
 * @param message Log message
 */
void log(LogLevel level,
         std::string_view component,
         std::string_view message);

} // namespace alpacacore::logging

// Convenience macros
#define ALPACA_LOG_TRACE(component, message) \
    ::alpacacore::logging::log(::alpacacore::logging::LogLevel::Trace, component, message)

#define ALPACA_LOG_DEBUG(component, message) \
    ::alpacacore::logging::log(::alpacacore::logging::LogLevel::Debug, component, message)

#define ALPACA_LOG_INFO(component, message) \
    ::alpacacore::logging::log(::alpacacore::logging::LogLevel::Info, component, message)

#define ALPACA_LOG_WARN(component, message) \
    ::alpacacore::logging::log(::alpacacore::logging::LogLevel::Warn, component, message)

#define ALPACA_LOG_ERROR(component, message) \
    ::alpacacore::logging::log(::alpacacore::logging::LogLevel::Error, component, message)

#define ALPACA_LOG_CRITICAL(component, message) \
    ::alpacacore::logging::log(::alpacacore::logging::LogLevel::Critical, component, message)


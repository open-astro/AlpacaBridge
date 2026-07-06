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

/**
 * @brief Set the minimum log level for output.
 *
 * Messages below this level are ignored before reaching the sink.
 */
void set_log_level(LogLevel level);

/**
 * @brief Get the current minimum log level.
 */
LogLevel get_log_level();

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

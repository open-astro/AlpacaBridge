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

#pragma once

#include "../config.h"
#include <alpacacore/util/logging.h>
#include <string>
#include <functional>

namespace alpacahttp::util {

// Initialize logging adapter - connects AlpacaHTTP to AlpacaCore logging
void init_logging(const Config& config);

// Log functions - these use AlpacaCore logging system
void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warning(const std::string& message);
void log_error(const std::string& message);

// Retrieve recent log history as plain text.
std::string get_log_history_text();

// Optional external sink (e.g., stdout logger) layered on top of history capture.
void set_external_log_sink(alpacacore::logging::LogSink sink);

// Convert AlpacaHTTP LogLevel to AlpacaCore LogLevel
alpacacore::logging::LogLevel convert_log_level(LogLevel level);

} // namespace alpacahttp::util

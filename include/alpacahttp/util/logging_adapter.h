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

// Convert AlpacaHTTP LogLevel to AlpacaCore LogLevel
alpacacore::logging::LogLevel convert_log_level(LogLevel level);

} // namespace alpacahttp::util


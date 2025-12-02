// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

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


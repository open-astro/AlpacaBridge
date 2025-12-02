// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#pragma once

#include "../config.h"
#include <string>
#include <functional>

namespace alpacahttp::util {

// Log sink function type (matches AlpacaCore interface)
using LogSink = std::function<void(int level, const std::string& message)>;

// Initialize logging adapter
void init_logging(const Config& config, LogSink sink);

// Log functions
void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warning(const std::string& message);
void log_error(const std::string& message);

// Convert LogLevel to integer
int log_level_to_int(LogLevel level);

} // namespace alpacahttp::util


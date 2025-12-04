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

#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/config.h>
#include <alpacacore/util/logging.h>
#include <string>

namespace alpacahttp::util {

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
    // AlpacaHTTP uses AlpacaCore's logging system directly
    // The default stderr sink is already set up in AlpacaCore
    // If needed, we can set a custom sink here, but for now we use the default
    (void)config; // Config may be used in the future for log level filtering
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

} // namespace alpacahttp::util


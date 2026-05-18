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
#include <cstdint>
#include <optional>
#include <vector>

namespace alpacahttp::util {

// Initialize logging adapter - connects AlpacaHTTP to AlpacaCore logging
void init_logging(const Config& config);

// Log functions - these use AlpacaCore logging system
void log_debug(const std::string& message);
void log_info(const std::string& message);
void log_warning(const std::string& message);
void log_error(const std::string& message);

// Optional external sink (e.g., stdout logger) layered on top of disk capture.
void set_external_log_sink(alpacacore::logging::LogSink sink);

// Returns the basename of today's daily log file (alpacabridge-YYYY-MM-DD.log).
// Useful for the legacy /management/v1/logs endpoint, which now serves the
// current day's file rather than an in-memory buffer.
std::string current_log_filename();

// Convert AlpacaHTTP LogLevel to AlpacaCore LogLevel
alpacacore::logging::LogLevel convert_log_level(LogLevel level);

// On-disk log file management.
struct LogFileInfo {
    std::string name;        // file basename only (no path)
    std::uint64_t size = 0;  // in bytes
    std::int64_t modified_unix = 0; // last-modified epoch seconds
};

// Configure the directory that on-disk log files are written to. Falls back to a
// user-writable directory if the configured path cannot be opened for writing.
// Returns the directory that was actually selected.
std::string configure_log_directory(const std::string& preferred, bool file_logging_enabled);

// Returns the directory currently used for on-disk logging (empty if disabled).
std::string get_log_directory();

// List the log files currently stored in the log directory.
std::vector<LogFileInfo> list_log_files();

// Read the contents of a named log file. Throws std::runtime_error on
// validation or I/O failure. The `name` must be a basename only (no path).
std::string read_log_file(const std::string& name);

// Delete the named log file. If the targeted file is today's active daily
// file, its handle is closed first and the next log line will reopen (and
// recreate) it. Throws std::runtime_error on validation or I/O failure.
void delete_log_file(const std::string& name);

// True if the provided basename matches the alpacabridge log file pattern.
bool is_valid_log_filename(const std::string& name);

// Configure how long daily log files are kept on disk before being auto-deleted.
// `days > 0` enables cleanup; `0` disables it (keep forever). Cleanup runs once
// at configure time and then on every day-rollover inside the file sink.
void set_log_retention_days(int days);

// Delete daily log files whose embedded date is older than retention_days
// before today. Safe to call repeatedly; no-op when retention is disabled.
// Returns the number of files removed.
std::size_t prune_old_log_files();

// Persist the currently-active log level so it survives a server restart.
// Stored in config/runtime_state.json relative to cwd; failures are logged at
// warning level but do not throw — callers should never let a save error block
// the user-visible operation that requested it.
void save_runtime_log_level(alpacacore::logging::LogLevel level);

// Read the persisted log level, if any. Returns nullopt when the state file
// is missing, unreadable, or contains an unrecognized level.
std::optional<alpacacore::logging::LogLevel> load_runtime_log_level();

} // namespace alpacahttp::util

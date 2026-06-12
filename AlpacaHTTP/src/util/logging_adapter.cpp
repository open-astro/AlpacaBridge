// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/config.h>
#include <alpacacore/util/logging.h>
#include <string>
#include <mutex>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <system_error>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <unistd.h>

namespace alpacahttp::util {

namespace {
    std::mutex g_sink_mutex;
    alpacacore::logging::LogSink g_external_sink;

    // File-sink state
    std::mutex g_file_mutex;
    std::filesystem::path g_log_directory;
    std::ofstream g_log_file;
    std::string g_log_file_name;  // basename currently being written to
    std::string g_log_file_date;  // YYYY-MM-DD of currently-open file
    bool g_file_logging_enabled = false;
    int g_log_retention_days = 0;  // 0 = forever

    const std::regex kLogFilenameRegex(R"(^alpacabridge-(\d{4})-(\d{2})-(\d{2})\.log$)");

    const char* level_to_string(alpacacore::logging::LogLevel level) {
        switch (level) {
            case alpacacore::logging::LogLevel::Trace: return "TRACE";
            case alpacacore::logging::LogLevel::Debug: return "DEBUG";
            case alpacacore::logging::LogLevel::Info: return "INFO";
            case alpacacore::logging::LogLevel::Warn: return "WARN";
            case alpacacore::logging::LogLevel::Error: return "ERROR";
            case alpacacore::logging::LogLevel::Critical: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }

    std::string current_local_date() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&time_t, &tm_buf);
        std::ostringstream stream;
        stream << std::put_time(&tm_buf, "%Y-%m-%d");
        return stream.str();
    }

    std::string format_log_line(alpacacore::logging::LogLevel level,
                                std::string_view component,
                                std::string_view message) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
        localtime_r(&time_t, &tm_buf);

        std::ostringstream stream;
        stream << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
               << "." << std::setfill('0') << std::setw(3) << ms.count()
               << " [" << level_to_string(level) << "] "
               << "[" << component << "] "
               << message;
        return stream.str();
    }

    // Try to use the directory at `path` for log writes. Returns true if the
    // directory now exists and a probe file could be opened for append.
    bool try_use_directory(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec && !std::filesystem::exists(path)) {
            return false;
        }
        const auto probe = path / ".alpacabridge-write-test";
        std::ofstream test(probe, std::ios::app);
        if (!test.is_open()) {
            return false;
        }
        test.close();
        std::error_code remove_ec;
        std::filesystem::remove(probe, remove_ec);
        return true;
    }

    std::filesystem::path user_state_fallback_directory() {
        const char* xdg = std::getenv("XDG_STATE_HOME");
        if (xdg && *xdg) {
            return std::filesystem::path(xdg) / "AlpacaBridge" / "logs";
        }
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            if (auto* pw = getpwuid(getuid())) {
                home = pw->pw_dir;
            }
        }
        if (home && *home) {
            return std::filesystem::path(home) / ".local" / "state" / "AlpacaBridge" / "logs";
        }
        return std::filesystem::path("/tmp") / "AlpacaBridge" / "logs";
    }

    // Caller must hold g_file_mutex.
    void open_log_file_for_today_locked() {
        if (g_log_directory.empty()) {
            return;
        }
        const std::string date = current_local_date();
        const std::string name = "alpacabridge-" + date + ".log";
        const auto path = g_log_directory / name;

        if (g_log_file.is_open() && g_log_file_date == date) {
            return;
        }
        if (g_log_file.is_open()) {
            g_log_file.close();
        }
        g_log_file.open(path, std::ios::app);
        if (g_log_file.is_open()) {
            g_log_file_name = name;
            g_log_file_date = date;
        } else {
            g_log_file_name.clear();
            g_log_file_date.clear();
        }
    }

    // Cutoff date (YYYY-MM-DD) for retention. Files with an embedded date
    // strictly less than this string are eligible for deletion. retention=0
    // returns an empty string and disables pruning.
    std::string retention_cutoff_locked() {
        if (g_log_retention_days <= 0) {
            return "";
        }
        const auto now = std::chrono::system_clock::now() -
                         std::chrono::hours(24) * g_log_retention_days;
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&time_t, &tm_buf);
        std::ostringstream stream;
        stream << std::put_time(&tm_buf, "%Y-%m-%d");
        return stream.str();
    }

    std::size_t prune_old_files_locked() {
        if (g_log_directory.empty()) {
            return 0;
        }
        const std::string cutoff = retention_cutoff_locked();
        if (cutoff.empty()) {
            return 0;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(g_log_directory, ec)) {
            return 0;
        }
        std::size_t removed = 0;
        for (const auto& entry : std::filesystem::directory_iterator(g_log_directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            std::smatch match;
            if (!std::regex_match(name, match, kLogFilenameRegex)) continue;
            // File's date is the YYYY-MM-DD captured by group 1..3 — but the
            // entire YYYY-MM-DD substring is the chars after "alpacabridge-"
            // and before ".log". Extract and compare lexicographically.
            const std::string file_date = name.substr(13, 10);  // YYYY-MM-DD
            if (file_date >= cutoff) continue;
            if (g_log_file_name == name) continue;  // never prune today's file
            std::error_code rm_ec;
            if (std::filesystem::remove(entry.path(), rm_ec)) {
                ++removed;
            }
        }
        return removed;
    }

    void write_to_file(const std::string& line) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        if (!g_file_logging_enabled || g_log_directory.empty()) {
            return;
        }
        const std::string today = current_local_date();
        const bool day_changed = (g_log_file_date != today);
        if (!g_log_file.is_open() || day_changed) {
            open_log_file_for_today_locked();
            if (day_changed) {
                // Take the opportunity to retire files past retention.
                prune_old_files_locked();
            }
        }
        if (g_log_file.is_open()) {
            g_log_file << line << '\n';
            g_log_file.flush();
        }
    }

    void log_sink(alpacacore::logging::LogLevel level,
                  std::string_view component,
                  std::string_view message) {
        const std::string line = format_log_line(level, component, message);
        {
            std::lock_guard<std::mutex> lock(g_sink_mutex);
            if (g_external_sink) {
                g_external_sink(level, component, message);
            } else {
                std::cerr << line << std::endl;
            }
        }
        write_to_file(line);
    }
}

alpacacore::logging::LogLevel convert_log_level(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return alpacacore::logging::LogLevel::Debug;
        case LogLevel::INFO: return alpacacore::logging::LogLevel::Info;
        case LogLevel::WARNING: return alpacacore::logging::LogLevel::Warn;
        case LogLevel::ERROR: return alpacacore::logging::LogLevel::Error;
        default: return alpacacore::logging::LogLevel::Info;
    }
}

std::string configure_log_directory(const std::string& preferred, bool file_logging_enabled) {
    // We must NOT call log_warning while holding g_file_mutex: log_warning
    // routes through log_sink -> write_to_file, which re-acquires the same
    // mutex on the same thread and self-deadlocks on a non-recursive
    // std::mutex (manifests as a futex_wait hang during startup whenever
    // the preferred log directory is unwritable). Capture any warning
    // message into a local string under the lock, release, then emit.
    std::string pending_warning;
    std::string result;
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);

        if (g_log_file.is_open()) {
            g_log_file.close();
        }
        g_log_file_name.clear();
        g_log_file_date.clear();
        g_log_directory.clear();
        g_file_logging_enabled = false;

        if (!file_logging_enabled) {
            return "";
        }

        std::filesystem::path chosen;
        if (!preferred.empty() && try_use_directory(preferred)) {
            chosen = preferred;
        } else {
            const auto fallback = user_state_fallback_directory();
            if (try_use_directory(fallback)) {
                chosen = fallback;
                pending_warning = "Log directory '" + preferred +
                                  "' not writable; using fallback '" +
                                  fallback.string() + "'";
            } else {
                pending_warning = "No writable log directory available; "
                                  "file logging disabled";
            }
        }

        if (!chosen.empty()) {
            g_log_directory = chosen;
            g_file_logging_enabled = true;
            open_log_file_for_today_locked();
            result = g_log_directory.string();
        }
    }
    if (!pending_warning.empty()) {
        log_warning(pending_warning);
    }
    return result;
}

std::string get_log_directory() {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    if (!g_file_logging_enabled) {
        return "";
    }
    return g_log_directory.string();
}

bool is_valid_log_filename(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::regex_match(name, kLogFilenameRegex);
}

std::vector<LogFileInfo> list_log_files() {
    std::vector<LogFileInfo> result;
    std::filesystem::path directory;
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        directory = g_log_directory;
    }
    if (directory.empty()) {
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (!is_valid_log_filename(name)) continue;

        LogFileInfo info;
        info.name = name;
        std::error_code size_ec;
        const auto size = entry.file_size(size_ec);
        info.size = size_ec ? 0 : static_cast<std::uint64_t>(size);

        std::error_code time_ec;
        const auto ftime = entry.last_write_time(time_ec);
        if (!time_ec) {
            const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            info.modified_unix = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    sctp.time_since_epoch()).count());
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(),
              [](const LogFileInfo& a, const LogFileInfo& b) {
                  return a.name > b.name; // newest first (date in name)
              });
    return result;
}

std::string read_log_file(const std::string& name) {
    if (!is_valid_log_filename(name)) {
        throw std::runtime_error("Invalid log file name");
    }
    std::filesystem::path directory;
    bool is_current = false;
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        directory = g_log_directory;
        is_current = (g_log_file_name == name);
        if (is_current && g_log_file.is_open()) {
            g_log_file.flush();
        }
    }
    if (directory.empty()) {
        throw std::runtime_error("File logging is not configured");
    }

    const auto path = directory / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        throw std::runtime_error("Log file not found");
    }

    // Guard against pathological single-day log volumes (e.g., a runaway driver
    // error loop) blowing up a single HTTP response into a multi-GB allocation.
    // Callers can still read large files manually via the on-disk path.
    std::error_code size_ec;
    const auto file_size = std::filesystem::file_size(path, size_ec);
    if (!size_ec && file_size > kMaxLogFileReadBytes) {
        throw std::runtime_error(
            "Log file exceeds 10 MiB; download the file directly from disk");
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open log file");
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

void delete_log_file(const std::string& name) {
    if (!is_valid_log_filename(name)) {
        throw std::runtime_error("Invalid log file name");
    }
    std::lock_guard<std::mutex> lock(g_file_mutex);
    if (g_log_directory.empty()) {
        throw std::runtime_error("File logging is not configured");
    }
    if (g_log_file_name == name) {
        // Close the current handle so we can remove the file. A subsequent log
        // line will reopen (and recreate) it for today's date.
        if (g_log_file.is_open()) {
            g_log_file.close();
        }
        g_log_file_name.clear();
        g_log_file_date.clear();
    }
    const auto path = g_log_directory / name;
    std::error_code ec;
    if (!std::filesystem::remove(path, ec)) {
        if (ec) {
            throw std::runtime_error("Failed to delete log file: " + ec.message());
        }
        throw std::runtime_error("Log file not found");
    }
}

void init_logging(const Config& config) {
    // Apply the configured log level before any components start logging
    alpacacore::logging::set_log_level(convert_log_level(config.log_level()));
    alpacacore::logging::set_log_sink(log_sink);
    set_log_retention_days(config.log_retention_days());
    const std::string selected =
        configure_log_directory(config.log_directory(), config.file_logging_enabled());
    if (config.file_logging_enabled() && !selected.empty()) {
        log_info("File logging active in " + selected);
        const std::size_t removed = prune_old_log_files();
        if (removed > 0) {
            log_info("Removed " + std::to_string(removed) +
                     " log file(s) older than retention");
        }
    }
    // A user-chosen level (set via /management/v1/loglevel) overrides the
    // default-yaml level on restart.
    if (auto persisted = load_runtime_log_level()) {
        alpacacore::logging::set_log_level(*persisted);
        const char* name = "INFO";
        switch (*persisted) {
            case alpacacore::logging::LogLevel::Trace:    name = "TRACE"; break;
            case alpacacore::logging::LogLevel::Debug:    name = "DEBUG"; break;
            case alpacacore::logging::LogLevel::Info:     name = "INFO"; break;
            case alpacacore::logging::LogLevel::Warn:     name = "WARNING"; break;
            case alpacacore::logging::LogLevel::Error:    name = "ERROR"; break;
            case alpacacore::logging::LogLevel::Critical: name = "CRITICAL"; break;
        }
        log_info(std::string("Restored persisted log level: ") + name);
    }
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

void set_external_log_sink(alpacacore::logging::LogSink sink) {
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    g_external_sink = std::move(sink);
}

std::string current_log_filename() {
    return "alpacabridge-" + current_local_date() + ".log";
}

void set_log_retention_days(int days) {
    if (days < 0) days = 0;
    std::lock_guard<std::mutex> lock(g_file_mutex);
    g_log_retention_days = days;
}

std::size_t prune_old_log_files() {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    return prune_old_files_locked();
}

namespace {

const std::filesystem::path kRuntimeStateFile =
    std::filesystem::path("config") / "runtime_state.json";

const char* core_level_to_persistent_string(alpacacore::logging::LogLevel level) {
    switch (level) {
        case alpacacore::logging::LogLevel::Trace:    return "TRACE";
        case alpacacore::logging::LogLevel::Debug:    return "DEBUG";
        case alpacacore::logging::LogLevel::Info:     return "INFO";
        case alpacacore::logging::LogLevel::Warn:     return "WARNING";
        case alpacacore::logging::LogLevel::Error:    return "ERROR";
        case alpacacore::logging::LogLevel::Critical: return "CRITICAL";
    }
    return "INFO";
}

std::optional<alpacacore::logging::LogLevel> persistent_string_to_core_level(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (text == "TRACE")    return alpacacore::logging::LogLevel::Trace;
    if (text == "DEBUG")    return alpacacore::logging::LogLevel::Debug;
    if (text == "INFO")     return alpacacore::logging::LogLevel::Info;
    if (text == "WARN" || text == "WARNING") return alpacacore::logging::LogLevel::Warn;
    if (text == "ERROR")    return alpacacore::logging::LogLevel::Error;
    if (text == "CRITICAL") return alpacacore::logging::LogLevel::Critical;
    return std::nullopt;
}

} // namespace

void save_runtime_log_level(alpacacore::logging::LogLevel level) {
    try {
        if (kRuntimeStateFile.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(kRuntimeStateFile.parent_path(), ec);
        }

        // Round-trip through any existing state so we don't blow away other
        // fields a future revision might add.
        nlohmann::json payload = nlohmann::json::object();
        std::ifstream existing(kRuntimeStateFile);
        if (existing) {
            try {
                existing >> payload;
                if (!payload.is_object()) {
                    payload = nlohmann::json::object();
                }
            } catch (...) {
                payload = nlohmann::json::object();
            }
        }
        payload["LogLevel"] = core_level_to_persistent_string(level);

        const auto tmp_path = kRuntimeStateFile.string() + ".tmp";
        {
            std::ofstream out(tmp_path, std::ios::trunc);
            if (!out) {
                throw std::runtime_error("unable to open " + tmp_path + " for writing");
            }
            out << payload.dump(2);
            out.flush();
        }
        std::error_code rename_ec;
        std::filesystem::rename(tmp_path, kRuntimeStateFile, rename_ec);
        if (rename_ec) {
            std::filesystem::remove(tmp_path);
            throw std::runtime_error("rename failed: " + rename_ec.message());
        }
    } catch (const std::exception& e) {
        log_warning(std::string("Failed to persist log level: ") + e.what());
    }
}

std::optional<alpacacore::logging::LogLevel> load_runtime_log_level() {
    std::error_code ec;
    if (!std::filesystem::exists(kRuntimeStateFile, ec)) {
        return std::nullopt;
    }
    try {
        std::ifstream in(kRuntimeStateFile);
        if (!in) {
            return std::nullopt;
        }
        nlohmann::json payload;
        in >> payload;
        if (!payload.is_object() || !payload.contains("LogLevel")) {
            return std::nullopt;
        }
        const auto& level_node = payload["LogLevel"];
        if (!level_node.is_string()) {
            return std::nullopt;
        }
        return persistent_string_to_core_level(level_node.get<std::string>());
    } catch (const std::exception& e) {
        log_warning(std::string("Failed to read runtime log state: ") + e.what());
        return std::nullopt;
    }
}

} // namespace alpacahttp::util

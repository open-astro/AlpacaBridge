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

#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/config.h>
#include <alpacacore/util/logging.h>
#include <string>
#include <deque>
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
    std::mutex g_history_mutex;
    std::deque<std::string> g_log_history;
    alpacacore::logging::LogSink g_external_sink;

    // File-sink state
    std::mutex g_file_mutex;
    std::filesystem::path g_log_directory;
    std::ofstream g_log_file;
    std::string g_log_file_name;  // basename currently being written to
    std::string g_log_file_date;  // YYYY-MM-DD of currently-open file
    bool g_file_logging_enabled = false;

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

    void append_log_history(std::string line) {
        std::lock_guard<std::mutex> lock(g_history_mutex);
        g_log_history.push_back(std::move(line));
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

    void write_to_file(const std::string& line) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        if (!g_file_logging_enabled || g_log_directory.empty()) {
            return;
        }
        const std::string today = current_local_date();
        if (!g_log_file.is_open() || g_log_file_date != today) {
            open_log_file_for_today_locked();
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
        append_log_history(line);
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
            std::cerr << "[AlpacaHTTP] log directory '" << preferred
                      << "' not writable, using fallback '" << fallback.string()
                      << "'" << std::endl;
        } else {
            std::cerr << "[AlpacaHTTP] no writable log directory available; "
                         "file logging disabled" << std::endl;
            return "";
        }
    }

    g_log_directory = chosen;
    g_file_logging_enabled = true;
    open_log_file_for_today_locked();
    return g_log_directory.string();
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
    const std::string selected =
        configure_log_directory(config.log_directory(), config.file_logging_enabled());
    if (config.file_logging_enabled() && !selected.empty()) {
        log_info("File logging active in " + selected);
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

std::string get_log_history_text() {
    std::lock_guard<std::mutex> lock(g_history_mutex);
    if (g_log_history.empty()) {
        return "";
    }

    std::ostringstream stream;
    for (std::size_t i = 0; i < g_log_history.size(); ++i) {
        stream << g_log_history[i];
        if (i + 1 < g_log_history.size()) {
            stream << "\n";
        }
    }
    stream << "\n";
    return stream.str();
}

void set_external_log_sink(alpacacore::logging::LogSink sink) {
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    g_external_sink = std::move(sink);
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

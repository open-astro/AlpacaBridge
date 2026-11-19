// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Per-day log file size cap (logging.max_file_size_mb): at the cap the sink
// rotates the day's file to a numbered part and keeps logging; it never drops
// a line. Only the newest util::kMaxRotatedLogFiles parts of a day are kept.
// Total cap (logging.max_total_size_mb, #2422): all log files together, the
// oldest deleted first across days, the open file never.

#include <alpacahttp/config.h>
#include <alpacahttp/util/logging_adapter.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_assert.h"

namespace fs = std::filesystem;
namespace util = alpacahttp::util;

namespace {

constexpr std::uintmax_t kMiB = 1024ull * 1024;

// A fresh directory under $TMPDIR (or /tmp).
std::string temp_template(const char* stem) {
    const char* tmpdir = std::getenv("TMPDIR");
    return std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/" + stem + "_XXXXXX";
}

fs::path make_dir() {
    std::string tmpl = temp_template("alpacahttp_log_cap");
    EXPECT(::mkdtemp(tmpl.data()) != nullptr);
    return tmpl;
}

// A log file of exactly @p bytes.
void write_file(const fs::path& p, std::size_t bytes) { std::ofstream(p, std::ios::binary) << std::string(bytes, 'z'); }

// Bytes of all regular files in @p dir.
std::uintmax_t dir_bytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) total += entry.file_size();
    }
    return total;
}

// alpacabridge-<date>.log, or its part N.
fs::path dated(const fs::path& dir, const std::string& date, int n = 0) {
    return dir / ("alpacabridge-" + date + (n > 0 ? "." + std::to_string(n) : "") + ".log");
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string last_line(const std::string& text) {
    std::string t = text;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    const auto pos = t.rfind('\n');
    return pos == std::string::npos ? t : t.substr(pos + 1);
}

// The local date @p days before today, YYYY-MM-DD, as the logger names its files.
std::string local_date_days_ago(int days) {
    const auto then = std::chrono::system_clock::now() - std::chrono::hours(24) * days;
    const std::time_t t = std::chrono::system_clock::to_time_t(then);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}

// Points the sink at `dir`, silences stderr, and sets the caps before the file
// is opened (as init_logging does), so a reopened file is judged against them.
// The total cap is off unless a case sets it.
fs::path start(const fs::path& dir, std::uintmax_t cap, std::uintmax_t total_cap = 0) {
    alpacahttp::Config config;
    config.set_file_logging_enabled(false);
    config.set_log_level(alpacahttp::LogLevel::INFO);
    util::init_logging(config);
    util::set_external_log_sink([](alpacacore::logging::LogLevel, std::string_view, std::string_view) {});
    util::set_log_max_file_bytes(cap);
    util::set_log_max_total_bytes(total_cap);
    EXPECT(util::configure_log_directory(dir.string(), true) == dir.string());
    return dir / util::current_log_filename();
}

// Today's rotated part N: alpacabridge-YYYY-MM-DD.N.log.
std::string part_name(int n) {
    std::string name = util::current_log_filename();
    return name.insert(name.size() - 4, "." + std::to_string(n));
}

void write_lines(int count) {
    for (int i = 0; i < count; ++i) {
        util::log_info("line " + std::to_string(i) + " padding padding padding padding");
    }
}

// Starts logging through init_logging from a yaml file, as the server does, with
// file logging in @p dir and the two caps set; returns the text of today's file.
std::string init_from_yaml(const fs::path& dir, int max_file_size_mb, int max_total_size_mb) {
    for (const char* name : {"ALPACAHTTP_LOG_LEVEL", "ALPACAHTTP_LOG_DIRECTORY", "ALPACAHTTP_LOG_RETENTION_DAYS",
                             "ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB"}) {
        ::unsetenv(name);
    }
    const fs::path yaml = dir / "config.yaml";
    {
        std::ofstream out(yaml);
        out << "logging:\n"
            << "  level: INFO\n"
            << "  directory: " << dir.string() << "\n"
            << "  file_enabled: true\n"
            << "  retention_days: 0\n"
            << "  max_file_size_mb: " << max_file_size_mb << "\n"
            << "  max_total_size_mb: " << max_total_size_mb << "\n";
    }
    alpacahttp::Config config;
    EXPECT(config.load(yaml.string()));
    EXPECT(config.log_max_file_size_mb() == max_file_size_mb);
    EXPECT(config.log_max_total_size_mb() == max_total_size_mb);
    util::set_external_log_sink([](alpacacore::logging::LogLevel, std::string_view, std::string_view) {});
    util::init_logging(config);
    return read_all(dir / util::current_log_filename());
}

constexpr const char* kTotalBelowFileWarning = "smaller than logging.max_file_size_mb";

}  // namespace

int main() {
    {
        // case: at the cap the file rotates and logging goes on; nothing is dropped
        const fs::path dir = make_dir();
        const std::uintmax_t cap = 4096;
        const fs::path file = start(dir, cap);
        write_lines(500);
        util::log_info("after the cap");
        EXPECT(fs::file_size(file) <= cap);
        EXPECT(last_line(read_all(file)).find("after the cap") != std::string::npos);
        EXPECT(fs::exists(dir / part_name(1)));
        EXPECT(fs::file_size(dir / part_name(1)) <= cap);
        // Consecutive: the newest part ends right before the current file starts.
        const std::string current = read_all(file);
        const std::string part1 = read_all(dir / part_name(1));
        const auto first_line = current.substr(0, current.find('\n'));
        const auto n_first = std::stoi(first_line.substr(first_line.find("line ") + 5));
        EXPECT(last_line(part1).find("line " + std::to_string(n_first - 1) + " ") != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: only the newest kMaxRotatedLogFiles parts of a day are kept
        // A 1-byte cap rotates on every line after the first.
        const fs::path dir = make_dir();
        start(dir, 1);
        write_lines(util::kMaxRotatedLogFiles + 5);
        for (int i = 1; i <= util::kMaxRotatedLogFiles; ++i) {
            EXPECT(fs::exists(dir / part_name(i)));
        }
        EXPECT(!fs::exists(dir / part_name(util::kMaxRotatedLogFiles + 1)));
        fs::remove_all(dir);
    }

    {
        // case: cap 0 means no limit and no rotation
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 0);
        write_lines(500);
        EXPECT(fs::file_size(file) > 4096);
        EXPECT(!fs::exists(dir / part_name(1)));
        fs::remove_all(dir);
    }

    {
        // case: deleting today's file, then logging, recreates it
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 4096);
        write_lines(500);
        util::delete_log_file(util::current_log_filename());
        util::log_info("after delete");
        EXPECT(fs::exists(file));
        EXPECT(read_all(file).find("after delete") != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: reopening an over-cap file rotates it on the next line instead of dropping lines
        const fs::path dir = make_dir();
        const fs::path file = dir / util::current_log_filename();
        {
            std::ofstream out(file);
            out << std::string(8192, 'x') << '\n';
        }
        start(dir, 4096);
        util::log_info("after restart");
        EXPECT(fs::file_size(dir / part_name(1)) == 8193);
        EXPECT(read_all(file).find("after restart") != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: a single line longer than the cap is still written
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 4096);
        util::log_info(std::string(6000, 'y'));
        EXPECT(read_all(file).find(std::string(6000, 'y')) != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: parts are log files the viewer lists (newest first), reads and deletes
        const fs::path dir = make_dir();
        start(dir, 1);
        write_lines(13);  // 12 rotations: parts .1 .. .12
        EXPECT(util::is_valid_log_filename(part_name(1)));
        EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.0.log"));
        EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.19.log"));
        EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.100.log"));
        EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.999.log"));
        EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.01.log"));
        EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.1000.log"));
        const auto files = util::list_log_files();
        EXPECT(files.size() == 13);
        EXPECT(files[0].name == util::current_log_filename());
        for (std::size_t i = 1; i < files.size(); ++i) {
            EXPECT(files[i].name == part_name(static_cast<int>(i)));  // .1, .2, ... .10 in number order
        }
        EXPECT(util::read_log_file(part_name(1)) == read_all(dir / part_name(1)));
        util::delete_log_file(part_name(1));
        EXPECT(!fs::exists(dir / part_name(1)));
        fs::remove_all(dir);
    }

    {
        // case: the default cap fits what the viewer and archive will read
        alpacahttp::Config config;
        EXPECT(config.log_max_file_size_mb() == 5);
        EXPECT(static_cast<std::uintmax_t>(config.log_max_file_size_mb()) * kMiB <= util::kMaxLogFileReadBytes);
        // The total cap holds a night at TRACE (~30 MiB/h on an iOptron rig) and more than one day's part ceiling,
        // so the total cap, not the part count, decides what survives (#2422).
        EXPECT(config.log_max_total_size_mb() == 1024);
        EXPECT(static_cast<std::uintmax_t>(config.log_max_file_size_mb()) * (1 + util::kMaxRotatedLogFiles) >
               static_cast<std::uintmax_t>(config.log_max_total_size_mb()));
    }

    {
        // case: the total cap deletes the oldest files first, across days, at rotation
        // Two seeded days, each a current file and parts .1-.3 of 1000 bytes. The first rotation today makes part .1
        // (just under 4096 bytes) and a fresh, empty file: 8000 + .1 is over 8000, so files go oldest first, the
        // oldest day whole (.3, .2, .1, then its current file), then the next day's .3, and no more.
        const fs::path dir = make_dir();
        const std::string d3 = local_date_days_ago(3);
        const std::string d2 = local_date_days_ago(2);
        for (const auto& date : {d3, d2}) {
            for (int n = 0; n <= 3; ++n) write_file(dated(dir, date, n), 1000);
        }
        const std::uintmax_t total_cap = 8000;
        const fs::path file = start(dir, 4096, total_cap);
        for (int i = 0; i < 1000 && !fs::exists(dir / part_name(1)); ++i) write_lines(1);
        EXPECT(fs::file_size(dir / part_name(1)) > 4000);
        for (int n = 0; n <= 3; ++n) EXPECT(!fs::exists(dated(dir, d3, n)));
        EXPECT(!fs::exists(dated(dir, d2, 3)));
        EXPECT(fs::exists(dated(dir, d2, 2)));
        EXPECT(fs::exists(dated(dir, d2, 1)));
        EXPECT(fs::exists(dated(dir, d2)));  // the evening before stays while today grows
        EXPECT(fs::exists(dir / part_name(1)));
        EXPECT(fs::exists(file));
        // Every later rotation brings the other files back under the cap; only the open file has grown since.
        write_lines(400);
        EXPECT(dir_bytes(dir) - fs::file_size(file) <= total_cap);
        EXPECT(fs::exists(dir / part_name(1)));
        EXPECT(fs::exists(file));
        fs::remove_all(dir);
    }

    {
        // case: the day being written is the newest, whatever dates other files carry
        // A file dated after today (the clock jumped ahead once, then came back) sorts after today by name. The
        // session being written is still the newest, so the total cap deletes the future-dated file before today's
        // parts; by name order alone every rotation would delete today's part .1 and keep the future file forever.
        const fs::path dir = make_dir();
        const fs::path older = dated(dir, local_date_days_ago(2));
        const fs::path future = dated(dir, local_date_days_ago(-400));
        write_file(older, 1000);
        write_file(future, 6000);
        const fs::path file = start(dir, 4096, 8000);
        for (int i = 0; i < 1000 && !fs::exists(dir / part_name(1)); ++i) write_lines(1);
        EXPECT(fs::exists(dir / part_name(1)));
        EXPECT(!fs::exists(older));
        EXPECT(!fs::exists(future));
        EXPECT(fs::exists(file));
        fs::remove_all(dir);
    }

    {
        // case: the file being written counts toward the total cap
        // Yesterday's 1000 bytes alone fit 3000; with today's open file (about 2600 bytes) they do not.
        const fs::path dir = make_dir();
        const fs::path yesterday = dated(dir, local_date_days_ago(1));
        write_file(yesterday, 1000);
        const fs::path file = start(dir, 0, 3000);
        write_lines(30);
        EXPECT(fs::file_size(file) > 2000);
        EXPECT(util::prune_old_log_files() == 1);
        EXPECT(!fs::exists(yesterday));
        EXPECT(fs::exists(file));
        fs::remove_all(dir);
    }

    {
        // case: the file being written is never deleted, even when it alone is over the total cap
        const fs::path dir = make_dir();
        const fs::path yesterday = dated(dir, local_date_days_ago(1));
        write_file(yesterday, 1000);
        const fs::path file = start(dir, 0, 100);
        write_lines(50);
        EXPECT(util::prune_old_log_files() == 1);
        EXPECT(!fs::exists(yesterday));
        EXPECT(fs::exists(file));
        const std::string text = read_all(file);
        EXPECT(text.find("line 0 ") != std::string::npos);
        EXPECT(text.find("line 49 ") != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: total cap 0 keeps every file
        const fs::path dir = make_dir();
        const fs::path old_file = dated(dir, local_date_days_ago(3));
        const fs::path old_part = dated(dir, local_date_days_ago(3), 1);
        write_file(old_file, 100000);
        write_file(old_part, 100000);
        start(dir, 4096, 0);
        write_lines(500);
        EXPECT(util::prune_old_log_files() == 0);
        EXPECT(fs::exists(old_file));
        EXPECT(fs::exists(old_part));
        EXPECT(fs::exists(dir / part_name(1)));
        fs::remove_all(dir);
    }

    {
        // case: start-up applies logging.max_total_size_mb from the config file, oldest day first
        // Three earlier days of 600 KiB each are over 1 MiB: init_logging deletes the oldest two.
        const fs::path dir = make_dir();
        const fs::path d3 = dated(dir, local_date_days_ago(3));
        const fs::path d2 = dated(dir, local_date_days_ago(2));
        const fs::path d1 = dated(dir, local_date_days_ago(1));
        for (const auto& p : {d3, d2, d1}) write_file(p, 600 * 1024);
        const fs::path yaml = dir / "config.yaml";
        std::ofstream(yaml) << "logging:\n  directory: " << dir.string()
                            << "\n  retention_days: 0\n  max_total_size_mb: 1\n";
        ::unsetenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB");
        ::unsetenv("ALPACAHTTP_LOG_DIRECTORY");
        ::unsetenv("ALPACAHTTP_LOG_RETENTION_DAYS");
        alpacahttp::Config config;
        EXPECT(config.load(yaml.string()));
        util::init_logging(config);
        EXPECT(!fs::exists(d3));
        EXPECT(!fs::exists(d2));
        EXPECT(fs::exists(d1));
        EXPECT(fs::exists(yaml));  // not a log file
        util::set_log_max_total_bytes(0);
        fs::remove_all(dir);
    }

    {
        // case: a rotation shifts only the parts below the first missing number
        // Parts .1 and .2 exist, .3 was deleted from the viewer, .4 is older: .4 stays put and nothing is overwritten.
        const fs::path dir = make_dir();
        const fs::path file = dir / util::current_log_filename();
        std::ofstream(dir / part_name(1)) << "was 1\n";
        std::ofstream(dir / part_name(2)) << "was 2\n";
        std::ofstream(dir / part_name(4)) << "was 4\n";
        std::ofstream(file) << std::string(5000, 'x') << '\n';
        start(dir, 4096);
        util::log_info("after rotation");
        EXPECT(read_all(dir / part_name(1)).size() == 5001);
        EXPECT(read_all(dir / part_name(2)) == "was 1\n");
        EXPECT(read_all(dir / part_name(3)) == "was 2\n");
        EXPECT(read_all(dir / part_name(4)) == "was 4\n");
        EXPECT(!fs::exists(dir / part_name(5)));
        EXPECT(read_all(file).find("after rotation") != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: read_log_file succeeds on a rotated file
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 4096);
        write_lines(500);
        const std::string text = util::read_log_file(util::current_log_filename());
        EXPECT(text == read_all(file));
        fs::remove_all(dir);
    }

    {
        // case: age pruning removes old dated files and their parts, and keeps today's
        const fs::path dir = make_dir();
        const fs::path old_file = dir / "alpacabridge-2000-01-01.log";
        const fs::path old_part = dir / "alpacabridge-2000-01-01.2.log";
        std::ofstream(old_file) << "old\n";
        std::ofstream(old_part) << "old\n";
        const fs::path file = start(dir, 4096);
        write_lines(500);
        util::set_log_retention_days(90);
        EXPECT(util::prune_old_log_files() == 2);
        EXPECT(!fs::exists(old_file));
        EXPECT(!fs::exists(old_part));
        EXPECT(fs::exists(file));
        EXPECT(fs::exists(dir / part_name(1)));
        util::set_log_retention_days(0);
        fs::remove_all(dir);
    }

    {
        // case: logging.max_file_size_mb from yaml, env, 0 = off, bad values keep the default
        std::string path_template = temp_template("alpacahttp_log_cap_cfg");
        const int fd = ::mkstemp(path_template.data());
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string path = path_template;
        const auto loaded = [&path](const char* value) {
            std::ofstream out(path);
            out << "logging:\n";
            if (*value != '\0') out << "  max_file_size_mb: " << value << "\n";
            out.close();
            alpacahttp::Config c;
            EXPECT(c.load(path));
            return c.log_max_file_size_mb();
        };
        ::unsetenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB");
        EXPECT(loaded("") == 5);
        EXPECT(loaded("25") == 25);
        EXPECT(loaded("0") == 0);
        EXPECT(loaded("-5") == 5);
        EXPECT(loaded("abc") == 5);
        EXPECT(loaded("99999999999999999999") == 5);

        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "3", 1);
        EXPECT(loaded("25") == 3);
        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "0", 1);
        EXPECT(loaded("25") == 0);
        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "-1", 1);
        EXPECT(loaded("25") == 25);
        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "junk", 1);
        EXPECT(loaded("") == 5);
        ::unsetenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB");
        ::unlink(path.c_str());
    }

    {
        // case: logging.max_total_size_mb from yaml, env, 0 = off, bad values keep the default
        std::string path_template = temp_template("alpacahttp_log_total_cfg");
        const int fd = ::mkstemp(path_template.data());
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string path = path_template;
        const auto loaded = [&path](const char* value) {
            std::ofstream out(path);
            out << "logging:\n";
            if (*value != '\0') out << "  max_total_size_mb: " << value << "\n";
            out.close();
            alpacahttp::Config c;
            EXPECT(c.load(path));
            return c.log_max_total_size_mb();
        };
        ::unsetenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB");
        EXPECT(loaded("") == 1024);
        EXPECT(loaded("4096") == 4096);
        EXPECT(loaded("0") == 0);
        EXPECT(loaded("-5") == 1024);
        EXPECT(loaded("abc") == 1024);
        EXPECT(loaded("99999999999999999999") == 1024);

        ::setenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB", "512", 1);
        EXPECT(loaded("4096") == 512);
        ::setenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB", "0", 1);
        EXPECT(loaded("4096") == 0);
        ::setenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB", "-1", 1);
        EXPECT(loaded("4096") == 4096);
        ::setenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB", "junk", 1);
        EXPECT(loaded("") == 1024);
        ::unsetenv("ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB");
        ::unlink(path.c_str());
    }

    {
        // case: start-up warns when the total cap is smaller than the per-file cap
        const fs::path dir = make_dir();
        const std::string text = init_from_yaml(dir, 10, 4);
        EXPECT(text.find(kTotalBelowFileWarning) != std::string::npos);
        EXPECT(text.find("logging.max_total_size_mb (4 MiB)") != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: start-up does not warn when the total cap is off or at least the per-file cap
        for (const int total : {0, 10, 1024}) {
            const fs::path dir = make_dir();
            const std::string text = init_from_yaml(dir, 10, total);
            EXPECT(text.find("File logging active") != std::string::npos);
            EXPECT(text.find(kTotalBelowFileWarning) == std::string::npos);
            fs::remove_all(dir);
        }
        util::set_log_max_total_bytes(0);
    }

    return 0;
}

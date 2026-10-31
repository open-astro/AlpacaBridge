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

#include <alpacahttp/config.h>
#include <alpacahttp/util/logging_adapter.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
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

fs::path make_dir() {
    char tmpl[] = "/tmp/alpacahttp_log_cap_XXXXXX";
    EXPECT(::mkdtemp(tmpl) != nullptr);
    return tmpl;
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

// Points the sink at `dir`, silences stderr, and sets the cap before the file
// is opened (as init_logging does), so a reopened file is judged against it.
fs::path start(const fs::path& dir, std::uintmax_t cap) {
    alpacahttp::Config config;
    config.set_file_logging_enabled(false);
    config.set_log_level(alpacahttp::LogLevel::INFO);
    util::init_logging(config);
    util::set_external_log_sink([](alpacacore::logging::LogLevel, std::string_view, std::string_view) {});
    util::set_log_max_file_bytes(cap);
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
        const fs::path dir = make_dir();
        start(dir, 4096);
        write_lines(2000);
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
        start(dir, 4096);
        write_lines(2000);
        EXPECT(util::is_valid_log_filename(part_name(1)));
        EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.0.log"));
        EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.19.log"));
        EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.01.log"));
        EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.100.log"));
        const auto files = util::list_log_files();
        EXPECT(files.size() == static_cast<std::size_t>(util::kMaxRotatedLogFiles) + 1);
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
        char path_template[] = "/tmp/alpacahttp_log_cap_cfg_XXXXXX";
        const int fd = ::mkstemp(path_template);
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

    return 0;
}

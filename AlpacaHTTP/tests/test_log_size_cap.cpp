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

// Per-day log file size cap (logging.max_file_size_mb): the sink stops writing
// to a day's file at the cap, with one marker line inside the cap.

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
const char* const kMarker = "Log file size cap reached";

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

void write_lines(int count) {
    for (int i = 0; i < count; ++i) {
        util::log_info("line " + std::to_string(i) + " padding padding padding padding");
    }
}

}  // namespace

int main() {
    {
        // case: the file stays within the cap and ends with the marker
        const fs::path dir = make_dir();
        const std::uintmax_t cap = 4096;
        const fs::path file = start(dir, cap);
        write_lines(500);
        EXPECT(fs::file_size(file) <= cap);
        EXPECT(last_line(read_all(file)).find(kMarker) != std::string::npos);
        const std::string text = read_all(file);
        EXPECT(text.find(kMarker) == text.rfind(kMarker));  // one marker only
        fs::remove_all(dir);
    }

    {
        // case: cap 0 means no limit
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 0);
        write_lines(500);
        EXPECT(fs::file_size(file) > 4096);
        EXPECT(read_all(file).find(kMarker) == std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: deleting today's file resets the cap
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
        // case: reopening an over-cap file appends nothing, not even a marker
        const fs::path dir = make_dir();
        const fs::path file = dir / util::current_log_filename();
        {
            std::ofstream out(file);
            out << std::string(8192, 'x') << '\n';
        }
        const auto before = fs::file_size(file);
        start(dir, 4096);
        write_lines(50);
        EXPECT(fs::file_size(file) == before);
        fs::remove_all(dir);
    }

    {
        // case: a restart with a file already holding the marker adds no second marker
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 4096);
        write_lines(500);
        const auto capped_size = fs::file_size(file);
        start(dir, 4096);  // reconfigure reopens the same file
        write_lines(50);
        EXPECT(fs::file_size(file) == capped_size);
        fs::remove_all(dir);
    }

    {
        // case: the default cap fits what the viewer and archive will read
        alpacahttp::Config config;
        EXPECT(config.log_max_file_size_mb() == 10);
        EXPECT(static_cast<std::uintmax_t>(config.log_max_file_size_mb()) * kMiB <= util::kMaxLogFileReadBytes);
    }

    {
        // case: read_log_file succeeds on a capped file
        const fs::path dir = make_dir();
        const fs::path file = start(dir, 4096);
        write_lines(500);
        const std::string text = util::read_log_file(util::current_log_filename());
        EXPECT(text == read_all(file));
        EXPECT(last_line(text).find(kMarker) != std::string::npos);
        fs::remove_all(dir);
    }

    {
        // case: age pruning still removes old dated files and keeps today's
        const fs::path dir = make_dir();
        const fs::path old_file = dir / "alpacabridge-2000-01-01.log";
        std::ofstream(old_file) << "old\n";
        const fs::path file = start(dir, 4096);
        write_lines(5);
        util::set_log_retention_days(90);
        EXPECT(util::prune_old_log_files() == 1);
        EXPECT(!fs::exists(old_file));
        EXPECT(fs::exists(file));
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
        EXPECT(loaded("") == 10);
        EXPECT(loaded("25") == 25);
        EXPECT(loaded("0") == 0);
        EXPECT(loaded("-5") == 10);
        EXPECT(loaded("abc") == 10);
        EXPECT(loaded("99999999999999999999") == 10);

        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "3", 1);
        EXPECT(loaded("25") == 3);
        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "0", 1);
        EXPECT(loaded("25") == 0);
        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "-1", 1);
        EXPECT(loaded("25") == 25);
        ::setenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB", "junk", 1);
        EXPECT(loaded("") == 10);
        ::unsetenv("ALPACAHTTP_LOG_MAX_FILE_SIZE_MB");
        ::unlink(path.c_str());
    }

    return 0;
}

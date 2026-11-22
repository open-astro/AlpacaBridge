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
// Every case that logs runs in on_one_day(), so a run across local midnight
// does not fail (#2442).

#include <alpacahttp/config.h>
#include <alpacahttp/util/logging_adapter.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
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

// The local date of @p at, YYYY-MM-DD, as the logger names its files.
std::string local_date(std::chrono::system_clock::time_point at) {
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}

// The local date one case runs on, read once before the case seeds or logs
// anything. The logger names a line's file by the date of the write, so a case
// names every file it checks from this date, never from the date at check time:
// merge-queue run 44640615583 crossed midnight inside a case and then looked
// for the day's parts under the next day's name (#2442).
class Day {
public:
    // Today's file (n = 0) or its part n: alpacabridge-YYYY-MM-DD[.N].log.
    std::string name(int n = 0) const { return dated({}, date_, n).string(); }
    fs::path file(const fs::path& dir, int n = 0) const { return dated(dir, date_, n); }
    // The local date @p days before this day (after it for a negative count).
    std::string days_ago(int days) const { return local_date(at_ - std::chrono::hours(24) * days); }
    // True once the local date is not this day: the case's lines may be split
    // across two days' files, so its checks cannot judge them.
    bool ended() const { return local_date(std::chrono::system_clock::now()) != date_; }

private:
    std::chrono::system_clock::time_point at_ = std::chrono::system_clock::now();
    std::string date_ = local_date(at_);
};

// Runs @p body(dir, day) in a fresh directory. The body checks day.ended()
// after its last line is logged and returns false if the day ended: midnight
// fell inside the case, so it runs again on the new day. One run of a case
// cannot cross two midnights. Nothing may log after the last ended() check,
// and the check relies on the sink writing each line before log_info returns.
template <typename Body>
void on_one_day(const Body& body) {
    bool judged = false;
    for (int run = 0; run < 2 && !judged; ++run) {
        const Day day;
        const fs::path dir = make_dir();
        judged = body(dir, day);
        fs::remove_all(dir);
        if (!judged) std::fprintf(stderr, "local midnight fell inside a case; running it again\n");
    }
    EXPECT(judged);
}

// Points the sink at `dir`, silences stderr, and sets the caps before the file
// is opened (as init_logging does), so a reopened file is judged against them.
// The total cap is off unless a case sets it. Returns @p day's file.
fs::path start(const fs::path& dir, const Day& day, std::uintmax_t cap, std::uintmax_t total_cap = 0) {
    alpacahttp::Config config;
    config.set_file_logging_enabled(false);
    config.set_log_level(alpacahttp::LogLevel::INFO);
    util::init_logging(config);
    util::set_external_log_sink([](alpacacore::logging::LogLevel, std::string_view, std::string_view) {});
    util::set_log_max_file_bytes(cap);
    util::set_log_max_total_bytes(total_cap);
    EXPECT(util::configure_log_directory(dir.string(), true) == dir.string());
    return day.file(dir);
}

void write_lines(int count) {
    for (int i = 0; i < count; ++i) {
        util::log_info("line " + std::to_string(i) + " padding padding padding padding");
    }
}

// Starts logging through init_logging from a yaml file, as the server does, with
// file logging in @p dir and the two caps set.
void start_from_yaml(const fs::path& dir, int max_file_size_mb, int max_total_size_mb) {
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
}

constexpr const char* kTotalBelowFileWarning = "smaller than logging.max_file_size_mb";

}  // namespace

int main() {
    {
        // case: at the cap the file rotates and logging goes on; nothing is dropped
        on_one_day([](const fs::path& dir, const Day& day) {
            const std::uintmax_t cap = 4096;
            const fs::path file = start(dir, day, cap);
            write_lines(500);
            util::log_info("after the cap");
            if (day.ended()) return false;
            EXPECT(fs::file_size(file) <= cap);
            EXPECT(last_line(read_all(file)).find("after the cap") != std::string::npos);
            EXPECT(fs::exists(day.file(dir, 1)));
            EXPECT(fs::file_size(day.file(dir, 1)) <= cap);
            // Consecutive: the newest part ends right before the current file starts.
            const std::string current = read_all(file);
            const std::string part1 = read_all(day.file(dir, 1));
            const auto first_line = current.substr(0, current.find('\n'));
            const auto n_first = std::stoi(first_line.substr(first_line.find("line ") + 5));
            EXPECT(last_line(part1).find("line " + std::to_string(n_first - 1) + " ") != std::string::npos);
            return true;
        });
    }

    {
        // case: only the newest kMaxRotatedLogFiles parts of a day are kept
        // A 1-byte cap rotates on every line after the first.
        on_one_day([](const fs::path& dir, const Day& day) {
            start(dir, day, 1);
            write_lines(util::kMaxRotatedLogFiles + 5);
            if (day.ended()) return false;
            for (int i = 1; i <= util::kMaxRotatedLogFiles; ++i) {
                EXPECT(fs::exists(day.file(dir, i)));
            }
            EXPECT(!fs::exists(day.file(dir, util::kMaxRotatedLogFiles + 1)));
            return true;
        });
    }

    {
        // case: cap 0 means no limit and no rotation
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path file = start(dir, day, 0);
            write_lines(500);
            if (day.ended()) return false;
            EXPECT(fs::file_size(file) > 4096);
            EXPECT(!fs::exists(day.file(dir, 1)));
            return true;
        });
    }

    {
        // case: deleting today's file, then logging, recreates it
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path file = start(dir, day, 4096);
            // A file opened on the next day leaves no file of this day to delete.
            if (day.ended()) return false;
            write_lines(500);
            util::delete_log_file(day.name());
            util::log_info("after delete");
            if (day.ended()) return false;
            EXPECT(fs::exists(file));
            EXPECT(read_all(file).find("after delete") != std::string::npos);
            return true;
        });
    }

    {
        // case: reopening an over-cap file rotates it on the next line instead of dropping lines
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path file = day.file(dir);
            {
                std::ofstream out(file);
                out << std::string(8192, 'x') << '\n';
            }
            start(dir, day, 4096);
            util::log_info("after restart");
            if (day.ended()) return false;
            EXPECT(fs::file_size(day.file(dir, 1)) == 8193);
            EXPECT(read_all(file).find("after restart") != std::string::npos);
            return true;
        });
    }

    {
        // case: a single line longer than the cap is still written
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path file = start(dir, day, 4096);
            util::log_info(std::string(6000, 'y'));
            if (day.ended()) return false;
            EXPECT(read_all(file).find(std::string(6000, 'y')) != std::string::npos);
            return true;
        });
    }

    {
        // case: parts are log files the viewer lists (newest first), reads and deletes
        on_one_day([](const fs::path& dir, const Day& day) {
            start(dir, day, 1);
            write_lines(13);  // 12 rotations: parts .1 .. .12
            if (day.ended()) return false;
            EXPECT(util::is_valid_log_filename(day.name(1)));
            EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.0.log"));
            EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.19.log"));
            EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.100.log"));
            EXPECT(util::is_valid_log_filename("alpacabridge-2026-01-01.999.log"));
            EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.01.log"));
            EXPECT(!util::is_valid_log_filename("alpacabridge-2026-01-01.1000.log"));
            const auto files = util::list_log_files();
            EXPECT(files.size() == 13);
            EXPECT(files[0].name == day.name());
            for (std::size_t i = 1; i < files.size(); ++i) {
                EXPECT(files[i].name == day.name(static_cast<int>(i)));  // .1, .2, ... .10 in number order
            }
            EXPECT(util::read_log_file(day.name(1)) == read_all(day.file(dir, 1)));
            util::delete_log_file(day.name(1));
            EXPECT(!fs::exists(day.file(dir, 1)));
            return true;
        });
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
        on_one_day([](const fs::path& dir, const Day& day) {
            const std::string d3 = day.days_ago(3);
            const std::string d2 = day.days_ago(2);
            for (const auto& date : {d3, d2}) {
                for (int n = 0; n <= 3; ++n) write_file(dated(dir, date, n), 1000);
            }
            const std::uintmax_t total_cap = 8000;
            const fs::path file = start(dir, day, 4096, total_cap);
            for (int i = 0; i < 1000 && !fs::exists(day.file(dir, 1)); ++i) write_lines(1);
            if (day.ended()) return false;
            EXPECT(fs::file_size(day.file(dir, 1)) > 4000);
            for (int n = 0; n <= 3; ++n) EXPECT(!fs::exists(dated(dir, d3, n)));
            EXPECT(!fs::exists(dated(dir, d2, 3)));
            EXPECT(fs::exists(dated(dir, d2, 2)));
            EXPECT(fs::exists(dated(dir, d2, 1)));
            EXPECT(fs::exists(dated(dir, d2)));  // the evening before stays while today grows
            EXPECT(fs::exists(day.file(dir, 1)));
            EXPECT(fs::exists(file));
            // Every later rotation brings the other files back under the cap; only the open file has grown since.
            write_lines(400);
            if (day.ended()) return false;
            EXPECT(dir_bytes(dir) - fs::file_size(file) <= total_cap);
            EXPECT(fs::exists(day.file(dir, 1)));
            EXPECT(fs::exists(file));
            return true;
        });
    }

    {
        // case: the day being written is the newest, whatever dates other files carry
        // A file dated after today (the clock jumped ahead once, then came back) sorts after today by name. The
        // session being written is still the newest, so the total cap deletes the future-dated file before today's
        // parts; by name order alone every rotation would delete today's part .1 and keep the future file forever.
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path older = dated(dir, day.days_ago(2));
            const fs::path future = dated(dir, day.days_ago(-400));
            write_file(older, 1000);
            write_file(future, 6000);
            const fs::path file = start(dir, day, 4096, 8000);
            for (int i = 0; i < 1000 && !fs::exists(day.file(dir, 1)); ++i) write_lines(1);
            if (day.ended()) return false;
            EXPECT(fs::exists(day.file(dir, 1)));
            EXPECT(!fs::exists(older));
            EXPECT(!fs::exists(future));
            EXPECT(fs::exists(file));
            return true;
        });
    }

    {
        // case: the file being written counts toward the total cap
        // Yesterday's 1000 bytes alone fit 3000; with today's open file (about 2600 bytes) they do not.
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path yesterday = dated(dir, day.days_ago(1));
            write_file(yesterday, 1000);
            const fs::path file = start(dir, day, 0, 3000);
            write_lines(30);
            if (day.ended()) return false;
            EXPECT(fs::file_size(file) > 2000);
            EXPECT(util::prune_old_log_files() == 1);
            EXPECT(!fs::exists(yesterday));
            EXPECT(fs::exists(file));
            return true;
        });
    }

    {
        // case: the file being written is never deleted, even when it alone is over the total cap
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path yesterday = dated(dir, day.days_ago(1));
            write_file(yesterday, 1000);
            const fs::path file = start(dir, day, 0, 100);
            write_lines(50);
            if (day.ended()) return false;
            EXPECT(util::prune_old_log_files() == 1);
            EXPECT(!fs::exists(yesterday));
            EXPECT(fs::exists(file));
            const std::string text = read_all(file);
            EXPECT(text.find("line 0 ") != std::string::npos);
            EXPECT(text.find("line 49 ") != std::string::npos);
            return true;
        });
    }

    {
        // case: total cap 0 keeps every file
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path old_file = dated(dir, day.days_ago(3));
            const fs::path old_part = dated(dir, day.days_ago(3), 1);
            write_file(old_file, 100000);
            write_file(old_part, 100000);
            start(dir, day, 4096, 0);
            write_lines(500);
            if (day.ended()) return false;
            EXPECT(util::prune_old_log_files() == 0);
            EXPECT(fs::exists(old_file));
            EXPECT(fs::exists(old_part));
            EXPECT(fs::exists(day.file(dir, 1)));
            return true;
        });
    }

    {
        // case: start-up applies logging.max_total_size_mb from the config file, oldest day first
        // Three earlier days of 600 KiB each are over 1 MiB: init_logging deletes the oldest two.
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path d3 = dated(dir, day.days_ago(3));
            const fs::path d2 = dated(dir, day.days_ago(2));
            const fs::path d1 = dated(dir, day.days_ago(1));
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
            if (day.ended()) return false;
            EXPECT(!fs::exists(d3));
            EXPECT(!fs::exists(d2));
            EXPECT(fs::exists(d1));
            EXPECT(fs::exists(yaml));  // not a log file
            return true;
        });
        util::set_log_max_total_bytes(0);
    }

    {
        // case: a rotation shifts only the parts below the first missing number
        // Parts .1 and .2 exist, .3 was deleted from the viewer, .4 is older: .4 stays put and nothing is overwritten.
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path file = day.file(dir);
            std::ofstream(day.file(dir, 1)) << "was 1\n";
            std::ofstream(day.file(dir, 2)) << "was 2\n";
            std::ofstream(day.file(dir, 4)) << "was 4\n";
            std::ofstream(file) << std::string(5000, 'x') << '\n';
            start(dir, day, 4096);
            util::log_info("after rotation");
            if (day.ended()) return false;
            EXPECT(read_all(day.file(dir, 1)).size() == 5001);
            EXPECT(read_all(day.file(dir, 2)) == "was 1\n");
            EXPECT(read_all(day.file(dir, 3)) == "was 2\n");
            EXPECT(read_all(day.file(dir, 4)) == "was 4\n");
            EXPECT(!fs::exists(day.file(dir, 5)));
            EXPECT(read_all(file).find("after rotation") != std::string::npos);
            return true;
        });
    }

    {
        // case: read_log_file succeeds on a rotated file
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path file = start(dir, day, 4096);
            write_lines(500);
            if (day.ended()) return false;
            const std::string text = util::read_log_file(day.name());
            EXPECT(text == read_all(file));
            return true;
        });
    }

    {
        // case: age pruning removes old dated files and their parts, and keeps today's
        // Logged every day from 5 days ago to today, plus a file and its part from 7 days ago: no gap is longer than
        // the retention, so the cutoff counts back from today (#2292).
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path old_file = dir / ("alpacabridge-" + day.days_ago(7) + ".log");
            const fs::path old_part = dir / ("alpacabridge-" + day.days_ago(7) + ".2.log");
            std::ofstream(old_file) << "old\n";
            std::ofstream(old_part) << "old\n";
            std::vector<fs::path> recent;
            for (int days = 1; days <= 5; ++days) {
                recent.push_back(dir / ("alpacabridge-" + day.days_ago(days) + ".log"));
                std::ofstream(recent.back()) << "recent\n";
            }
            const fs::path file = start(dir, day, 4096);
            write_lines(500);
            if (day.ended()) return false;
            util::set_log_retention_days(5);
            EXPECT(util::prune_old_log_files() == 2);
            EXPECT(!fs::exists(old_file));
            EXPECT(!fs::exists(old_part));
            EXPECT(fs::exists(file));
            EXPECT(fs::exists(day.file(dir, 1)));
            for (const auto& kept : recent) EXPECT(fs::exists(kept));
            util::set_log_retention_days(0);
            return true;
        });
    }

    {
        // case: a clock set far ahead does not prune the logs the server kept before the jump (#2292)
        // Files from 2000 stand for the history before a client's far-future synctime: to this test today is that
        // future. The cutoff counts back from the day after the newest earlier file, so only the files older than
        // the retention before it go.
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path older = dir / "alpacabridge-2000-01-01.log";
            const fs::path newest = dir / "alpacabridge-2000-01-10.log";
            const fs::path newest_part = dir / "alpacabridge-2000-01-10.2.log";
            std::ofstream(older) << "older\n";
            std::ofstream(newest) << "newest\n";
            std::ofstream(newest_part) << "newest\n";
            const fs::path file = start(dir, day, 4096);
            write_lines(10);
            if (day.ended()) return false;
            util::set_log_retention_days(5);
            EXPECT(util::prune_old_log_files() == 1);
            EXPECT(!fs::exists(older));
            EXPECT(fs::exists(newest));
            EXPECT(fs::exists(newest_part));
            EXPECT(fs::exists(file));
            util::set_log_retention_days(0);
            return true;
        });
    }

    {
        // case: the logs before a clock jump survive the days after it, not only its first day (#2292)
        // The jump came yesterday: yesterday's and today's logs follow the files from 2000. Two days of newer logs
        // count as following 2000-01-10, so with a 5-day retention only the file from 2000-01-01 goes.
        on_one_day([](const fs::path& dir, const Day& day) {
            const fs::path older = dir / "alpacabridge-2000-01-01.log";
            const fs::path newest_before = dir / "alpacabridge-2000-01-10.log";
            const fs::path yesterday = dir / ("alpacabridge-" + day.days_ago(1) + ".log");
            std::ofstream(older) << "older\n";
            std::ofstream(newest_before) << "before the jump\n";
            std::ofstream(yesterday) << "after the jump\n";
            const fs::path file = start(dir, day, 4096);
            write_lines(10);
            if (day.ended()) return false;
            util::set_log_retention_days(5);
            EXPECT(util::prune_old_log_files() == 1);
            EXPECT(!fs::exists(older));
            EXPECT(fs::exists(newest_before));
            EXPECT(fs::exists(yesterday));
            EXPECT(fs::exists(file));
            util::set_log_retention_days(0);
            return true;
        });
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
        on_one_day([](const fs::path& dir, const Day& day) {
            start_from_yaml(dir, 10, 4);
            if (day.ended()) return false;
            const std::string text = read_all(day.file(dir));
            EXPECT(text.find(kTotalBelowFileWarning) != std::string::npos);
            EXPECT(text.find("logging.max_total_size_mb (4 MiB)") != std::string::npos);
            return true;
        });
    }

    {
        // case: start-up does not warn when the total cap is off or at least the per-file cap
        for (const int total : {0, 10, 1024}) {
            on_one_day([total](const fs::path& dir, const Day& day) {
                start_from_yaml(dir, 10, total);
                if (day.ended()) return false;
                const std::string text = read_all(day.file(dir));
                EXPECT(text.find("File logging active") != std::string::npos);
                EXPECT(text.find(kTotalBelowFileWarning) == std::string::npos);
                return true;
            });
        }
        util::set_log_max_total_bytes(0);
    }

    return 0;
}

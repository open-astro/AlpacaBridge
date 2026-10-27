// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/util/atomic_file.h>
#include <sys/resource.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "catch2_compat.h"

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void spit(const fs::path& p, const std::string& text) {
    std::ofstream out(p);
    out << text;
}

// Makes any write past @p bytes fail with EFBIG; restores limit and SIGXFSZ.
struct FileSizeLimit {
    rlimit saved{};
    void (*saved_handler)(int);
    explicit FileSizeLimit(rlim_t bytes) {
        getrlimit(RLIMIT_FSIZE, &saved);
        saved_handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit lim = saved;
        lim.rlim_cur = bytes;
        setrlimit(RLIMIT_FSIZE, &lim);
    }
    ~FileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, saved_handler);
    }
};

}  // namespace

TEST_CASE("write_file_atomic - replaces the file and leaves no temp", "[atomic-file][unit]") {
    const fs::path dir = fs::temp_directory_path() / "atomic_file_ok";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path target = dir / "state.json";
    spit(target, "old");
    alpacacore::util::write_file_atomic(target, "new content");
    CHECK(slurp(target) == "new content");
    CHECK_FALSE(fs::exists(target.string() + ".tmp"));
    fs::remove_all(dir);
}

TEST_CASE("write_file_atomic - a failed write keeps the old file and removes the temp", "[atomic-file][unit]") {
    const fs::path dir = fs::temp_directory_path() / "atomic_file_fail";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path target = dir / "state.json";
    spit(target, "old");
    {
        FileSizeLimit limit(8);
        CHECK_THROWS(alpacacore::util::write_file_atomic(target, std::string(4096, 'x')));
    }
    CHECK(slurp(target) == "old");
    CHECK_FALSE(fs::exists(target.string() + ".tmp"));
    fs::remove_all(dir);
}

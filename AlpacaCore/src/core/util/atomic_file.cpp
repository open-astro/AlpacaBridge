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

#include "alpacacore/util/atomic_file.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace alpacacore::util {

namespace {

// fsync of a file or directory opened read-only (the directory entry of the rename needs it).
void fsync_path(const std::filesystem::path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("cannot open " + p.string() + " for fsync: " + std::strerror(errno));
    }
    const int rc = ::fsync(fd);
    const int err = errno;
    ::close(fd);
    if (rc != 0) {
        throw std::runtime_error("fsync failed for " + p.string() + ": " + std::strerror(err));
    }
}

}  // namespace

void write_file_atomic(const std::filesystem::path& path, std::string_view content) {
    const std::filesystem::path temp = path.string() + ".tmp";
    try {
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("cannot open " + temp.string() + " for writing");
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.flush();
            if (!out) {
                throw std::runtime_error("write failed for " + temp.string());
            }
        }
        fsync_path(temp);
        // The rename swaps the inode: carry over the mode an operator or the package set.
        std::error_code ec;
        const auto existing = std::filesystem::status(path, ec);
        if (!ec && std::filesystem::is_regular_file(existing)) {
            std::filesystem::permissions(temp, existing.permissions(), ec);
        }
        std::filesystem::rename(temp, path);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw;
    }
    const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    fsync_path(dir);
}

}  // namespace alpacacore::util

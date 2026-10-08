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

#pragma once

#include <alpacacore/util/rotator_sync_offset_store.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace alpacacore::test {

// Points the sync-offset store at a fresh temp directory for one test and
// restores the previous default path and removes the directory on exit.
class TempSyncOffsetDir {
public:
    TempSyncOffsetDir() {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("rotator_sync_offsets_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(dir_);
        previous_ = util::RotatorSyncOffsetStore::set_default_path(file().string());
    }
    ~TempSyncOffsetDir() {
        util::RotatorSyncOffsetStore::set_default_path(previous_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempSyncOffsetDir(const TempSyncOffsetDir&) = delete;
    TempSyncOffsetDir& operator=(const TempSyncOffsetDir&) = delete;
    std::filesystem::path file() const { return dir_ / "state" / "rotator_sync_offsets.json"; }

private:
    std::filesystem::path dir_;
    std::string previous_;
};

}  // namespace alpacacore::test

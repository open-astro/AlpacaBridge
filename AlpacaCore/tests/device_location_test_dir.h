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

#include <alpacacore/util/device_location_store.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace alpacacore::test {

// Points the store at a fresh temp directory for one test and restores it on exit.
class TempLocationDir {
public:
    TempLocationDir() {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("device_locations_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(dir_);
        previous_ = util::DeviceLocationStore::set_default_path(file().string());
    }
    ~TempLocationDir() {
        util::DeviceLocationStore::set_default_path(previous_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempLocationDir(const TempLocationDir&) = delete;
    TempLocationDir& operator=(const TempLocationDir&) = delete;
    std::filesystem::path file() const { return dir_ / "state" / "device_locations.json"; }
    std::filesystem::path dir() const { return dir_; }

private:
    std::filesystem::path dir_;
    std::string previous_;
};

}  // namespace alpacacore::test

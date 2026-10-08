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

#include <alpacacore/util/logging.h>
#include <alpacacore/util/rotator_sync_offset_store.h>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "rotator_sync_offset_test_dir.h"

using alpacacore::util::RotatorSyncOffsetStore;

TEST_CASE("Rotator sync offset store - round trip", "[rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    CHECK(RotatorSyncOffsetStore::load("A") == 0.0);
    REQUIRE(RotatorSyncOffsetStore::save("A", 30.25));
    CHECK(RotatorSyncOffsetStore::load("A") == 30.25);
    REQUIRE(RotatorSyncOffsetStore::save("A", -12.5));
    CHECK(RotatorSyncOffsetStore::load("A") == -12.5);
}

TEST_CASE("Rotator sync offset store - two keys kept", "[rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    REQUIRE(RotatorSyncOffsetStore::save("WANDERERASTRO_ROTATOR_0", 30.0));
    REQUIRE(RotatorSyncOffsetStore::save("ZWO_CAA_SN_123", 90.5));
    CHECK(RotatorSyncOffsetStore::load("WANDERERASTRO_ROTATOR_0") == 30.0);
    CHECK(RotatorSyncOffsetStore::load("ZWO_CAA_SN_123") == 90.5);
    CHECK(RotatorSyncOffsetStore::load("other") == 0.0);
}

TEST_CASE("Rotator sync offset store - missing or corrupt file loads 0", "[rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    CHECK(RotatorSyncOffsetStore::load("A") == 0.0);

    std::filesystem::create_directories(dir.file().parent_path());
    for (const char* bad : {"", "not json", "{\"A\": ", "{\"A\": \"x\"}", "[1,2]", "{\"A\" 3}"}) {
        {
            std::ofstream(dir.file(), std::ios::trunc) << bad;
        }
        CHECK(RotatorSyncOffsetStore::load("A") == 0.0);
    }
    // A save over a corrupt file recovers.
    REQUIRE(RotatorSyncOffsetStore::save("A", 5.0));
    CHECK(RotatorSyncOffsetStore::load("A") == 5.0);
}

TEST_CASE("Rotator sync offset store - failed save returns false without throwing", "[rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    // A regular file where the parent directory should be.
    { std::ofstream(dir.file().parent_path().parent_path() / "state") << "x"; }
    bool ok = true;
    CHECK_NOTHROW(ok = RotatorSyncOffsetStore::save("A", 1.0));
    CHECK_FALSE(ok);
    CHECK(RotatorSyncOffsetStore::load("A") == 0.0);
}

TEST_CASE("Rotator sync offset store - unreadable file logs a warning", "[rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    { std::ofstream(dir.file(), std::ios::trunc) << "not json"; }

    std::vector<std::string> warnings;
    const auto previous = alpacacore::logging::get_log_sink();
    alpacacore::logging::set_log_sink(
        [&warnings](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn) {
                warnings.emplace_back(message);
            }
        });
    const double loaded = RotatorSyncOffsetStore::load("A");
    const bool saved = RotatorSyncOffsetStore::save("A", std::nan(""));
    alpacacore::logging::set_log_sink(previous);

    CHECK(loaded == 0.0);
    CHECK_FALSE(saved);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find(dir.file().string()) != std::string::npos);
    CHECK(warnings[1].find("non-finite") != std::string::npos);
}

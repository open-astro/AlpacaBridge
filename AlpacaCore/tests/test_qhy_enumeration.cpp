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

#include <alpacacore/vendor/qhy/detail/qhy_enumeration.h>

#include <algorithm>
#include <cstring>

#include "catch2_compat.h"

using namespace alpacacore::vendor::qhy;

namespace {

struct ScanLogs {
    alpacacore::logging::LogSink previous_sink{alpacacore::logging::get_log_sink()};
    alpacacore::logging::LogLevel previous_level{alpacacore::logging::get_log_level()};
    std::vector<std::pair<alpacacore::logging::LogLevel, std::string>> entries;

    ScanLogs() {
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Debug);
        alpacacore::logging::set_log_sink([this](auto level, auto component, auto message) {
            if (component == "QHY") {
                entries.emplace_back(level, message);
            }
        });
    }

    ~ScanLogs() {
        alpacacore::logging::set_log_sink(previous_sink);
        alpacacore::logging::set_log_level(previous_level);
    }

    bool contains(alpacacore::logging::LogLevel level, const std::string& text) const {
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.first == level && entry.second.find(text) != std::string::npos;
        });
    }
};

}  // namespace

TEST_CASE("QHY enumeration - zero and invalid scan results remain distinguishable", "[qhy][enumeration][unit]") {
    const uint32_t count = GENERATE(0U, 65U, 0xffffffffU);
    ScanLogs logs;
    int scans = 0;
    int id_calls = 0;
    int model_calls = 0;
    const auto cameras = detail::enumerate_cameras(
        [&] {
            ++scans;
            return count;
        },
        [&](uint32_t, char*) {
            ++id_calls;
            return 0U;
        },
        [&](char*, char*) {
            ++model_calls;
            return 0U;
        },
        2);
    CHECK(cameras.empty());
    CHECK(scans == 1);
    CHECK(id_calls == 0);
    CHECK(model_calls == 0);
    const auto level = count == 0 ? alpacacore::logging::LogLevel::Debug : alpacacore::logging::LogLevel::Warn;
    CHECK(logs.contains(level, "ScanQHYCCD returned " + detail::sdk_result(count)));
    CHECK(logs.contains(level, " ms; logical handle entries=2"));
    if (count == 0xffffffffU) {
        CHECK(logs.contains(level, "4294967295 (0xffffffff)"));
    }
}

TEST_CASE("QHY enumeration - ID failures do not hide usable cameras", "[qhy][enumeration][unit]") {
    ScanLogs logs;
    int id_calls = 0;
    int model_calls = 0;
    const auto cameras = detail::enumerate_cameras([] { return 3U; },
                                                   [&](uint32_t index, char* id) {
                                                       ++id_calls;
                                                       if (index != 1) {
                                                           return 0xffffffffU;
                                                       }
                                                       std::strcpy(id, "camera-b");
                                                       return 0U;
                                                   },
                                                   [&](char* id, char* model) {
                                                       ++model_calls;
                                                       CHECK(std::string(id) == "camera-b");
                                                       std::strcpy(model, "miniCam8M");
                                                       return 0U;
                                                   },
                                                   0);
    REQUIRE(cameras.size() == 1);
    CHECK(cameras[0].camera_id == "camera-b");
    CHECK(cameras[0].model == "miniCam8M");
    CHECK(id_calls == 3);
    CHECK(model_calls == 1);
    CHECK(logs.contains(alpacacore::logging::LogLevel::Warn, "GetQHYCCDId(0) failed: 4294967295 (0xffffffff)"));
    CHECK(logs.contains(alpacacore::logging::LogLevel::Warn, "GetQHYCCDId(2) failed:"));
    CHECK(logs.contains(alpacacore::logging::LogLevel::Debug, "1 usable camera IDs from 3 scan entries"));
}

TEST_CASE("QHY enumeration - all ID failures are not logged as zero detected", "[qhy][enumeration][unit]") {
    ScanLogs logs;
    int model_calls = 0;
    const auto cameras = detail::enumerate_cameras([] { return 1U; }, [](uint32_t, char*) { return 17U; },
                                                   [&](char*, char*) {
                                                       ++model_calls;
                                                       return 0U;
                                                   },
                                                   0);
    CHECK(cameras.empty());
    CHECK(model_calls == 0);
    CHECK(logs.contains(alpacacore::logging::LogLevel::Debug, "ScanQHYCCD returned 1 (0x1)"));
    CHECK(logs.contains(alpacacore::logging::LogLevel::Warn, "GetQHYCCDId(0) failed: 17 (0x11)"));
    CHECK(logs.contains(alpacacore::logging::LogLevel::Debug, "0 usable camera IDs from 1 scan entries"));
}

TEST_CASE("QHY enumeration - model failure preserves camera identity and ordering", "[qhy][enumeration][unit]") {
    ScanLogs logs;
    const auto cameras = detail::enumerate_cameras([] { return 2U; },
                                                   [](uint32_t index, char* id) {
                                                       std::strcpy(id, index == 0 ? "camera-a" : "camera-b");
                                                       return 0U;
                                                   },
                                                   [](char*, char*) { return 0xffffffffU; }, 0);
    REQUIRE(cameras.size() == 2);
    CHECK(cameras[0].camera_id == "camera-a");
    CHECK(cameras[0].model == "camera-a");
    CHECK(cameras[1].camera_id == "camera-b");
    CHECK(cameras[1].model == "camera-b");
    CHECK(logs.contains(alpacacore::logging::LogLevel::Debug,
                        "GetQHYCCDModel for scan index 1 failed: 4294967295 (0xffffffff)"));
}

TEST_CASE("QHY enumeration - the maximum accepted scan count is enumerated", "[qhy][enumeration][unit]") {
    int id_calls = 0;
    const auto cameras = detail::enumerate_cameras([] { return 64U; },
                                                   [&](uint32_t index, char* id) {
                                                       ++id_calls;
                                                       std::strcpy(id, ("camera-" + std::to_string(index)).c_str());
                                                       return 0U;
                                                   },
                                                   [](char*, char* model) {
                                                       std::strcpy(model, "QHY");
                                                       return 0U;
                                                   },
                                                   0);
    REQUIRE(cameras.size() == 64);
    CHECK(id_calls == 64);
    CHECK(cameras.front().camera_id == "camera-0");
    CHECK(cameras.back().camera_id == "camera-63");
}

TEST_CASE("QHY enumeration - unterminated SDK strings are bounded", "[qhy][enumeration][unit]") {
    const auto cameras = detail::enumerate_cameras([] { return 1U; },
                                                   [](uint32_t, char* id) {
                                                       std::memset(id, 'a', 64);
                                                       return 0U;
                                                   },
                                                   [](char* id, char* model) {
                                                       CHECK(std::strlen(id) == 63);
                                                       std::memset(model, 'b', 64);
                                                       return 0U;
                                                   },
                                                   0);
    REQUIRE(cameras.size() == 1);
    CHECK(cameras[0].camera_id == std::string(63, 'a'));
    CHECK(cameras[0].model == std::string(63, 'b'));
}

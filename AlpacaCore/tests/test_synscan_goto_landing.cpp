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

// open-astro#880, SynScan: the driver reports the handset's own position after
// a GOTO, and a handset that lands ~29" off in RA (EQM-35 Pro, 1.96 s of RA)
// fails ConformU's +/-10" SlewToCoordinates check. The driver re-issues the
// GOTO from the handset's own readback until the landing is inside tolerance.
// The fake lands every GOTO a fixed RA offset away from what it was told.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kCountsMask = 0xFFFFFF;
// 1.96 s of RA (the offset measured on the EQM-35 Pro) in 24-bit counts: 1.96 / 86400 * 2^24.
constexpr uint32_t kRaOffsetCounts = 381;

struct LandingOffsetHandset {
    std::atomic<uint32_t> ra_raw{0x400000};
    std::atomic<uint32_t> dec_raw{0x100000};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<bool> goto_stopped{false};
    // Each GOTO lands this many counts past the commanded RA; multiplied by the GOTO ordinal when `growing`.
    std::atomic<uint32_t> ra_offset_counts{kRaOffsetCounts};
    std::atomic<bool> growing{false};
    std::atomic<int> goto_ms{300};

    bool goto_in_progress() const {
        if (goto_count.load() == 0) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return !goto_stopped.load() && Clock::now() - started < std::chrono::milliseconds(goto_ms.load());
    }
};

std::string hex24(uint32_t raw) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X00", raw & kCountsMask);
    return buf;
}

alpacacore::test::FakeMountServer::Responder responder(const std::shared_ptr<LandingOffsetHandset>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
                return hex24(st->ra_raw.load()) + "," + hex24(st->dec_raw.load()) + "#";
            case 'r': {
                // "r" + 6 hex + "00" + "," + 6 hex + "00"
                if (chunk.size() < 18) return "";
                const uint32_t ra = static_cast<uint32_t>(std::strtoul(chunk.substr(1, 6).c_str(), nullptr, 16));
                const uint32_t dec = static_cast<uint32_t>(std::strtoul(chunk.substr(10, 6).c_str(), nullptr, 16));
                const int ordinal = st->goto_count.fetch_add(1) + 1;
                const uint32_t offset = st->ra_offset_counts.load() * (st->growing.load() ? ordinal : 1U);
                st->ra_raw.store((ra + offset) & kCountsMask);
                st->dec_raw.store(dec);
                st->goto_started.store(Clock::now().time_since_epoch().count());
                st->goto_stopped.store(false);
                return "#";
            }
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':
                st->goto_stopped.store(true);
                return "#";
            case 'P':
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 16 && chunk[4] == 0 && chunk[5] == 0) {
                    st->goto_stopped.store(true);
                }
                return "#";
            case 'T':
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(50)) + "#";
            case 'w':
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 1000;
    return info;
}

double ra_error_arcsec(alpacacore::TelescopeDriver& driver, double target_hours) {
    return std::abs(driver.get_right_ascension() - target_hours) * 15.0 * 3600.0;
}

double dec_error_arcsec(alpacacore::TelescopeDriver& driver, double target_degrees) {
    return std::abs(driver.get_declination() - target_degrees) * 3600.0;
}

constexpr double kTargetRa = 5.5;
constexpr double kTargetDec = 20.0;
// ConformU's SlewToCoordinates tolerance.
constexpr double kConformUToleranceArcsec = 10.0;

}  // namespace

TEST_CASE("SynScan GOTO landing - SlewToCoordinates refines a GOTO the handset lands 29 arcsec off (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    CHECK(st->goto_count.load() == 2);  // the GOTO plus one refinement
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - SlewToTarget refines the landing too (#880)", "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->set_target_right_ascension(kTargetRa);
    driver->set_target_declination(kTargetDec);
    REQUIRE_NOTHROW(driver->slew_to_target());

    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(st->goto_count.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async slew keeps Slewing true until the refinement lands (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (driver->get_slewing() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(driver->get_slewing());

    // Slewing fell only after the refinement GOTO, so the landing is final.
    CHECK(st->goto_count.load() == 2);
    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->growing.store(true);  // every GOTO lands further off than the last
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    try {
        driver->slew_to_coordinates(kTargetRa, kTargetDec);
        FAIL("Expected AlpacaException for an exhausted refinement");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(st->goto_count.load() == 4);  // the GOTO plus three refinement passes, no more
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->growing.store(true);  // every GOTO lands further off than the last
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    bool reported = false;
    while (!reported && Clock::now() < deadline) {
        try {
            if (!driver->get_slewing()) {
                break;
            }
        } catch (const alpacacore::AlpacaException& ex) {
            reported = ex.error_code() == alpacacore::AlpacaError::DriverException;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(reported);
    CHECK(st->goto_count.load() == 4);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - AbortSlew during a refinement pass stops it (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->growing.store(true);  // left alone, the refinement would go on to a third GOTO
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (st->goto_count.load() < 2 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(st->goto_count.load() == 2);

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_FALSE(driver->get_slewing());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->goto_count.load() == 2);
    driver->set_connected(false);
}

namespace {

// A GOTO that stays in progress for 6 s, started asynchronously. Every stop path below must return at once
// while the async slew task is still waiting on the handset (the Platform 7 Disconnect budget is 5 s).
constexpr auto kStopBudget = std::chrono::milliseconds(1500);

struct LongGoto {
    std::shared_ptr<LandingOffsetHandset> st = std::make_shared<LandingOffsetHandset>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    LongGoto() {
        st->goto_ms.store(6000);
        server = std::make_unique<alpacacore::test::FakeMountServer>(responder(st));
        REQUIRE(server->ok());
        driver = alpacacore::vendor::synscan::create_synscan_telescope(0, endpoint(server->port()),
                                                                       alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        REQUIRE(st->goto_count.load() == 1);
    }
};

}  // namespace

TEST_CASE("SynScan GOTO landing - Disconnect during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    g.driver->set_connected(false);
    CHECK(Clock::now() - t0 < kStopBudget);
}

TEST_CASE("SynScan GOTO landing - MoveAxis during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    try {
        g.driver->move_axis(0, 1.0);
    } catch (const alpacacore::AlpacaException&) {
        // the call may refuse the rate; only its latency is under test
    }
    CHECK(Clock::now() - t0 < kStopBudget);
    g.driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a second async slew replaces a waiting one at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    REQUIRE_NOTHROW(g.driver->slew_to_coordinates_async(kTargetRa + 1.0, kTargetDec));
    CHECK(Clock::now() - t0 < kStopBudget);
    g.driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - destroying the driver during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    g.driver.reset();
    CHECK(Clock::now() - t0 < kStopBudget);
}

#endif  // !_WIN32

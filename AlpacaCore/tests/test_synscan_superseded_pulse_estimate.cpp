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

// A pulse that a later command supersedes drove the mount for only part of its duration. PulseGuide publishes the
// full-duration displacement as the RightAscension/Declination estimate and holds it for the pulse, the completion
// delay and a 3 s grace; a command that supersedes the pulse must not leave that estimate in front of what the
// mount drove. The handset here reports the position a case sets: the start plus guide rate x the virtual time the
// pulse ran. Each case reads RA/Dec inside the grace window, on the fake task clock.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace sc = alpacacore::vendor::synscan;
using std::chrono::milliseconds;

constexpr milliseconds kBound{5000};
constexpr double kToleranceArcsec = 1.0;  // a 100 ms pulse moves the mount 0.75 arcsec; the cases differ by more

// A mount that stands where a case puts it, answering the precise `e` read in the V4 24-bit format.
struct MovingHandset {
    std::atomic<uint32_t> ra_raw{0};
    std::atomic<uint32_t> dec_raw{0};

    void set(double ra_hours, double dec_degrees) {
        ra_raw = static_cast<uint32_t>(std::llround(ra_hours / 24.0 * 16777216.0)) << 8;
        dec_raw = static_cast<uint32_t>(std::llround(dec_degrees / 360.0 * 16777216.0)) << 8;
    }

    alpacacore::test::FakeMountServer::Responder responder() {
        return [this](const std::string& chunk) -> std::string {
            if (chunk.empty()) return "0#";
            char buffer[32];
            switch (chunk[0]) {
                case 'K':
                    return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
                case 'e':
                case 'z':
                    std::snprintf(buffer, sizeof buffer, "%08X,%08X#", ra_raw.load(), dec_raw.load());
                    return buffer;
                case 'E':
                case 'Z':
                    return "1234,4000#";
                case 'L':
                    return "0#";
                case 'J':
                    return "0#";
                case 'h':
                    return std::string{3, 4, 5, 1, 2, 20, 0, 0} + "#";
                case 'w':
                    return std::string(8, '\0') + "#";
                case 'm':
                    return std::string(1, static_cast<char>(50)) + "#";
                case 't':
                    return std::string(1, '\x02') + "#";
                case 'W':
                case 'H':
                case 'R':
                case 'P':
                case 'M':
                case 'T':
                    return "#";
                default:
                    return "0#";
            }
        };
    }
};

double arcsec_ra(double hours) { return hours * 15.0 * 3600.0; }

struct Rig {
    alpacacore::test::FakeTaskClock clock;
    MovingHandset handset;
    alpacacore::test::FakeMountServer server{handset.responder()};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    double start_ra_hours = 6.0;
    double start_dec_degrees = 30.0;

    Rig() {
        REQUIRE(server.ok());
        handset.set(start_ra_hours, start_dec_degrees);
        sc::ConnectionInfo conn;
        conn.type = sc::ConnectionType::Network;
        conn.host = "127.0.0.1";
        conn.tcp_port = server.port();
        conn.response_timeout_ms = 5000;
        driver = sc::create_synscan_telescope_with_site(0, conn, sc::SynScanVersion::V4, -33.5, 151.0, std::nullopt,
                                                        false, sc::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::connect_and_park(*driver, clock, kBound));
        REQUIRE(driver->get_connected());
        sc::set_status_poll_paused_for_testing(*driver, true);
        sc::poll_status_now_for_testing(*driver);
        REQUIRE(std::abs(arcsec_ra(driver->get_right_ascension() - start_ra_hours)) < kToleranceArcsec);
        REQUIRE(std::abs(driver->get_declination() - start_dec_degrees) * 3600.0 < kToleranceArcsec);
    }
    ~Rig() {
        driver->set_connected(false);
        driver.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // What `ms` of pulse moves the mount, at the rate the driver reports.
    double guide_ra_hours(int ms) const { return driver->get_guide_rate().ra * ms / 1000.0 / 15.0; }
    double guide_dec_degrees(int ms) const { return driver->get_guide_rate().dec * ms / 1000.0; }

    void wait_waiters(std::size_t n) { REQUIRE(clock.wait_for_waiters(n, kBound)); }
    void advance(milliseconds ms) {
        clock.advance(ms);
        REQUIRE(clock.wait_for_woken_settled(kBound));
    }

    // The mount stands where the commands drove it; the handset is polled so the driver's fallback reads it.
    void mount_at(double ra_hours, double dec_degrees) {
        handset.set(ra_hours, dec_degrees);
        sc::poll_status_now_for_testing(*driver);
    }

    void check_ra(double driven_ra_hours) {
        mount_at(driven_ra_hours, start_dec_degrees);
        const double observed = driver->get_right_ascension();
        INFO("RA offset from start: observed " << arcsec_ra(observed - start_ra_hours) << " arcsec, mount drove "
                                               << arcsec_ra(driven_ra_hours - start_ra_hours) << " arcsec");
        CHECK(std::abs(arcsec_ra(observed - driven_ra_hours)) < kToleranceArcsec);
    }
};

}  // namespace

// Path 1: a newer pulse on the same axis. 200 ms of the 2000 ms East pulse ran, then 100 ms East: 300 ms driven.
TEST_CASE("SynScan superseded pulse estimate - a newer same-direction pulse leaves the driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);  // East: RA rises
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->pulse_guide(2, 100);
    rig.wait_waiters(2);
    rig.advance(milliseconds(100));
    rig.check_ra(rig.start_ra_hours + rig.guide_ra_hours(300));
}

// Path 1: the newer pulse runs the other way. 200 ms East then 100 ms West: 100 ms net driven.
TEST_CASE("SynScan superseded pulse estimate - a newer opposite-direction pulse leaves the driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);  // East
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->pulse_guide(3, 100);  // West
    rig.wait_waiters(2);
    rig.advance(milliseconds(100));
    rig.check_ra(rig.start_ra_hours + rig.guide_ra_hours(200) - rig.guide_ra_hours(100));
}

// Path 2: a pulse on the other axis supersedes nothing: the East pulse runs its full 2000 ms, so both axes end at
// the displacement they were driven.
TEST_CASE("SynScan superseded pulse estimate - an other-axis pulse leaves both axes at their driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);  // East
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->pulse_guide(0, 100);  // North
    rig.wait_waiters(3);
    rig.advance(milliseconds(1800));  // the East pulse ends at 2000 ms; the North pulse ended at 300 ms

    rig.mount_at(rig.start_ra_hours + rig.guide_ra_hours(2000), rig.start_dec_degrees + rig.guide_dec_degrees(100));
    const double ra = rig.driver->get_right_ascension();
    const double dec = rig.driver->get_declination();
    CHECK(std::abs(arcsec_ra(ra - rig.start_ra_hours - rig.guide_ra_hours(2000))) < kToleranceArcsec);
    CHECK(std::abs((dec - rig.start_dec_degrees - rig.guide_dec_degrees(100)) * 3600.0) < kToleranceArcsec);
}

// Path 3: MoveAxis on the pulsed axis, then stopped. 200 ms of the pulse ran.
TEST_CASE("SynScan superseded pulse estimate - MoveAxis on the pulsed axis leaves the driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->move_axis(0, 0.5);
    rig.driver->move_axis(0, 0.0);
    rig.check_ra(rig.start_ra_hours + rig.guide_ra_hours(200));
}

// Path 4: a slew started over the pulse, then aborted (the fake mount does not move for the slew).
TEST_CASE("SynScan superseded pulse estimate - a slew and AbortSlew over a pulse leave the driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->slew_to_coordinates_async(rig.start_ra_hours + 1.0, rig.start_dec_degrees);
    rig.driver->abort_slew();
    rig.check_ra(rig.start_ra_hours + rig.guide_ra_hours(200));
}

// Path 4: AbortSlew alone ends a running pulse.
TEST_CASE("SynScan superseded pulse estimate - AbortSlew over a pulse leaves the driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->abort_slew();
    rig.check_ra(rig.start_ra_hours + rig.guide_ra_hours(200));
}

// Path 5: Tracking switched off while the pulse runs.
TEST_CASE("SynScan superseded pulse estimate - Tracking off over a pulse leaves the driven displacement",
          "[synscan][pulse-guide][superseded-estimate]") {
    Rig rig;
    rig.driver->pulse_guide(2, 2000);
    rig.wait_waiters(2);
    rig.advance(milliseconds(200));
    rig.driver->set_tracking(false);
    rig.check_ra(rig.start_ra_hours + rig.guide_ra_hours(200));
}

#endif

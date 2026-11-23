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

// FakeIoptronMount in ConformU mode (#2614): the iOptron v3 wire answered from FakeMountMotionModel. The wire cases
// drive respond_to() on a hand-advanced clock; the driver case runs the real telescope driver over the fake's TCP
// port. The scripted default keeps its old answers (pinned in the first case).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_ioptron_mount.h"

namespace {

using alpacacore::test::FakeIoptronMount;
using alpacacore::test::FakeMountMotionModel;
using std::chrono::seconds;

// A fake in ConformU mode whose clock the case advances.
struct ModelledMount {
    std::shared_ptr<FakeMountMotionModel::TimePoint> now = std::make_shared<FakeMountMotionModel::TimePoint>(
        FakeMountMotionModel::TimePoint{seconds(1767225600)});  // 2026-01-01 00:00:00 UTC
    FakeIoptronMount mount;

    ModelledMount() : mount("0012", config(now)) {}
    explicit ModelledMount(double slew_deg_per_s) : mount("0012", config(now, slew_deg_per_s)) {}

    static FakeMountMotionModel::Config config(const std::shared_ptr<FakeMountMotionModel::TimePoint>& clock,
                                               double slew_deg_per_s = 6.0) {
        auto c = FakeIoptronMount::conformu_motion_config();
        c.slew_deg_per_s = slew_deg_per_s;
        c.clock = [clock] { return *clock; };
        return c;
    }
    void advance(seconds by) { *now += by; }
    std::string ask(const std::string& cmd) { return mount.respond_to(cmd); }

    // ":GLS#": sign, 16 site digits, then GPS, system status, rate, speed, time source and hemisphere digits.
    int status() { return ask(":GLS#")[18] - '0'; }
    // ":GEP#": Dec (sign + 8 digits), RA (9 digits), pier digit, pointing digit; both in 0.01 arcsec.
    double ra_hours() { return std::stod(ask(":GEP#").substr(9, 9)) / (15.0 * 3600.0 * 100.0); }
    double dec_degrees() { return std::stod(ask(":GEP#").substr(0, 9)) / (3600.0 * 100.0); }

    void set_target(double ra_hours, double dec_degrees) {
        char b[32];
        std::snprintf(b, sizeof(b), ":SRA%09lld#", std::llround(ra_hours * 15.0 * 3600.0 * 100.0));
        REQUIRE(ask(b) == "1");
        std::snprintf(b, sizeof(b), ":Sd%c%08lld#", dec_degrees < 0 ? '-' : '+',
                      std::llabs(std::llround(dec_degrees * 3600.0 * 100.0)));
        REQUIRE(ask(b) == "1");
    }
};

alpacacore::vendor::ioptron::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    conn.response_timeout_ms = 2000;
    return conn;
}

}  // namespace

TEST_CASE("FakeIoptronMount: the scripted default still lands a GOTO at once and never reports parked",
          "[ioptron][fake][mount-model][unit]") {
    FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    REQUIRE(mount.motion() == nullptr);
    REQUIRE(mount.respond_to(":SRA054000000#") == "1");  // 10 h
    REQUIRE(mount.respond_to(":Sd+03600000#") == "1");   // +10 degrees
    CHECK(mount.respond_to(":MS1#") == "1");
    const std::string gls = mount.respond_to(":GLS#");
    CHECK(gls[18] == '1');  // tracking at once: no slewing status
    CHECK(mount.respond_to(":GEP#").substr(0, 18) == "+03600000054000000");
    mount.respond_to(":MP1#");
    CHECK(mount.respond_to(":GLS#")[18] == '1');  // Park changes no status unless a case asks
}

TEST_CASE("FakeIoptronMount ConformU mode: a GOTO slews over its travel time before :GLS reads tracking",
          "[ioptron][fake][mount-model][unit]") {
    ModelledMount rig;
    REQUIRE(rig.mount.ok());
    rig.ask(":ST1#");
    const double lst = rig.mount.motion()->lst_hours();
    const double target_ra = std::fmod(lst + 1.0, 24.0);  // an hour west of the meridian's east: above the horizon
    rig.set_target(target_ra, -30.0);
    const double dec_before = rig.dec_degrees();
    REQUIRE(rig.ask(":MS1#") == "1");

    CHECK(rig.status() == 2);  // slewing
    rig.advance(seconds(2));
    CHECK(rig.status() == 2);
    const double mid = rig.dec_degrees();
    CHECK(mid != Catch::Approx(dec_before).margin(0.5));
    CHECK(mid != Catch::Approx(-30.0).margin(0.5));  // moving, not there yet

    rig.advance(seconds(60));
    CHECK(rig.status() == 1);  // tracking
    CHECK(rig.dec_degrees() == Catch::Approx(-30.0).epsilon(0).margin(1e-4));
    CHECK(rig.ra_hours() == Catch::Approx(target_ra).epsilon(0).margin(1e-5));
}

TEST_CASE("FakeIoptronMount ConformU mode: :GLS reports parked after Park lands and not before",
          "[ioptron][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.set_target(rig.mount.motion()->lst_hours(), -30.0);  // off the zero position, so Park has a slew to make
    REQUIRE(rig.ask(":MS1#") == "1");
    rig.advance(seconds(60));
    rig.ask(":MP1#");
    CHECK(rig.status() == 2);
    rig.advance(seconds(60));
    CHECK(rig.status() == 6);
    rig.advance(seconds(3600));
    CHECK(rig.status() == 6);  // an hour parked: still parked, the axes held
    rig.ask(":MP0#");
    CHECK(rig.status() != 6);
}

TEST_CASE("FakeIoptronMount ConformU mode: the tracking commands move the RA reading with the sky",
          "[ioptron][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.set_target(rig.mount.motion()->lst_hours(), -30.0);
    REQUIRE(rig.ask(":MS1#") == "1");
    rig.advance(seconds(120));
    const double held = rig.ra_hours();
    rig.advance(seconds(3600));
    CHECK(rig.ra_hours() == Catch::Approx(held).epsilon(0).margin(1e-5));  // tracking after a GOTO

    rig.ask(":ST0#");
    CHECK(rig.status() == 0);
    rig.advance(seconds(3600));
    // Tracking off: one sidereal hour of RA slips by.
    CHECK(rig.ra_hours() - held == Catch::Approx(1.0027379).epsilon(0).margin(1e-3));

    rig.ask(":ST1#");
    CHECK(rig.status() == 1);
}

TEST_CASE("FakeIoptronMount ConformU mode: the :GEP# pier digit follows the hour angle of where the mount points",
          "[ioptron][fake][mount-model][unit]") {
    ModelledMount rig;
    const double lst = rig.mount.motion()->lst_hours();
    rig.set_target(std::fmod(lst - 2.0 + 24.0, 24.0), -30.0);  // hour angle +2 h: west of the meridian
    REQUIRE(rig.ask(":MS1#") == "1");
    rig.advance(seconds(120));
    CHECK(rig.ask(":GEP#")[18] == '0');  // pier east, as the hour-angle rule of the driver has it

    rig.set_target(std::fmod(rig.mount.motion()->lst_hours() + 2.0, 24.0), -30.0);  // hour angle -2 h: east
    REQUIRE(rig.ask(":MS1#") == "1");
    rig.advance(seconds(120));
    CHECK(rig.ask(":GEP#")[18] == '1');  // pier west
}

TEST_CASE("FakeIoptronMount ConformU mode: a target below the horizon is refused like the altitude limit",
          "[ioptron][fake][mount-model][unit]") {
    ModelledMount rig;
    // Dec +60 from a southern site, 12 h from the meridian: far below the horizon.
    rig.set_target(std::fmod(rig.mount.motion()->lst_hours() + 12.0, 24.0), 60.0);
    CHECK(rig.ask(":MS1#") == "0");
    CHECK(rig.status() != 2);
}

TEST_CASE("FakeIoptronMount ConformU mode: a :Z pulse and the arrows move the axes at their rates",
          "[ioptron][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.set_target(rig.mount.motion()->lst_hours() - 2.0 < 0 ? 22.0 : rig.mount.motion()->lst_hours() - 2.0, -30.0);
    REQUIRE(rig.ask(":MS1#") == "1");
    rig.advance(seconds(60));
    const double ra0 = rig.ra_hours();

    rig.ask(":ZS02000#");  // RA+ for 2 s at the default 0.50x guide rate
    CHECK(rig.status() == 3);
    rig.advance(seconds(1));
    CHECK(rig.status() == 3);
    rig.advance(seconds(10));
    CHECK(rig.status() == 1);
    CHECK((rig.ra_hours() - ra0) * 15.0 ==
          Catch::Approx(0.5 * FakeMountMotionModel::kSiderealDegPerSec * 2.0).epsilon(0).margin(1e-4));

    rig.ask(":SR3#");  // 8x
    rig.ask(":mw#");   // RA+
    CHECK(rig.status() == 2);
    rig.advance(seconds(10));
    rig.ask(":qR#");
    CHECK(rig.status() == 1);
    CHECK((rig.ra_hours() - ra0) * 15.0 > 8.0 * 15.0 / 3600.0 * 10.0 * 0.9);
}

TEST_CASE("FakeIoptronMount ConformU mode: the driver reads AtPark after Park over the model",
          "[ioptron][fake][mount-model][unit]") {
    // A fast slew (720 degrees per second) keeps the case short; the clock is the real one.
    FakeMountMotionModel::Config config = FakeIoptronMount::conformu_motion_config();
    config.slew_deg_per_s = 720.0;
    FakeIoptronMount mount("0012", std::move(config));
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, seconds(10)));

    CHECK_FALSE(driver->get_at_park());
    driver->park();
    const auto deadline = std::chrono::steady_clock::now() + seconds(15);
    while (!driver->get_at_park() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(driver->get_at_park());
    CHECK(mount.motion()->parked());
}

#endif  // !_WIN32

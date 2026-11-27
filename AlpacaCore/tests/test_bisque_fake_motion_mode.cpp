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

// FakeBisqueMount in ConformU mode (#2697): the TheSkyX JavaScript wire answered from FakeMountMotionModel. The cases
// send the scripts the Bisque protocol wrapper sends straight to responder(), on a clock the case advances, so a case
// moves time by hand and never sleeps (the DirectGuide hold case excepted: it measures a real-time hold).

#ifndef _WIN32

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "fake_bisque_mount.h"

namespace {

using alpacacore::test::FakeBisqueMount;
using alpacacore::test::FakeMountMotionModel;
using std::chrono::seconds;

constexpr double kSiderealDegPerSec = 360.98564736629 / 86400.0;

// A fake in ConformU mode whose clock the case advances.
struct ModelledMount {
    std::shared_ptr<FakeMountMotionModel::TimePoint> now = std::make_shared<FakeMountMotionModel::TimePoint>(
        FakeMountMotionModel::TimePoint{seconds(1767225600)});  // 2026-01-01 00:00:00 UTC
    FakeBisqueMount mount{config(now)};

    static FakeMountMotionModel::Config config(const std::shared_ptr<FakeMountMotionModel::TimePoint>& clock) {
        auto c = FakeBisqueMount::conformu_motion_config();
        c.clock = [clock] { return *clock; };
        return c;
    }
    void advance(seconds by) { *now += by; }
    std::string ask(const std::string& script) { return mount.responder()(script); }

    // The wrapper's two script shapes (bisque_protocol_wrapper.cpp): an OK command and a read with its Out line.
    std::string command(const std::string& body) {
        return ask("/* Java Script */var Out;try {" + body + "Out  = 'OK#'; }catch (err) {Out = err; }");
    }
    std::string read(const std::string& body) {
        const std::string reply = ask("/* Java Script */var Out;" + body);
        const std::string prefix = "|No error. Error = 0.";
        REQUIRE(reply.rfind(prefix, 0) == 0);
        REQUIRE(reply.back() == '#');
        return reply.substr(prefix.size(), reply.size() - prefix.size() - 1);
    }

    void read_radec(double& ra_hours, double& dec_degrees) {
        const std::string value = read(
            "sky6RASCOMTele.GetRaDec();Out = String(sky6RASCOMTele.dRa) + ',' + String(sky6RASCOMTele.dDec) + '#';");
        REQUIRE(std::sscanf(value.c_str(), "%lf,%lf", &ra_hours, &dec_degrees) == 2);
    }
    double ra_hours() {
        double ra = 0.0, dec = 0.0;
        read_radec(ra, dec);
        return ra;
    }
    double dec_degrees() {
        double ra = 0.0, dec = 0.0;
        read_radec(ra, dec);
        return dec;
    }
    bool slew_complete() { return read("Out = sky6RASCOMTele.IsSlewComplete + '#';") == "1"; }
    bool parked() { return read("Out = sky6RASCOMTele.IsParked() + '#';") == "true"; }
    bool tracking() { return read("Out = sky6RASCOMTele.IsTracking + '#';") == "1"; }

    std::string set_tracking(bool on) {
        return command(std::string("sky6RASCOMTele.SetTracking(") + (on ? "1" : "0") + ", 1, 0, 0);");
    }
    std::string sync(double ra_hours, double dec_degrees) {
        char body[96];
        std::snprintf(body, sizeof(body), "sky6RASCOMTele.Sync(%g, %g,'');", ra_hours, dec_degrees);
        return command(body);
    }
    std::string slew(double ra_hours, double dec_degrees) {
        char body[128];
        std::snprintf(body, sizeof(body), "sky6RASCOMTele.Asynchronous = true;sky6RASCOMTele.SlewToRaDec(%g, %g,'');",
                      ra_hours, dec_degrees);
        return command(body);
    }
    std::string guide(double ra_arcsec, double dec_arcsec) {
        char body[128];
        std::snprintf(body, sizeof(body), "sky6DirectGuide.lAsynchronous = 0;sky6DirectGuide.MoveTelescope(%g, %g);",
                      ra_arcsec, dec_arcsec);
        return command(body);
    }
};

const std::string kOk = "|No error. Error = 0.OK#";

// The shortest wrapped difference of two RA values, hours.
double ra_gap_hours(double a, double b) { return std::remainder(a - b, 24.0); }

}  // namespace

TEST_CASE("FakeBisqueMount: the scripted default has no motion model and keeps its fixed position",
          "[bisque][fake][mount-model][unit]") {
    FakeBisqueMount mount;
    CHECK(mount.motion() == nullptr);
    const auto ask = mount.responder();
    CHECK(ask("/* Java Script */var Out;try {sky6RASCOMTele.Asynchronous = true;sky6RASCOMTele.SlewToRaDec(6, 40,'');"
              "Out  = 'OK#'; }catch (err) {Out = err; }") == kOk);
    // land_on_target is off: the slew completes on the first poll and the reported position never moves.
    CHECK(ask("/* Java Script */var Out;Out = sky6RASCOMTele.IsSlewComplete + '#';") == "|No error. Error = 0.1#");
    CHECK(ask("/* Java Script */var Out;sky6RASCOMTele.GetRaDec();Out = String(sky6RASCOMTele.dRa) + ',' + "
              "String(sky6RASCOMTele.dDec) + '#';") == "|No error. Error = 0.5.500000,20.000000#");
}

TEST_CASE("FakeBisqueMount ConformU mode: a SlewToRaDec reports complete and on target only after its travel time",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    REQUIRE(rig.mount.motion() != nullptr);
    REQUIRE(rig.set_tracking(true) == kOk);
    // Four decimals, so the wrapper's %g (six significant digits) carries the aim exactly.
    const double aim_ra = std::round(std::fmod(rig.mount.motion()->lst_hours() + 1.0, 24.0) * 1e4) / 1e4;
    const double dec_before = rig.dec_degrees();
    REQUIRE(rig.slew(aim_ra, -30.0) == kOk);

    CHECK_FALSE(rig.slew_complete());
    rig.advance(seconds(5));
    CHECK_FALSE(rig.slew_complete());
    const double mid = rig.dec_degrees();
    CHECK(mid != Catch::Approx(dec_before).margin(0.5));
    CHECK(mid != Catch::Approx(-30.0).margin(0.5));  // on its way, not there

    rig.advance(seconds(60));
    CHECK(rig.slew_complete());
    CHECK(rig.dec_degrees() == Catch::Approx(-30.0).margin(1e-6));
    CHECK(ra_gap_hours(rig.ra_hours(), aim_ra) == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("FakeBisqueMount ConformU mode: a Sync re-labels the reported position",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    const double dec_before = rig.dec_degrees();
    REQUIRE(rig.sync(6.0, 40.0) == kOk);
    CHECK(rig.dec_degrees() != Catch::Approx(dec_before).margin(1.0));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0).margin(1e-6));
    CHECK(rig.ra_hours() == Catch::Approx(6.0).margin(1e-6));
    CHECK(rig.slew_complete());  // a sync moves nothing
}

TEST_CASE("FakeBisqueMount ConformU mode: tracking holds RA and stopped tracking lets it drift east",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.sync(6.0, 40.0);
    REQUIRE(rig.set_tracking(true) == kOk);
    CHECK(rig.tracking());
    rig.advance(seconds(600));
    CHECK(rig.ra_hours() == Catch::Approx(6.0).margin(1e-6));

    REQUIRE(rig.set_tracking(false) == kOk);
    CHECK_FALSE(rig.tracking());
    rig.advance(seconds(600));
    CHECK(rig.ra_hours() == Catch::Approx(6.0 + 600.0 * kSiderealDegPerSec / 15.0).margin(1e-5));
}

TEST_CASE("FakeBisqueMount ConformU mode: DoCommand(9) runs one axis at its preset rate until DoCommand(10)",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.sync(6.0, 40.0);
    rig.set_tracking(true);
    // Direction 0 (North) at preset 3 (8x sidereal) raises Dec; RA holds.
    REQUIRE(rig.command("sky6RASCOMTele.DoCommand(9,'0|3');") == kOk);
    rig.advance(seconds(10));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0 + 8.0 * kSiderealDegPerSec * 10.0).margin(1e-6));
    CHECK(rig.ra_hours() == Catch::Approx(6.0).margin(1e-6));
    CHECK(rig.slew_complete());  // open-loop motion is not a slew

    REQUIRE(rig.command("sky6RASCOMTele.DoCommand(10,'');") == kOk);
    const double stopped = rig.dec_degrees();
    rig.advance(seconds(10));
    CHECK(rig.dec_degrees() == Catch::Approx(stopped).margin(1e-9));

    // Direction 3 (West) at preset 8 (512x) lowers RA.
    REQUIRE(rig.command("sky6RASCOMTele.DoCommand(9,'3|8');") == kOk);
    rig.advance(seconds(2));
    CHECK(rig.ra_hours() == Catch::Approx(6.0 - 512.0 * kSiderealDegPerSec * 2.0 / 15.0).margin(1e-6));
    rig.command("sky6RASCOMTele.DoCommand(10,'');");

    // A rate index past the nine presets is a TheSkyX error, and nothing moves.
    CHECK(rig.command("sky6RASCOMTele.DoCommand(9,'0|9');") != kOk);
    const double before = rig.dec_degrees();
    rig.advance(seconds(5));
    CHECK(rig.dec_degrees() == Catch::Approx(before).margin(1e-9));
}

TEST_CASE("FakeBisqueMount ConformU mode: a DirectGuide moves each axis by its arc seconds",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.sync(6.0, 40.0);
    rig.set_tracking(true);
    // 15 arc seconds of RA east and 30 north, at 0.5x sidereal: the longer (Dec) move runs about 4 s.
    REQUIRE(rig.guide(15.0, 30.0) == kOk);
    rig.advance(seconds(1));
    CHECK(rig.dec_degrees() < 40.0 + 30.0 / 3600.0 - 1e-4);  // still moving
    rig.advance(seconds(10));
    CHECK(rig.ra_hours() == Catch::Approx(6.0 + 15.0 / 3600.0 / 15.0).margin(1e-6));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0 + 30.0 / 3600.0).margin(1e-6));

    REQUIRE(rig.guide(0.0, -30.0) == kOk);  // south
    rig.advance(seconds(10));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0).margin(1e-6));
}

TEST_CASE("FakeBisqueMount ConformU mode: with guide_holds_for_pulse a DirectGuide replies only after its move",
          "[bisque][fake][mount-model][unit]") {
    FakeBisqueMount mount{FakeBisqueMount::conformu_motion_config()};  // the real clock: the hold is real time
    const auto guide = [&mount](double dec_arcsec) {
        char script[160];
        std::snprintf(script, sizeof(script),
                      "/* Java Script */var Out;try {sky6DirectGuide.lAsynchronous = 0;"
                      "sky6DirectGuide.MoveTelescope(0, %g);Out  = 'OK#'; }catch (err) {Out = err; }",
                      dec_arcsec);
        const auto start = std::chrono::steady_clock::now();
        CHECK(mount.responder()(script) == kOk);
        return std::chrono::steady_clock::now() - start;
    };
    // 1.5 arc seconds at 0.5x sidereal (7.52 arcsec/s) is a 200 ms move.
    CHECK(guide(1.5) < std::chrono::milliseconds(150));  // off by default: the reply comes at once
    mount.guide_holds_for_pulse = true;
    CHECK(guide(1.5) >= std::chrono::milliseconds(190));
}

TEST_CASE("FakeBisqueMount ConformU mode: a park reads as a slew and reports parked on arrival; Unpark clears it",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.sync(6.0, 40.0);
    rig.set_tracking(true);
    REQUIRE(rig.command("sky6RASCOMTele.Asynchronous = true;sky6RASCOMTele.ParkAndDoNotDisconnect();") == kOk);
    CHECK_FALSE(rig.slew_complete());
    CHECK_FALSE(rig.parked());
    rig.advance(seconds(120));
    CHECK(rig.slew_complete());
    CHECK(rig.parked());
    CHECK_FALSE(rig.tracking());  // a parked mount does not track
    CHECK(rig.dec_degrees() == Catch::Approx(0.0).margin(1e-6));
    CHECK(rig.mount.motion()->hour_angle_hours() == Catch::Approx(-6.0).margin(1e-4));

    REQUIRE(rig.command("sky6RASCOMTele.Unpark();") == kOk);
    CHECK_FALSE(rig.parked());
}

TEST_CASE("FakeBisqueMount ConformU mode: FindHome slews home, and Abort stops a slew where it is",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    rig.sync(6.0, 40.0);
    REQUIRE(rig.command("sky6RASCOMTele.Asynchronous = true;sky6RASCOMTele.FindHome();") == kOk);
    CHECK_FALSE(rig.slew_complete());
    rig.advance(seconds(120));
    CHECK(rig.slew_complete());
    CHECK(rig.mount.motion()->at_home());
    CHECK(rig.dec_degrees() == Catch::Approx(-90.0).margin(1e-6));

    rig.set_tracking(true);
    REQUIRE(rig.slew(std::fmod(rig.mount.motion()->lst_hours() + 1.0, 24.0), -30.0) == kOk);
    rig.advance(seconds(5));
    REQUIRE(rig.command("sky6RASCOMTele.Abort();") == kOk);
    CHECK(rig.slew_complete());
    const double stopped = rig.dec_degrees();
    CHECK(stopped > -90.0 + 1.0);
    CHECK(stopped < -30.0 - 1.0);
    rig.advance(seconds(60));
    CHECK(rig.dec_degrees() == Catch::Approx(stopped).margin(1e-9));
}

TEST_CASE("FakeBisqueMount ConformU mode: Alt/Az, pier side and the UTC read come from the model",
          "[bisque][fake][mount-model][unit]") {
    ModelledMount rig;
    const auto* model = rig.mount.motion();
    double az = 0.0, alt = 0.0;
    const std::string altaz = rig.read(
        "sky6RASCOMTele.GetAzAlt();Out = String(sky6RASCOMTele.dAz) + ',' + String(sky6RASCOMTele.dAlt) + '#';");
    REQUIRE(std::sscanf(altaz.c_str(), "%lf,%lf", &az, &alt) == 2);
    CHECK(alt == Catch::Approx(model->altitude_degrees()).margin(1e-6));
    CHECK(az == Catch::Approx(model->azimuth_degrees()).margin(1e-6));

    const auto pier = [&rig] {
        return rig.read("sky6RASCOMTele.DoCommand(11, \"Pier Side\");Out = sky6RASCOMTele.DoCommandOutput + '#';");
    };
    rig.set_tracking(true);
    rig.sync(std::fmod(model->lst_hours() + 2.0 + 24.0, 24.0), 0.0);  // hour angle -2 h: east of the meridian
    CHECK(pier() == std::to_string(model->pier_side()));
    const std::string east = pier();
    rig.sync(std::fmod(model->lst_hours() - 2.0 + 24.0, 24.0), 0.0);  // hour angle +2 h: west
    CHECK(pier() == std::to_string(model->pier_side()));
    CHECK(pier() != east);

    const std::string utc = rig.read(
        "sky6StarChart.DocumentProperty(5);"
        "Out = (sky6StarChart.DocPropOut != 0 ? String(new Date().getTime()) : 'unavailable') + '#';");
    CHECK(utc == "1767225600000");
    rig.advance(seconds(3));
    CHECK(rig.read("sky6StarChart.DocumentProperty(5);"
                   "Out = (sky6StarChart.DocPropOut != 0 ? String(new Date().getTime()) : 'unavailable') + '#';") ==
          "1767225603000");
}

#endif  // !_WIN32

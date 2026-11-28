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

// FakeOnStepMount (fake_onstep_mount.h, #2692): the OnStepX mount commands answered from FakeMountMotionModel. The
// cases drive respond_to() on a hand-advanced clock, so they pin the fake, not the driver (the driver runs against it
// in the fake ConformU row).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "fake_onstep_mount.h"

namespace {

using alpacacore::test::FakeMountMotionModel;
using alpacacore::test::FakeOnStepMount;
using std::chrono::seconds;

struct ModelledOnStep {
    std::shared_ptr<FakeMountMotionModel::TimePoint> now = std::make_shared<FakeMountMotionModel::TimePoint>(
        FakeMountMotionModel::TimePoint{seconds(1767225600)});  // 2026-01-01 00:00:00 UTC
    FakeOnStepMount mount;

    ModelledOnStep() : mount(config(now)) {}

    static FakeMountMotionModel::Config config(const std::shared_ptr<FakeMountMotionModel::TimePoint>& clock) {
        auto c = FakeOnStepMount::conformu_motion_config();
        c.clock = [clock] { return *clock; };
        return c;
    }
    void advance(seconds by) { *now += by; }
    std::string ask(const std::string& command) { return mount.respond_to(command); }
    // Raw bytes as a socket delivers them: several commands, or part of one.
    std::string chunk(const std::string& bytes) { return mount.responder()(bytes); }

    // ":GU#" without its '#': the letters the driver reads.
    std::string status() {
        std::string reply = ask(":GU#");
        reply.pop_back();
        return reply;
    }
    bool has(char letter) { return status().find(letter) != std::string::npos; }
    bool slewing() { return !has('N'); }
    bool tracking() { return !has('n'); }

    static double parse_hms(const std::string& reply) {
        int h = 0, m = 0;
        double s = 0.0;
        REQUIRE(std::sscanf(reply.c_str(), "%d:%d:%lf", &h, &m, &s) == 3);
        return h + m / 60.0 + s / 3600.0;
    }
    static double parse_dms(const std::string& reply) {
        int d = 0, m = 0;
        double s = 0.0;
        REQUIRE(std::sscanf(reply.c_str() + 1, "%d*%d:%lf", &d, &m, &s) == 3);
        return (reply[0] == '-' ? -1.0 : 1.0) * (d + m / 60.0 + s / 3600.0);
    }
    double ra_hours() { return parse_hms(ask(":GRa#")); }
    double dec_degrees() { return parse_dms(ask(":GDe#")); }
    int last_error() { return std::stoi(ask(":GE#")); }

    static std::string hms_of(double hours) {
        const int total = static_cast<int>(std::lround(std::fmod(hours + 24.0, 24.0) * 3600.0)) % 86400;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
        return buf;
    }
    void set_target(double ra_hours, const std::string& dec) {
        REQUIRE(ask(":Sr" + hms_of(ra_hours) + "#") == "1");
        REQUIRE(ask(":Sd" + dec + "#") == "1");
    }
    // A sync to a known start, so a case does not depend on the mount's power-on pointing.
    void sync_to(double ra_hours, const std::string& dec) {
        set_target(ra_hours, dec);
        REQUIRE(ask(":CM#") == "N/A#");
    }
};

}  // namespace

TEST_CASE("FakeOnStepMount - a GOTO reports slewing and the target only after its travel time",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    CHECK(rig.ask(":Te#") == "1");
    const double dec_before = rig.dec_degrees();
    const double aim_ra = std::fmod(rig.mount.motion().lst_hours() + 1.0, 24.0);
    rig.set_target(aim_ra, "-30*00:00:00");
    REQUIRE(rig.ask(":MS#") == "0");

    CHECK(rig.slewing());
    rig.advance(seconds(5));
    CHECK(rig.slewing());
    const double mid = rig.dec_degrees();
    CHECK(mid != Catch::Approx(dec_before).margin(0.5));
    CHECK(mid != Catch::Approx(-30.0).margin(0.5));  // on its way, not there

    rig.advance(seconds(60));
    CHECK_FALSE(rig.slewing());
    CHECK(rig.dec_degrees() == Catch::Approx(-30.0).margin(1e-3));
    CHECK(std::remainder(rig.ra_hours() - aim_ra, 24.0) == Catch::Approx(0.0).margin(1e-3));
}

TEST_CASE("FakeOnStepMount - :MS# is refused with the firmware's digit and :GE# keeps the code",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.set_target(12.0, "+80*00:00:00");  // never above the horizon from a site at -33.9 degrees
    CHECK(rig.ask(":MS#") == "1");
    CHECK(rig.last_error() == 15);
    CHECK_FALSE(rig.slewing());
    CHECK(rig.dec_degrees() == Catch::Approx(40.0).margin(1e-3));

    rig.set_target(rig.mount.motion().lst_hours(), "-30*00:00:00");
    REQUIRE(rig.ask(":MS#") == "0");
    CHECK(rig.last_error() == 0);
    CHECK(rig.ask(":MS#") == "5");  // a GOTO already running; the firmware stops it
    CHECK(rig.last_error() == 19);
    CHECK_FALSE(rig.slewing());
}

TEST_CASE("FakeOnStepMount - a Sync re-labels the reported position", "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    const double dec_before = rig.dec_degrees();
    rig.set_target(6.0, "+40*00:00:00");
    REQUIRE(rig.ask(":CM#") == "N/A#");
    CHECK(rig.dec_degrees() != Catch::Approx(dec_before).margin(1.0));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0).margin(1e-4));
    CHECK(rig.ra_hours() == Catch::Approx(6.0).margin(1e-5));
    CHECK_FALSE(rig.slewing());  // a sync moves nothing
}

TEST_CASE("FakeOnStepMount - tracking holds RA and stopped tracking lets it drift east",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    REQUIRE(rig.ask(":Te#") == "1");
    CHECK(rig.tracking());
    CHECK(rig.ask(":GT#") == "60.16427#");
    rig.advance(seconds(600));
    CHECK(rig.ra_hours() == Catch::Approx(6.0).margin(1e-4));  // tracking: the sky position holds

    REQUIRE(rig.ask(":ST0#") == "1");
    CHECK_FALSE(rig.tracking());
    CHECK(rig.ask(":GT#") == "0#");
    rig.advance(seconds(600));
    // 600 s of sidereal time is 600 * 360.98564736629 / 86400 degrees of RA east.
    CHECK(rig.ra_hours() == Catch::Approx(6.0 + 600.0 * 360.98564736629 / 86400.0 / 15.0).margin(1e-4));
}

TEST_CASE("FakeOnStepMount - a rate offset moves the position while tracking, in arcseconds per sidereal second",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.ask(":Te#");
    REQUIRE(rig.ask(":SXTD,36.000000#") == "1");   // 36 arcsec per sidereal second: 0.01 degrees per sidereal second
    REQUIRE(rig.ask(":SXTR,-15.000000#") == "1");  // -1 second of RA per sidereal second
    CHECK(rig.ask(":GXTD#") == "36.00000000#");
    rig.advance(seconds(100));
    const double sidereal_second = 86164.0905 / 86400.0;
    CHECK(rig.dec_degrees() == Catch::Approx(40.0 + 0.01 * 100.0 / sidereal_second).margin(1e-4));
    CHECK(rig.ra_hours() == Catch::Approx(6.0 - 100.0 / sidereal_second / 3600.0).margin(1e-4));
}

TEST_CASE("FakeOnStepMount - the tracking rate letter follows :TL#, :TS# and :TK#",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.ask(":Te#");
    CHECK(rig.ask(":TL#").empty());  // blind: no reply
    CHECK(rig.has('('));
    CHECK(rig.ask(":GT#") == "57.90000#");
    rig.ask(":TS#");
    CHECK(rig.has('O'));
    rig.ask(":TK#");
    CHECK(rig.has('k'));
    rig.ask(":TQ#");
    CHECK_FALSE(rig.has('k'));
    CHECK(rig.ask(":GT#") == "60.16427#");
}

TEST_CASE("FakeOnStepMount - a MoveAxis runs its axis, ends at the guide time limit unless re-sent",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.ask(":Te#");
    CHECK(rig.chunk(":RA0.5000#:Me#").empty());  // two blind commands in one chunk, no reply
    CHECK(rig.has('g'));
    CHECK_FALSE(rig.slewing());  // 'N' covers a GOTO only: a MoveAxis never clears it
    rig.advance(seconds(8));
    CHECK(rig.ra_hours() == Catch::Approx(6.0 + 0.5 * 8.0 / 15.0).margin(1e-4));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0).margin(1e-4));  // the other axis stays

    rig.ask(":Me#");  // the driver's re-send restarts the 10 s limit
    rig.advance(seconds(8));
    CHECK(rig.has('g'));
    CHECK(rig.ra_hours() == Catch::Approx(6.0 + 0.5 * 16.0 / 15.0).margin(1e-4));

    rig.advance(seconds(5));  // 13 s since the last send: the firmware has ended the move
    CHECK_FALSE(rig.has('g'));
    const double ended = rig.ra_hours();
    CHECK(ended == Catch::Approx(6.0 + 0.5 * 18.0 / 15.0).margin(1e-4));  // moved to the limit (8 s + 10 s), no further
    rig.advance(seconds(10));
    CHECK(rig.ra_hours() == Catch::Approx(ended).margin(1e-5));

    rig.ask(":RE1.0000#");
    rig.ask(":Ms#");  // south lowers Dec
    rig.advance(seconds(5));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0 - 5.0).margin(1e-4));
    rig.ask(":Qs#");
    CHECK_FALSE(rig.has('g'));
    CHECK(rig.dec_degrees() == Catch::Approx(35.0).margin(1e-4));
}

TEST_CASE("FakeOnStepMount - a guide pulse moves its axis for its duration, then stops",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.ask(":Te#");
    CHECK(rig.ask(":Mgn9999#").empty());
    CHECK(rig.has('G'));
    CHECK_FALSE(rig.has('g'));  // a pulse guide is G, not g
    rig.advance(seconds(5));
    const double sidereal = FakeMountMotionModel::kSiderealDegPerSec;
    CHECK(rig.dec_degrees() == Catch::Approx(40.0 + 0.5 * sidereal * 5.0).margin(1e-4));
    rig.advance(seconds(10));
    CHECK(rig.dec_degrees() == Catch::Approx(40.0 + 0.5 * sidereal * 9.999).margin(1e-4));
    CHECK_FALSE(rig.has('G'));
    CHECK(rig.ra_hours() == Catch::Approx(6.0).margin(1e-5));  // the RA axis never moved
}

TEST_CASE("FakeOnStepMount - a move and a pulse are ignored while the mount slews",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.set_target(rig.mount.motion().lst_hours(), "-30*00:00:00");
    REQUIRE(rig.ask(":MS#") == "0");
    rig.chunk(":RA0.5000#:Me#");
    rig.ask(":Mge1000#");
    CHECK_FALSE(rig.has('g'));
    CHECK_FALSE(rig.has('G'));
    CHECK(rig.slewing());
}

TEST_CASE("FakeOnStepMount - :hQ# is refused during a GOTO, a guide move or while parked",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;  // the firmware's Park.cpp set() refusals, which SetPark reads with :GE#
    rig.sync_to(6.0, "+40*00:00:00");
    rig.set_target(rig.mount.motion().lst_hours(), "-30*00:00:00");
    REQUIRE(rig.ask(":MS#") == "0");
    CHECK(rig.ask(":hQ#") == "0");
    CHECK(rig.last_error() == 22);
    rig.advance(seconds(60));
    REQUIRE_FALSE(rig.slewing());
    CHECK(rig.ask(":Mge5000#").empty());
    CHECK(rig.ask(":hQ#") == "0");
    CHECK(rig.last_error() == 22);
    rig.advance(seconds(6));
    CHECK(rig.ask(":hQ#") == "1");
    CHECK(rig.last_error() == 0);

    REQUIRE(rig.ask(":hP#") == "1");
    rig.advance(seconds(120));
    REQUIRE(rig.has('P'));
    CHECK(rig.ask(":hQ#") == "0");
    CHECK(rig.last_error() == 9);
}

TEST_CASE("FakeOnStepMount - Park is reached after its travel time, tracking goes off and Unpark clears it",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.ask(":Te#");
    REQUIRE(rig.ask(":hP#") == "1");
    CHECK(rig.has('I'));  // parking
    CHECK(rig.slewing());
    rig.advance(seconds(5));
    CHECK(rig.has('I'));
    rig.advance(seconds(60));
    CHECK(rig.has('P'));
    CHECK_FALSE(rig.slewing());
    CHECK_FALSE(rig.tracking());
    CHECK(rig.has('o'));  // a parked mount reports no pier side

    CHECK(rig.ask(":Te#") == "0");  // enabling is refused while parked
    CHECK(rig.last_error() == 9);
    CHECK(rig.ask(":ST60.164275#") == "0");
    CHECK(rig.ask(":ST0#") == "1");  // disabling a stopped mount is a no-op, as ASCOM requires of Tracking = false
    CHECK(rig.last_error() == 0);
    CHECK(rig.ask(":MS#") == "4");
    CHECK(rig.last_error() == 18);  // digit 4 is code 18, "mount parked"
    CHECK(rig.ask(":CM#") == "E4#");
    CHECK(rig.last_error() == 18);
    CHECK(rig.ask(":hC#").empty());  // a home of a parked mount: code 18, nothing starts
    CHECK(rig.last_error() == 18);
    CHECK_FALSE(rig.has('h'));
    CHECK(rig.has('P'));
    CHECK(rig.ask(":hP#") == "0");  // already parked: code 9, which the driver takes as ASCOM's no-op
    CHECK(rig.last_error() == 9);
    CHECK(rig.has('P'));

    REQUIRE(rig.ask(":hR#") == "1");
    CHECK(rig.has('p'));
    CHECK(rig.ask(":hR#") == "0");  // not parked: the driver treats code 11 as ASCOM's no-op
    CHECK(rig.last_error() == 11);
}

TEST_CASE("FakeOnStepMount - a stop during a park leaves the mount unparked where it stopped",
          "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    rig.ask(":Te#");
    rig.ask(":hP#");
    rig.advance(seconds(5));
    CHECK(rig.ask(":Q#").empty());
    CHECK_FALSE(rig.slewing());
    CHECK(rig.has('p'));
    rig.advance(seconds(60));
    CHECK_FALSE(rig.has('P'));
}

TEST_CASE("FakeOnStepMount - FindHome reports homing, then at home", "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    rig.sync_to(6.0, "+40*00:00:00");
    CHECK(rig.ask(":hC#").empty());  // blind
    CHECK(rig.has('h'));
    CHECK(rig.slewing());
    CHECK_FALSE(rig.has('H'));
    rig.advance(seconds(60));
    CHECK_FALSE(rig.slewing());
    CHECK_FALSE(rig.has('h'));
    CHECK(rig.has('H'));
    CHECK(rig.dec_degrees() == Catch::Approx(-90.0).margin(1e-3));  // the pole, counterweight down

    rig.set_target(rig.mount.motion().lst_hours(), "-30*00:00:00");
    REQUIRE(rig.ask(":MS#") == "0");
    rig.ask(":hC#");  // refused while moving: code 22, nothing starts
    CHECK(rig.last_error() == 22);
    CHECK_FALSE(rig.has('h'));
}

TEST_CASE("FakeOnStepMount - a command split across two chunks is answered once", "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    auto responder = rig.mount.responder();
    CHECK(responder(":GV").empty());
    CHECK(responder("N#") == "10.23a#");
    CHECK(responder(":GVP#:GVN#") == "On-Step#10.23a#");
}

TEST_CASE("FakeOnStepMount - site and clock writes are read back and run on", "[onstep][fake][mount-model][unit]") {
    ModelledOnStep rig;
    CHECK(rig.ask(":Gt#") == "-33*51#");
    CHECK(rig.ask(":Gg#") == "-151*13#");  // west-positive on the wire: east 151.2 reads as west -151.2
    REQUIRE(rig.ask(":St+51*29#") == "1");
    REQUIRE(rig.ask(":Sg+000*00#") == "1");  // Greenwich
    CHECK(rig.ask(":Gt#") == "+51*29#");
    CHECK(rig.ask(":Gg#") == "+000*00#");
    CHECK(rig.mount.motion().site_latitude_degrees() == Catch::Approx(51.0 + 29.0 / 60.0).margin(1e-9));

    CHECK(rig.ask(":GL#") == "00:00:00#");
    CHECK(rig.ask(":GC#") == "01/01/26#");
    REQUIRE(rig.ask(":SL05:06:07#") == "1");
    REQUIRE(rig.ask(":SC03/04/26#") == "1");
    CHECK(rig.ask(":GL#") == "05:06:07#");
    CHECK(rig.ask(":GC#") == "03/04/26#");
    rig.advance(seconds(90));
    CHECK(rig.ask(":GL#") == "05:07:37#");  // the written clock runs on
}

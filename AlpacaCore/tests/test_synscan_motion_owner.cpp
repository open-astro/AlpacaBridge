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
//
// Motion-owner follow-ups (issue #2467): tests that pin the #2263 motion-claim table for this driver.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <chrono>
#include <memory>
#include <optional>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_synscan_handset.h"
#include "fake_task_clock.h"

namespace {

using alpacacore::test::FakeSynScanHandset;
using alpacacore::test::SynScanGotoLanding;

struct PulseBesideJogRig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeSynScanHandset> st =
        std::make_shared<FakeSynScanHandset>(clock, SynScanGotoLanding::Stays, "12AB0500,20000500#");
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    PulseBesideJogRig() {
        st->model_id.store(50);
        server = std::make_unique<alpacacore::test::FakeMountServer>(st->responder());
        REQUIRE(server->ok());
        alpacacore::vendor::synscan::ConnectionInfo info;
        info.type = alpacacore::vendor::synscan::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server->port();
        info.response_timeout_ms = 1000;
        driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, info, alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
    }
    ~PulseBesideJogRig() { driver->set_connected(false); }
};

}  // namespace

// #2263 table, #2467 maintainer decision: a PulseGuide runs beside a MoveAxis on the OTHER axis. SynScan refused it
// because slewing_by_driver_state_locked() was true for a jog on either axis.
TEST_CASE("SynScan PulseGuide - accepted during a MoveAxis on the other axis (#2263, #2467)",
          "[synscan][telescope][pulseguiding][moveaxis][motion-owner]") {
    struct Case {
        const char* name;
        int jog_axis;
        int direction;  // 0 N, 1 S (axis 1); 2 E, 3 W (axis 0)
    };
    for (const Case c : {Case{"Dec jog, East pulse", 1, 2}, Case{"Dec jog, West pulse", 1, 3},
                         Case{"RA jog, North pulse", 0, 0}, Case{"RA jog, South pulse", 0, 1}}) {
        INFO(c.name);
        PulseBesideJogRig rig;
        rig.driver->move_axis(c.jog_axis, 0.5);
        REQUIRE(rig.driver->get_slewing());
        const int rate_commands = rig.st->command_count('P');
        REQUIRE_NOTHROW(rig.driver->pulse_guide(c.direction, 200));
        CHECK(rig.st->command_count('P') > rate_commands);  // the pulse's rate command reached the handset
        CHECK(rig.driver->get_is_pulse_guiding());
        CHECK(rig.driver->get_slewing());  // the jog still runs
    }
}

TEST_CASE("SynScan PulseGuide - still refused during a MoveAxis on its own axis (#2263, #2467)",
          "[synscan][telescope][pulseguiding][moveaxis][motion-owner]") {
    struct Case {
        const char* name;
        int jog_axis;
        int direction;
    };
    for (const Case c : {Case{"RA jog, East pulse", 0, 2}, Case{"RA jog, West pulse", 0, 3},
                         Case{"Dec jog, North pulse", 1, 0}, Case{"Dec jog, South pulse", 1, 1}}) {
        INFO(c.name);
        PulseBesideJogRig rig;
        rig.driver->move_axis(c.jog_axis, 0.5);
        const int rate_commands = rig.st->command_count('P');
        alpacacore::test::require_alpaca_error([&] { rig.driver->pulse_guide(c.direction, 200); },
                                               alpacacore::AlpacaError::InvalidOperation);
        CHECK(rig.st->command_count('P') == rate_commands);
        CHECK_FALSE(rig.driver->get_is_pulse_guiding());
        CHECK(rig.driver->get_slewing());
    }
}

#endif  // !_WIN32

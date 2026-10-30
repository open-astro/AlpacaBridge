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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <thread>

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {
using namespace std::chrono_literals;

struct GetterWaitRig {
    alpacacore::test::FakeCelestronCgxMount mount;
    alpacacore::test::FakeTaskClock clock;
    std::chrono::system_clock::time_point utc{1700000000s};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    GetterWaitRig() {
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = -33.856784;
        info.site_longitude_degrees = 151.215297;
        driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock, [this] { return utc; });
        driver->set_connected(true);
        driver->find_home();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        REQUIRE(driver->get_at_home());
    }

    // Holds the RA axis position reply (axis 0x10, command 0x01) half-sent.
    void hold_ra_position_reply() { mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x01}); }

    // get_guide_rate and get_aperture_diameter take mutex_ and need no device access.
    // One shared probe window, measured from probe start; the 500 ms AUX timeout is measured from the start of the
    // holder exchange. The window is wide for slow arm64/sanitizer hosts but well under that timeout, so a timed-out
    // holder cannot release the probes inside it.
    void check_state_getters_return() {
        auto guide = std::async(std::launch::async, [&] { return driver->get_guide_rate(); });
        auto aperture = std::async(std::launch::async, [&] { return driver->get_aperture_diameter(); });
        const auto deadline = std::chrono::steady_clock::now() + 300ms;
        const bool guide_done = guide.wait_until(deadline) == std::future_status::ready;
        const bool aperture_done = aperture.wait_until(deadline) == std::future_status::ready;
        mount.release_reply_tail();  // always release so the test cannot hang
        // On the current code the probes wait for the holder; a Sync holder needs the fake clock to move.
        while (guide.wait_for(2ms) != std::future_status::ready || aperture.wait_for(2ms) != std::future_status::ready)
            clock.advance(20ms);
        CHECK(guide_done);
        CHECK(aperture_done);
    }
};
}  // namespace

TEST_CASE("CGX state-only getters return while a position read waits on serial (#1837)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.clock.advance(1s);  // expire the 30 ms raw-position cache so the RA getter must read the axis
    rig.hold_ra_position_reply();
    auto read = std::async(std::launch::async, [&] { return rig.driver->get_right_ascension(); });
    const bool held = rig.mount.wait_for_reply_fragment(2s);
    if (!held) rig.mount.release_reply_tail();
    REQUIRE(held);
    rig.check_state_getters_return();
    try {
        static_cast<void>(read.get());
    } catch (const std::exception&) {  // may time out once held past 500 ms
    }
}

namespace {
// Syncs while the first reply matching `filter` is held half-sent, then probes the state getters.
// A held tracking-off or tracking-on ACK needs tracking on before Sync (it switches tracking off, then back on).
void check_getters_while_sync_holds(GetterWaitRig& rig, std::pair<std::uint8_t, std::uint8_t> filter,
                                    bool tracking_on) {
    if (tracking_on) rig.driver->set_tracking(true);
    rig.clock.advance(1s);
    rig.mount.fragment_next_reply(filter);
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(3.0, 20.0); });
    // Sync's settle loop sleeps on the fake clock; keep it moving until the reply is held.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool held = false;
    while (!held && std::chrono::steady_clock::now() < deadline) {
        held = rig.mount.wait_for_reply_fragment(5ms);
        if (!held) rig.clock.advance(20ms);
    }
    if (!held) rig.mount.release_reply_tail();
    REQUIRE(held);
    rig.check_state_getters_return();
    while (sync.wait_for(5ms) != std::future_status::ready) rig.clock.advance(20ms);
    try {
        sync.get();
    } catch (const std::exception&) {  // may time out once held past 500 ms
    }
}
}  // namespace

TEST_CASE("CGX state-only getters return while SyncToCoordinates waits on serial (#1837)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    check_getters_while_sync_holds(rig, {0x10, 0x01}, false);  // settle-loop axis read
}

TEST_CASE("CGX state-only getters return while Sync waits on the label write ACK (#1837)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    check_getters_while_sync_holds(rig, {0x10, 0x04}, false);  // MC_SET_POSITION
}

TEST_CASE("CGX state-only getters return while Sync waits on a tracking ACK (#1837)", "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    check_getters_while_sync_holds(rig, {0x10, 0x06}, true);  // tracking off before the labels
}

TEST_CASE("CGX state-only getters return while Sync restores tracking (#1837)", "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    check_getters_while_sync_holds(rig, {0x10, 0x07}, true);  // southern site: tracking-on rate command
}

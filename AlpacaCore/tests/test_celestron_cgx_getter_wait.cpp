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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <utility>

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

namespace {
// Runs `start` while the first reply matching `filter` is held half-sent, then probes Tracking, AtHome and GuideRate:
// each answers inside the probe window without throwing (decision 0016 rule 1), and Tracking serves the state from
// before the held exchange, never a transient unknown.
template <typename Start>
void check_tracking_getters_while_held(GetterWaitRig& rig, std::pair<std::uint8_t, std::uint8_t> filter,
                                       bool tracking_before, Start start) {
    rig.mount.fragment_next_reply(filter);
    auto holder = std::async(std::launch::async, start);
    // A motion body sleeps on the fake clock before it reaches the held exchange; keep it moving.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool held = false;
    while (!held && std::chrono::steady_clock::now() < deadline) {
        held = rig.mount.wait_for_reply_fragment(5ms);
        if (!held) rig.clock.advance(20ms);
    }
    if (!held) rig.mount.release_reply_tail();
    REQUIRE(held);
    auto tracking = std::async(std::launch::async, [&] { return rig.driver->get_tracking(); });
    auto at_home = std::async(std::launch::async, [&] { return rig.driver->get_at_home(); });
    auto guide = std::async(std::launch::async, [&] { return rig.driver->get_guide_rate(); });
    const auto probe_deadline = std::chrono::steady_clock::now() + 300ms;
    const bool tracking_done = tracking.wait_until(probe_deadline) == std::future_status::ready;
    const bool at_home_done = at_home.wait_until(probe_deadline) == std::future_status::ready;
    const bool guide_done = guide.wait_until(probe_deadline) == std::future_status::ready;
    rig.mount.release_reply_tail();  // always release so the test cannot hang
    alpacacore::test::drain_cgx_clock_task(holder, rig.clock);
    try {
        holder.get();
    } catch (const std::exception&) {  // may time out once held past 500 ms
    }
    CHECK(tracking_done);
    CHECK(at_home_done);
    CHECK(guide_done);
    CHECK(tracking.get() == tracking_before);
    CHECK_NOTHROW(at_home.get());
    CHECK_NOTHROW(guide.get());
}

// One hour east of the meridian: the slew keeps the pointing side, so no home route runs first.
double target_ra(GetterWaitRig& rig) { return std::fmod(rig.driver->get_sidereal_time() + 23.0, 24.0); }
}  // namespace

TEST_CASE("CGX Tracking getters return while Tracking waits on its ACK (#1759)", "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    check_tracking_getters_while_held(rig, {0x10, 0x07}, false, [&] { rig.driver->set_tracking(true); });
}

TEST_CASE("CGX Tracking getters return while TrackingRate waits on its ACK (#1759)", "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.driver->set_tracking(true);
    check_tracking_getters_while_held(rig, {0x10, 0x07}, true, [&] { rig.driver->set_tracking_rate(1); });
}

TEST_CASE("CGX Tracking getters return while AbortSlew waits on a stop ACK (#1759)", "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.driver->set_tracking(true);
    check_tracking_getters_while_held(rig, {0x10, 0x24}, true, [&] { rig.driver->abort_slew(); });
}

TEST_CASE("CGX Tracking getters return while FindHome waits on its tracking ACK (#1759)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.driver->set_tracking(true);
    check_tracking_getters_while_held(rig, {0x10, 0x06}, true, [&] { rig.driver->find_home(); });
}

TEST_CASE("CGX Tracking getters return while a slew start waits on its tracking ACK (#1759)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.driver->set_tracking(true);
    const double ra = target_ra(rig);
    check_tracking_getters_while_held(rig, {0x10, 0x06}, true,
                                      [&] { rig.driver->slew_to_coordinates_async(ra, 20.0); });
}

TEST_CASE("CGX Tracking getters return while slew arrival waits on its tracking ACK (#1759)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.driver->set_tracking(true);
    const double ra = target_ra(rig);
    check_tracking_getters_while_held(rig, {0x10, 0x07}, false,
                                      [&] { rig.driver->slew_to_coordinates_async(ra, 20.0); });
}

TEST_CASE("CGX Tracking getters return while slew arrival waits on a slow correction ACK (#1759)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.driver->set_tracking(true);
    rig.mount.set_auto_complete_motions(false);
    const double ra = target_ra(rig);
    const auto last_goto = [&](std::uint8_t axis) {
        const auto frames = rig.mount.requests();
        const auto found = std::find_if(frames.rbegin(), frames.rend(), [axis](const auto& frame) {
            return frame.destination == axis && frame.command == 0x02;
        });
        REQUIRE(found != frames.rend());
        return (static_cast<std::uint32_t>(found->payload[0]) << 16) |
               (static_cast<std::uint32_t>(found->payload[1]) << 8) | found->payload[2];
    };
    check_tracking_getters_while_held(rig, {0x10, 0x17}, false, [&] {
        rig.driver->slew_to_coordinates_async(ra, 20.0);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (rig.mount.count_command(0x11, 0x02) == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        // RA stops about 21 arcsec short: outside the 3-arcsec arrival tolerance, inside the 1-degree correction.
        rig.mount.complete_axis_slew_at(0x10, last_goto(0x10) + 1000);
        rig.mount.complete_axis_slew_at(0x11, last_goto(0x11));
    });
}

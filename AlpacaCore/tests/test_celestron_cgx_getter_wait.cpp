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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, [this] { return utc; });
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

    // The getters a client polls, coordinates included (#1759, #1940): each answers from the polled frame or
    // driver state inside one probe window while the wire is held, and none throws. Releases the held reply.
    void check_served_getters_return() {
        const std::vector<std::function<void()>> getters{
            [&] { static_cast<void>(driver->get_name()); },
            [&] { static_cast<void>(driver->get_connected()); },
            [&] { static_cast<void>(driver->get_right_ascension()); },
            [&] { static_cast<void>(driver->get_declination()); },
            [&] { static_cast<void>(driver->get_altitude()); },
            [&] { static_cast<void>(driver->get_azimuth()); },
            [&] { static_cast<void>(driver->get_side_of_pier()); },
            [&] { static_cast<void>(driver->get_is_pulse_guiding()); },
            [&] {  // no Catch assertion off the test thread: the throw reaches CHECK_NOTHROW below
                if (driver->get_device_state().empty()) throw std::runtime_error("DeviceState is empty");
            }};
        std::vector<std::future<void>> probes;
        for (const auto& getter : getters) probes.push_back(std::async(std::launch::async, getter));
        const auto deadline = std::chrono::steady_clock::now() + 300ms;
        std::vector<bool> done;
        for (auto& probe : probes) done.push_back(probe.wait_until(deadline) == std::future_status::ready);
        mount.release_reply_tail();  // always release so the test cannot hang
        for (auto& probe : probes)
            while (probe.wait_for(2ms) != std::future_status::ready) clock.advance(20ms);
        for (std::size_t i = 0; i < probes.size(); ++i) {
            INFO("getter " << i);
            CHECK(done[i]);
            CHECK_NOTHROW(probes[i].get());
        }
    }

    // Holds the state poll's RA position read (the only reader of the axes besides the control paths).
    std::future<void> hold_poll_position_read() {
        hold_ra_position_reply();
        auto poll =
            std::async(std::launch::async, [&] { alpacacore::vendor::celestron::poll_state_now_for_testing(*driver); });
        const bool held = mount.wait_for_reply_fragment(2s);
        if (!held) mount.release_reply_tail();
        REQUIRE(held);
        return poll;
    }
};
}  // namespace

TEST_CASE("CGX state-only getters return while a position read waits on serial (#1837)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    // The getters no longer read the axes (#1940): the state poll's read is the one held.
    auto poll = rig.hold_poll_position_read();
    rig.check_state_getters_return();
    poll.get();
}

TEST_CASE("CGX coordinate getters answer from the frame while the poll's position read waits on serial (#1940)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    auto poll = rig.hold_poll_position_read();
    const auto requests = rig.mount.requests();
    CHECK(std::count_if(requests.begin(), requests.end(),
                        [](const auto& frame) { return frame.destination == 0x10 && frame.command == 0x01; }) > 0);
    rig.check_served_getters_return();
    poll.get();
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

TEST_CASE("CGX coordinate getters answer from the frame while Sync waits on its settle read (#1940)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x01});
    auto sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(3.0, 20.0); });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool held = false;
    while (!held && std::chrono::steady_clock::now() < deadline) {
        held = rig.mount.wait_for_reply_fragment(5ms);
        if (!held) rig.clock.advance(20ms);
    }
    if (!held) rig.mount.release_reply_tail();
    REQUIRE(held);
    rig.check_served_getters_return();
    while (sync.wait_for(5ms) != std::future_status::ready) rig.clock.advance(20ms);
    CHECK_NOTHROW(sync.get());
}

TEST_CASE("CGX coordinate getters answer from the frame while Tracking waits on its ACK (#1940)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    rig.mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x07});
    auto tracking = std::async(std::launch::async, [&] { rig.driver->set_tracking(true); });
    const bool held = rig.mount.wait_for_reply_fragment(2s);
    if (!held) rig.mount.release_reply_tail();
    REQUIRE(held);
    rig.check_served_getters_return();
    alpacacore::test::drain_cgx_clock_task(tracking, rig.clock);
    CHECK_NOTHROW(tracking.get());
}

TEST_CASE("CGX IsPulseGuiding answers from driver state and reads no guide status (#1940)",
          "[celestron][cgx][getter-wait]") {
    GetterWaitRig rig;
    const auto guide_reads = [&] { return rig.mount.count_command(0x10, 0x27) + rig.mount.count_command(0x11, 0x27); };
    const int idle = guide_reads();
    for (int read = 0; read < 3; ++read) CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    static_cast<void>(rig.driver->get_device_state());
    CHECK(guide_reads() == idle);
    // During a guide whose status reply is held, the getter still answers at once.
    rig.driver->set_tracking(true);
    rig.mount.set_hold_guide_until_completed(true);
    rig.mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x11, 0x27});
    rig.driver->pulse_guide(0, 100);
    const bool held = rig.mount.wait_for_reply_fragment(2s);
    if (!held) rig.mount.release_reply_tail();
    REQUIRE(held);
    const int during = guide_reads();
    auto guiding = std::async(std::launch::async, [&] { return rig.driver->get_is_pulse_guiding(); });
    const bool answered = guiding.wait_for(300ms) == std::future_status::ready;
    rig.mount.release_reply_tail();
    rig.mount.complete_axis_guide(0x11);
    while (guiding.wait_for(2ms) != std::future_status::ready) rig.clock.advance(20ms);
    CHECK(answered);
    CHECK(guiding.get());
    CHECK(guide_reads() == during);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (rig.driver->get_is_pulse_guiding() && std::chrono::steady_clock::now() < deadline) rig.clock.advance(20ms);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
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
    // Runs on the holder thread, so it records a missing GOTO for the test thread instead of asserting there.
    std::atomic<bool> gotos_found{true};
    const auto last_goto = [&](std::uint8_t axis) -> std::uint32_t {
        const auto frames = rig.mount.requests();
        const auto found = std::find_if(frames.rbegin(), frames.rend(), [axis](const auto& frame) {
            return frame.destination == axis && frame.command == 0x02;
        });
        if (found == frames.rend()) {
            gotos_found = false;
            return 0;
        }
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
    CHECK(gotos_found);
}

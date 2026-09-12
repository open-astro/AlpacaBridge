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

// Connect/disconnect/operate concurrency stress for the ToupTek drivers
// (issue #101) — the drivers with the deepest race history (PR #99). Runs
// hardware-free over the fault-injectable SDK seam (issue #104), wrapped in
// LockedToupTekSDK so ThreadSanitizer findings point at DRIVER code, not at
// the deliberately unhardened test fake.
//
// These pass on a correct driver under any build; their teeth come from the
// sanitizers-tsan CI job (and RUN_TSAN=1 ./scripts/ci_preflight.sh locally),
// which turns a latent data race into a hard failure.

#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>

#include <atomic>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_touptek_sdk.h"
#include "locked_touptek_sdk.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeToupTekSDK;
using alpacacore::test::LockedToupTekSDK;
using alpacacore::test::StressOptions;

namespace {

FakeToupTekSDK make_fake_with_camera() {
    FakeToupTekSDK fake;
    fake.cameras.push_back(FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    return fake;
}

FakeToupTekSDK make_fake_with_wheel() {
    FakeToupTekSDK fake;
    FakeToupTekSDK::ToupFilterWheelInfo wheel;
    wheel.id = "fake-afw-0";
    wheel.name = "FakeAFW";
    wheel.model_name = "AFW-M";
    fake.wheels.push_back(wheel);
    // Home settles after one in-motion read; the trailing 0 repeats, so every
    // reconnect's homing poll completes quickly and the stress loop gets many
    // full connect cycles (including mid-homing disconnects) per window.
    fake.wheel_position_script = {-1, 0, 0, 0};
    return fake;
}

FakeToupTekSDK make_fake_with_focuser() {
    FakeToupTekSDK fake;
    FakeToupTekSDK::ToupFocuserInfo focuser;
    focuser.id = "fake-aaf-0";
    focuser.name = "FakeAAF";
    focuser.model_name = "AAF";
    fake.focusers.push_back(focuser);
    return fake;
}

}  // namespace

TEST_CASE("ToupTek camera - concurrent connect/disconnect/operate stress", "[touptek][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, sdk);

    // open-astro#326: one guard per call. run_lifecycle_stress wraps the WHOLE
    // callback in a single try/catch, so a storm racing a disconnect used to
    // exercise get_camera_state() and skip the other five outright. This file
    // is the one AGENTS.md points new authors at for the overall shape, which
    // is why it had the most to gain from being fixed.
    // The set REPLACES the default {NotConnected} rather than extending it.
    // InvalidOperation belongs here because this callback both starts
    // exposures and sets gain: once the calls are individually guarded, a
    // set_gain() landing while a PREVIOUS iteration's exposure is still
    // running hits the driver's "Cannot change camera settings during an
    // exposure" guard, which is the ASCOM contract working, not a defect.
    // Unguarded, that throw was invisible -- it aborted the callback instead.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& cam = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { static_cast<void>(cam.get_camera_state()); });
        guard([&] { static_cast<void>(cam.get_gain()); });
        guard([&] { cam.set_gain(100); });
        guard([&] { static_cast<void>(cam.get_readout_mode()); });
        guard([&] { cam.start_exposure(0.001, true); });
        guard([&] { cam.abort_exposure(); });
    });

    // The driver must still be usable after the storm, and the ref-counted
    // open ledger must balance once we settle it disconnected.
    CHECK(alpacacore::test::settle_connected(*driver, true));
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ToupTek camera - destruction races an in-flight connect", "[touptek][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::touptek::create_touptek_camera(0, 0, sdk); });
    // Every destructor joined its task; no open can outlive its driver.
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek camera - racing disconnect is never dropped", "[touptek][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, sdk);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
    CHECK(fake.ref_count("fake-cam-0") == 0);
}

TEST_CASE("ToupTek camera - disconnect racing a NO-OP connect is never dropped (round-4)",
          "[touptek][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, sdk);
    // connect() on an already-connected device + immediate disconnect(): the
    // no-op connect task must not eat the recorded disconnect.
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false) == false);
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
}

TEST_CASE("ToupTek camera - SYNC disconnect racing a NO-OP connect is never undone (round-6)",
          "[touptek][camera][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_camera(0, 0, sdk);
    // connect() on a connected device + sync set_connected(false): the record
    // must be left for the no-op task even though the sync caller tears down
    // the hardware itself — otherwise the task RECONNECTS at its idempotency
    // fall-through and the explicit disconnect is silently undone.
    CHECK(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true) == false);
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
}

TEST_CASE("ToupTek AFW - concurrent connect/disconnect/operate stress", "[touptek][filterwheel][stress]") {
    auto fake = make_fake_with_wheel();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", sdk);

    // The AFW's homing poll releases the driver mutex mid-connect — the widest
    // sync-connect window in the codebase and the home of PR #99's worst bugs.
    // Give it a longer window so many full home cycles interleave the storm.
    StressOptions opt;
    opt.duration = std::chrono::milliseconds(1500);
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(
        *driver,
        [&guard](AlpacaDriver& d) {
            auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
            guard([&] { static_cast<void>(wheel.get_position()); });
            guard([&] { wheel.set_position(1); });
            guard([&] { static_cast<void>(wheel.get_names()); });
        },
        opt);

    CHECK(alpacacore::test::settle_connected(*driver, true));
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.ref_count("fake-afw-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ToupTek AFW - destruction races an in-flight connect", "[touptek][filterwheel][stress]") {
    auto fake = make_fake_with_wheel();
    LockedToupTekSDK sdk(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", sdk); },
        // Each iteration can ride out a full homing poll; keep the count low
        // enough that the case stays in CI-friendly territory under TSan.
        30);
    CHECK(fake.ref_count("fake-afw-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek AFW - racing disconnect is never dropped (homing window)", "[touptek][filterwheel][stress]") {
    auto fake = make_fake_with_wheel();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_filterwheel_by_id(0, "fake-afw-0", sdk);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
    CHECK(fake.ref_count("fake-afw-0") == 0);
}

TEST_CASE("ToupTek AAF focuser - concurrent connect/disconnect/operate stress", "[touptek][focuser][stress]") {
    auto fake = make_fake_with_focuser();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", sdk);

    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        guard([&] { static_cast<void>(focuser.get_is_moving()); });
        guard([&] { static_cast<void>(focuser.get_position()); });
        guard([&] { static_cast<void>(focuser.get_max_step()); });
        guard([&] { static_cast<void>(focuser.get_max_increment()); });
        guard([&] { static_cast<void>(focuser.get_temperature()); });
        guard([&] { focuser.move(1234); });
        guard([&] { focuser.halt(); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, true));
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.ref_count("fake-aaf-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ToupTek AAF focuser - destruction races an in-flight connect", "[touptek][focuser][stress]") {
    auto fake = make_fake_with_focuser();
    LockedToupTekSDK sdk(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", sdk); });
    CHECK(fake.ref_count("fake-aaf-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek AAF focuser - racing disconnect is never dropped", "[touptek][focuser][stress]") {
    auto fake = make_fake_with_focuser();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_focuser_by_id(0, "fake-aaf-0", sdk);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
    CHECK(fake.ref_count("fake-aaf-0") == 0);
}

TEST_CASE("ToupTek thermal switch - concurrent connect/disconnect/operate stress", "[touptek][switch][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto driver = alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0, sdk);

    alpacacore::test::StressCallGuard guard;
    // A throw from get_max_switch() leaves max_switch at 0 and skips the
    // per-switch loop for that iteration; total_calls() still counts the two
    // calls outside it, so coverage could erode with the case green. The
    // per-switch tally pins that the loop ran at least once in the storm.
    std::atomic<int> switch_calls{0};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard, &switch_calls](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        // get_max_switch() drives the loop bound, so it is guarded with a
        // local default rather than inline: a throw here must not skip the
        // loop AND the device-state read below it.
        int max_switch = 0;
        guard([&] { max_switch = sw.get_max_switch(); });
        for (int id = 0; id < max_switch; ++id) {
            guard([&] { static_cast<void>(sw.get_switch_value(id)); });
            guard([&] { sw.set_switch_value(id, 1.0); });
            switch_calls.fetch_add(1, std::memory_order_relaxed);
        }
        guard([&] { static_cast<void>(sw.get_device_state()); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, true));
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(switch_calls.load() > 0);
}

TEST_CASE("ToupTek thermal switch - destruction races an in-flight connect", "[touptek][switch][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    alpacacore::test::run_destruction_during_connect_stress(
        [&]() { return alpacacore::vendor::touptek::create_touptek_thermal_switch(0, 0, sdk); });
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("ToupTek camera + thermal switch - shared open under concurrent lifecycle storms",
          "[touptek][camera][switch][stress]") {
    auto fake = make_fake_with_camera();
    LockedToupTekSDK sdk(fake);
    auto camera = alpacacore::vendor::touptek::create_touptek_camera(0, 0, sdk);
    auto thermal = alpacacore::vendor::touptek::create_touptek_thermal_switch(1, 0, sdk);

    // Two drivers share one ref-counted physical open; storm both at once so
    // open/close interleavings from different drivers hit the shared ledger.
    // One guard per driver: StressCallGuard is itself thread-safe, but keeping
    // them separate means a failure names which of the two drivers sharing the
    // handle produced it.
    alpacacore::test::StressCallGuard camera_guard;
    alpacacore::test::StressCallGuard thermal_guard;
    std::thread camera_storm([&]() {
        alpacacore::test::run_lifecycle_stress(*camera, [&camera_guard](AlpacaDriver& d) {
            auto& cam = static_cast<alpacacore::CameraDriver&>(d);
            camera_guard([&] { static_cast<void>(cam.get_camera_state()); });
            camera_guard([&] { static_cast<void>(cam.get_gain()); });
        });
    });
    alpacacore::test::run_lifecycle_stress(*thermal, [&thermal_guard](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        thermal_guard([&] { static_cast<void>(sw.get_max_switch()); });
        thermal_guard([&] { static_cast<void>(sw.get_device_state()); });
    });
    camera_storm.join();

    CHECK(alpacacore::test::settle_connected(*camera, false));
    CHECK(alpacacore::test::settle_connected(*thermal, false));
    CHECK(fake.ref_count("fake-cam-0") == 0);
    CHECK(fake.physical_opens == fake.physical_closes);
    CHECK(fake.underflow_closes == 0);

    INFO(camera_guard.report());
    CHECK(camera_guard.unexpected_count() == 0);
    CHECK(camera_guard.total_calls() > 0);
    INFO(thermal_guard.report());
    CHECK(thermal_guard.unexpected_count() == 0);
    CHECK(thermal_guard.total_calls() > 0);
}

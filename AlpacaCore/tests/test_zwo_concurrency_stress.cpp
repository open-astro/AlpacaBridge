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

// Connect/disconnect/operate concurrency stress for the ZWO EFW filter wheel
// (issue #101). The cases built on the real SDK singleton find no wheel on a
// hardware-free host, so every connect fails fast at enumeration — which still
// storms the AsyncConnectable machinery, the failure-path cleanup, and the
// property getters racing the lifecycle: the exact surfaces where this driver
// family's review findings lived. With a wheel attached the same tests exercise
// the full connect path; the case over the EFW SDK seam at the end does so with
// no hardware.

#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <atomic>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_zwo_caa_sdk.h"
#include "fake_zwo_eaf_sdk.h"
#include "fake_zwo_efw_sdk.h"
#include "fake_zwo_mount.h"
#include "fake_zwo_sdk.h"
#include "locked_zwo_caa_sdk.h"
#include "locked_zwo_eaf_sdk.h"
#include "locked_zwo_efw_sdk.h"
#include "locked_zwo_sdk.h"
#include "rotator_sync_offset_test_dir.h"

using alpacacore::AlpacaDriver;

TEST_CASE("ZWO EFW - concurrent connect/disconnect/operate stress", "[zwo][filterwheel][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        guard([&] { static_cast<void>(wheel.get_position()); });
        guard([&] { wheel.set_position(1); });
        guard([&] { static_cast<void>(wheel.get_names()); });
        guard([&] { static_cast<void>(wheel.get_focus_offsets()); });
    });

    // Still alive and coherent after the storm (Connected reflects whether a
    // physical wheel is attached; both outcomes are valid here).
    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO EFW - destruction races an in-flight connect", "[zwo][filterwheel][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0); });
}

// Camera lifecycle storm (issue #116): the ZWO camera's operational calls
// were converted from snapshot-then-call to the held-mutex_ shape, and its
// disconnect now publishes disconnected before the SDK close. On a
// hardware-free host every connect fails fast at enumeration, which still
// storms the connect-failure cleanup, the with_camera gate, and the
// disconnect ordering from many threads; with a camera attached the same
// test exercises the full path.
TEST_CASE("ZWO camera - concurrent connect/disconnect/operate stress", "[zwo][camera][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0);

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { static_cast<void>(camera.get_camera_state()); });
        guard([&] { static_cast<void>(camera.get_ccd_temperature()); });
        guard([&] { camera.set_gain(50); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { camera.stop_exposure(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO camera - destruction races an in-flight connect", "[zwo][camera][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0); });
}

TEST_CASE("ZWO camera - connected SDK acquisition lifecycle stress", "[zwo][camera][stress]") {
    alpacacore::test::FakeZWOSDK sdk;
    sdk.frame_bytes.resize(16 * 8 * 2, 0x5a);
    auto driver = alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { camera.start_exposure(0.001, true); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { static_cast<void>(camera.get_last_exposure_duration()); });
        guard([&] { static_cast<void>(camera.get_last_exposure_start_time()); });
        guard([&] { static_cast<void>(camera.get_image_array()); });
        guard([&] { camera.stop_exposure(); });
        guard([&] { static_cast<void>(camera.get_camera_state()); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(sdk.open_count() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(sdk.call_count("get_data_after_exposure") > 0);
}

// Same storm over the serialised seam (issue #1295): LockedZWOSDK around FakeZWOSDK, so every SDK call
// is one mutex-guarded forward and the driver, not the fake, owns the interleaving.
TEST_CASE("ZWO camera - connected lifecycle stress behind LockedZWOSDK", "[zwo][camera][stress]") {
    alpacacore::test::FakeZWOSDK fake;
    fake.frame_bytes.resize(16 * 8 * 2, 0x5a);
    alpacacore::test::LockedZWOSDK sdk(fake);
    auto driver = alpacacore::vendor::zwo::create_zwo_camera(0, fake.camera.camera_id, sdk);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        guard([&] { camera.start_exposure(0.001, true); });
        guard([&] { static_cast<void>(camera.get_image_ready()); });
        guard([&] { static_cast<void>(camera.get_last_exposure_duration()); });
        guard([&] { static_cast<void>(camera.get_last_exposure_start_time()); });
        guard([&] { static_cast<void>(camera.get_image_array()); });
        guard([&] { camera.stop_exposure(); });
        guard([&] { static_cast<void>(camera.get_camera_state()); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.open_count() == 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(fake.call_count("get_data_after_exposure") > 0);
}

// Focuser stress (#271): same shape as the EFW/camera cases above — on the
// real SDK singleton a hardware-free host has no focuser, so every connect
// fails fast at enumeration, which still storms the AsyncConnectable
// machinery and the failure-path cleanup. With a focuser attached the same
// test exercises the full connect path.
TEST_CASE("ZWO EAF focuser - concurrent connect/disconnect/operate stress", "[zwo][focuser][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        guard([&] { static_cast<void>(focuser.get_position()); });
        guard([&] { static_cast<void>(focuser.get_temperature()); });
        guard([&] { focuser.move(100); });
        guard([&] { focuser.halt(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO EAF focuser - destruction races an in-flight connect", "[zwo][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0); });
}

// Rotator stress (#271): same shape — the real CAA SDK has no rotator on a hardware-free host.
TEST_CASE("ZWO CAA rotator - concurrent connect/disconnect/operate stress", "[zwo][rotator][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& rotator = static_cast<alpacacore::RotatorDriver&>(d);
        guard([&] { static_cast<void>(rotator.get_position()); });
        guard([&] { static_cast<void>(rotator.get_reverse()); });
        guard([&] { rotator.move(10.0); });
        guard([&] { rotator.halt(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO CAA rotator - destruction races an in-flight connect", "[zwo][rotator][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0); });
}

// Connected lifecycle storms over the EFW, EAF and CAA SDK seams (open-astro#1309): the cases above
// stop at the connect-failure path on a host with no device, so the poll, the status cache and the
// disconnect ordering never ran with a connect that succeeds. Each fake sits behind its Locked
// decorator, so every SDK call is one mutex-guarded forward and the driver owns the interleaving.
// Connected drivers legitimately answer InvalidValue (a slot or position out of range) and
// InvalidOperation (a move during a move), so those join NotConnected in the expected set. The
// EFW fake scripts the moving window (Position -1 for two reads after a move), so a move issued
// during another is refused instead of each one settling at once.
TEST_CASE("ZWO EFW - connected lifecycle stress behind LockedZWOEFWSDK", "[zwo][filterwheel][stress]") {
    alpacacore::test::FakeZWOEFWSDK fake;
    fake.moving_reads = 2;
    alpacacore::test::LockedZWOEFWSDK sdk(fake);
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0, sdk);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidValue,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        guard([&] { static_cast<void>(wheel.get_position()); });
        guard([&] { wheel.set_position(1); });
        guard([&] { static_cast<void>(wheel.get_names()); });
        guard([&] { static_cast<void>(wheel.get_focus_offsets()); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.open_calls == fake.close_calls);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(fake.set_position_calls > 0);
    CHECK(fake.moving_refusals > 0);
}

TEST_CASE("ZWO EAF focuser - connected lifecycle stress behind LockedZWOEAFSDK", "[zwo][focuser][stress]") {
    alpacacore::test::FakeZWOEAFSDK fake;
    alpacacore::test::LockedZWOEAFSDK sdk(fake);
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0, sdk);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidValue,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        guard([&] { static_cast<void>(focuser.get_position()); });
        guard([&] { static_cast<void>(focuser.get_temperature()); });
        guard([&] { focuser.move(100); });
        guard([&] { focuser.halt(); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.open_calls == fake.close_calls);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(fake.position_calls > 0);
}

TEST_CASE("ZWO CAA rotator - connected lifecycle stress behind LockedZWOCAASDK", "[zwo][rotator][stress]") {
    alpacacore::test::TempSyncOffsetDir offsets;
    alpacacore::test::FakeZWOCAASDK fake;
    alpacacore::test::LockedZWOCAASDK sdk(fake);
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0, sdk);
    REQUIRE(alpacacore::test::settle_connected(*driver, true));

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::InvalidValue,
                                            alpacacore::AlpacaError::InvalidOperation};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& rotator = static_cast<alpacacore::RotatorDriver&>(d);
        guard([&] { static_cast<void>(rotator.get_position()); });
        guard([&] { static_cast<void>(rotator.get_reverse()); });
        guard([&] { rotator.move(10.0); });
        guard([&] { rotator.halt(); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(fake.open_calls == fake.close_calls);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK(fake.degree_calls > 0);
}

// Dew heater switch stress (#271): same shape — the dew heater switch wraps
// a camera SDK handle (ASIGetControlValue/ASISetControlValue for the heater
// duty cycle), so it fails fast at camera enumeration on a hardware-free
// host exactly like the ZWO camera driver above. This is the representative
// case for the [zwo][switch] pair: the two GPIO-backed ASIAIR switch drivers
// (zwo_asiair_switch_driver.h, zwo_asiair_plus_switch_driver.h) are a
// separate seam (libgpiod, not this SDK) and are not covered by this case.
TEST_CASE("ZWO dew heater switch - concurrent connect/disconnect/operate stress", "[zwo][switch][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        guard([&] { static_cast<void>(sw.get_max_switch()); });
        guard([&] { static_cast<void>(sw.get_switch(0)); });
        guard([&] { sw.set_switch(0, true); });
        guard([&] { static_cast<void>(sw.get_switch_value(0)); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO dew heater switch - destruction races an in-flight connect", "[zwo][switch][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0); });
}

// Dew heater switch stress over a fake SDK (open-astro#1316): the registrations
// above fail fast at enumeration on the real SDK, so the connect path that
// matters (camera listing under mutex_, the open in its own try block, the
// status cache the getters and setter hit) never ran under TSan. Here connect
// succeeds against FakeDewHeaterSdk, which is thread-safe by construction.
namespace {

void add_fake_dew_camera(alpacacore::test::FakeDewHeaterSdk& sdk) {
    alpacacore::vendor::zwo::ZWOCameraInfo cam;
    cam.camera_id = 9;
    cam.slot_index = 0;
    cam.name = "ZWO ASI2600MM Pro";
    sdk.cameras = {cam};
}

}  // namespace

TEST_CASE("ZWO dew heater switch over a fake SDK - concurrent connect/disconnect/operate stress",
          "[zwo][switch][stress]") {
    alpacacore::test::FakeDewHeaterSdk sdk;
    add_fake_dew_camera(sdk);
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0, sdk);

    // Connected registration: a disconnect racing a getter clears the heater
    // capability after ensure_connected() passed, so the getter answers
    // NotImplemented ("Dew heater not supported", 0x400) instead of NotConnected
    // (seen ~90 times per run with the default guard).
    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::NotImplemented});
    std::atomic<bool> saw_connected{false};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard, &saw_connected](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        if (sw.get_connected()) {
            saw_connected.store(true);
        }
        guard([&] { static_cast<void>(sw.get_max_switch()); });
        guard([&] { static_cast<void>(sw.get_switch(0)); });
        guard([&] { sw.set_switch(0, true); });
        guard([&] { static_cast<void>(sw.get_switch_value(0)); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    // A connect reached the end of the fake-backed path (serial read is its last
    // SDK call), so the case cannot silently fall back to the fail-fast one.
    CHECK(sdk.serial_reads > 0);
    CHECK(saw_connected.load());
}

TEST_CASE("ZWO dew heater switch over a fake SDK - destruction races an in-flight connect", "[zwo][switch][stress]") {
    alpacacore::test::FakeDewHeaterSdk sdk;
    add_fake_dew_camera(sdk);
    alpacacore::test::run_destruction_during_connect_stress(
        [&sdk]() { return alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0, sdk); });
}

// Telescope stress (audit follow-up, 3.0.1): the ZWO mount driver's worst
// review findings were detached-thread lifecycle bugs — the GOTO setup
// thread, the async disconnect teardown thread, and the poll/pulse threads
// racing set_connected(false) and destruction. Those threads only exist on a
// CONNECTED driver, so these tests connect through a FakeZwoMount (the
// mount supplies the mandatory AM identity and valid telemetry).
// The operate callback drives the exact newly-fixed paths: async GOTO and
// pulse guiding racing disconnects.
#ifndef _WIN32

namespace {

alpacacore::vendor::zwo::ConnectionInfo zwo_endpoint(int port) {
    alpacacore::vendor::zwo::ConnectionInfo info;
    info.type = alpacacore::vendor::zwo::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 250;
    return info;
}

// The AM mount for the telescope storms below, on the real clock: the storms exercise scheduling and lock order, and
// a wait on a driver timer is the driver's own (the fake's GOTO and park end at once, so a storm never waits for a
// slew). The fake reports tracking on, so PulseGuide gets past its tracking gate and queues work onto the pulse
// thread, and a non-zero guide rate (open-astro#714): with a zero rate the driver read a zero guide rate at connect
// and returned from pulse_guide() before ever touching pulse_mutex_, so the storm never exercised the
// pulse_mutex_ -> mutex_ acquisition that inverts the connect path's mutex_ -> pulse_mutex_ order, and TSan had
// nothing to report. ":GVP" supplies the mandatory AM identity, and ":MS" accepts, so the GOTO thread completes its
// protocol round-trip too.
void configure_storm_mount(alpacacore::test::FakeZwoMount& mount) { mount.set_slew_duration(std::chrono::seconds(0)); }

}  // namespace

TEST_CASE("ZWO mount - concurrent connect/disconnect/slew/pulse stress", "[zwo][telescope][stress]") {
    alpacacore::test::FakeZwoMount mount(alpacacore::util::default_task_clock(), "AM5");
    configure_storm_mount(mount);
    mount.override_reply(":SMMC", "e2");  // Exercise the legacy target transaction during lifecycle races.
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(mount.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(driver->get_tracking());

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    // Unlike the other five storms in this file, this one runs CONNECTED over
    // FakeZwoMount, so the set is widened per ci-gates.instructions.md. The set REPLACES
    // the default {NotConnected}. DriverException is here for the storm rather
    // than a defect: a getter that races a link teardown reports its
    // unavailable telemetry as DriverException.
    // InvalidValue/InvalidOperation cover a slew or pulse racing another
    // motion. A real defect shows up as a code outside this set, and
    // guard.report() names every distinct one it saw.
    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::NotImplemented,
        alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
        guard([&] { static_cast<void>(scope.get_tracking()); });
        guard([&] { static_cast<void>(scope.get_right_ascension()); });
        guard([&] { static_cast<void>(scope.get_declination()); });
        guard([&] { static_cast<void>(scope.get_slewing()); });
        guard([&] { static_cast<void>(scope.get_utc_date()); });
        guard([&] { scope.set_utc_date(std::chrono::system_clock::now()); });
        // The newly-fixed thread paths: async GOTO (goto_thread_) and pulse
        // guiding (pulse thread queue) issued while other threads disconnect.
        guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
        guard([&] { scope.sync_to_coordinates(6.0, 25.0); });
        guard([&] { scope.set_target_declination(30.0); });
        guard([&] { scope.pulse_guide(0, 50); });
        // open-astro#714: Connected=true on an already-connected driver takes
        // mutex_ and then pulse_mutex_ (the pulse-queue clear); pulse_guide()
        // above takes them in the opposite order. Racing the two from the
        // storm is what makes the sanitizers-tsan job report the
        // lock-order-inversion (and what deadlocks a PHD2 guiding session
        // against a client's Connect on hardware).
        guard([&] { scope.set_connected(true); });
        guard([&] { scope.abort_slew(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO mount - sibling lifecycle storms cannot redirect a connected telescope",
          "[zwo][telescope][stress][isolation]") {
    using namespace alpacacore;
    test::FakeZwoMount a(util::default_task_clock(), "AM5");
    test::FakeZwoMount b(util::default_task_clock(), "AM5N");
    configure_storm_mount(a);
    configure_storm_mount(b);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    auto stable = vendor::zwo::create_zwo_telescope(3, zwo_endpoint(a.port()));
    auto changing = vendor::zwo::create_zwo_telescope(7, zwo_endpoint(b.port()));
    REQUIRE(test::settle_connected(*stable, true));
    REQUIRE(test::settle_connected(*changing, true));
    REQUIRE(stable->command_string(":GVP", false) == "AM5");
    REQUIRE(changing->command_string(":GVP", false) == "AM5N");
    test::StressCallGuard stable_guard({});  // The stable entry has no legitimate disconnect/error.
    test::StressCallGuard guard{AlpacaError::NotConnected, AlpacaError::InvalidValue, AlpacaError::InvalidOperation,
                                AlpacaError::DriverException};
    std::atomic<int> wrong_endpoint{0}, accepted{0};
    std::jthread reader([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            stable_guard([&] {
                if (stable->command_string(":GVP", false) != "AM5") ++wrong_endpoint;
                ++accepted;
            });
            stable_guard([&] { static_cast<void>(stable->get_right_ascension()); });
            stable_guard([&] { stable->set_target_right_ascension(6.0); });
        }
    });
    test::run_lifecycle_stress(*changing, [&](AlpacaDriver& d) {
        auto& scope = static_cast<TelescopeDriver&>(d);
        guard([&] {
            if (scope.command_string(":GVP", false) != "AM5N") ++wrong_endpoint;
        });
        guard([&] { static_cast<void>(scope.get_right_ascension()); });
        guard([&] { scope.set_target_right_ascension(9.0); });
        guard([&] { scope.abort_slew(); });
    });
    reader.request_stop();
    reader.join();
    CHECK(test::settle_connected(*changing, false));
    changing.reset();
    CHECK(stable->command_string(":GVP", false) == "AM5");
    CHECK(stable->get_connected());
    CHECK(wrong_endpoint.load() == 0);
    CHECK(accepted.load() > 0);
    INFO(stable_guard.report());
    CHECK(stable_guard.unexpected_count() == 0);
    CHECK(stable_guard.total_calls() > 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO mount - destruction races an in-flight connect", "[zwo][telescope][stress]") {
    alpacacore::test::FakeZwoMount mount(alpacacore::util::default_task_clock(), "AM5");
    configure_storm_mount(mount);
    REQUIRE(mount.ok());
    const int port = mount.port();
    {
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(port));
        REQUIRE(alpacacore::test::settle_connected(*driver, true));
        REQUIRE(driver->get_tracking());
    }
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(port)); });
}

TEST_CASE("ZWO mount - destruction mid-operation (GOTO/pulse threads live)", "[zwo][telescope][stress]") {
    alpacacore::test::FakeZwoMount mount(alpacacore::util::default_task_clock(), "AM5");
    configure_storm_mount(mount);
    REQUIRE(mount.ok());

    for (int i = 0; i < 25; ++i) {
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(mount.port()));
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.0, 20.0));
        REQUIRE_NOTHROW(driver->pulse_guide(0, 300));
        if ((i % 2) != 0) {
            // Half the time the async teardown thread is also live at
            // destruction — the destructor must join GOTO, teardown, poll and
            // pulse threads, in that order, every time.
            driver->disconnect();
        }
        driver.reset();
    }
}

#endif  // !_WIN32

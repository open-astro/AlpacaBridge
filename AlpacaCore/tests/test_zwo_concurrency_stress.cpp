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
// (issue #101). No fake seam exists for the EFW SDK, so on a hardware-free
// host every connect fails fast at enumeration — which still storms the
// AsyncConnectable machinery, the failure-path cleanup, and the property
// getters racing the lifecycle: the exact surfaces where this driver family's
// review findings lived. With a wheel attached the same tests exercise the
// full connect path.

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

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

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

// Focuser stress (#271): same shape as the EFW/camera cases above — no fake
// seam exists for the EAF SDK, so on a hardware-free host every connect
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

// Rotator stress (#271): same shape — no fake seam for the CAA SDK.
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

// Telescope stress (audit follow-up, 3.0.1): the ZWO mount driver's worst
// review findings were detached-thread lifecycle bugs — the GOTO setup
// thread, the async disconnect teardown thread, and the poll/pulse threads
// racing set_connected(false) and destruction. Those threads only exist on a
// CONNECTED driver, so these tests connect through a FakeMountServer (the
// wrapper treats a successful TCP connect as mount-connected; every
// post-connect query tolerates failure). The operate callback drives the
// exact newly-fixed paths: async GOTO and pulse guiding racing disconnects.
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

// LX200-flavored canned replies: report tracking ON so PulseGuide gets past
// its tracking gate and actually queues work onto the pulse thread; default
// "0#" is a validly-terminated reply for everything else (":MS#" -> "0" is a
// GOTO accept, so the GOTO thread completes its protocol round-trip too).
alpacacore::test::FakeMountServer::Responder zwo_responder() {
    return [](const std::string& chunk) -> std::string {
        if (chunk.find(":GAT") != std::string::npos) {
            return "1#";
        }
        return "0#";
    };
}

}  // namespace

TEST_CASE("ZWO mount - concurrent connect/disconnect/slew/pulse stress", "[zwo][telescope][stress]") {
    alpacacore::test::FakeMountServer server(zwo_responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(server.port()));

    // open-astro#326: one guard per call -- before this the callback stopped at
    // the first throw, so only the first getter was ever storm-tested.
    // Unlike the other five storms in this file, this one runs CONNECTED over
    // FakeMountServer, so the set is widened per AGENTS.md. The set REPLACES
    // the default {NotConnected}. DriverException is here for the FAKE rather
    // than the driver: the canned replies cannot answer a position query, so
    // the driver correctly reports "Invalid RA response from ZWO mount".
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
        // The newly-fixed thread paths: async GOTO (goto_thread_) and pulse
        // guiding (pulse thread queue) issued while other threads disconnect.
        guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
        guard([&] { scope.pulse_guide(0, 50); });
        guard([&] { scope.abort_slew(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ZWO mount - destruction races an in-flight connect", "[zwo][telescope][stress]") {
    alpacacore::test::FakeMountServer server(zwo_responder());
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(port)); });
}

TEST_CASE("ZWO mount - destruction mid-operation (GOTO/pulse threads live)", "[zwo][telescope][stress]") {
    alpacacore::test::FakeMountServer server(zwo_responder());
    REQUIRE(server.ok());

    for (int i = 0; i < 25; ++i) {
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, zwo_endpoint(server.port()));
        static_cast<void>(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        try {
            driver->slew_to_coordinates_async(5.0, 20.0);
        } catch (const std::exception&) {
        }
        try {
            driver->pulse_guide(0, 300);
        } catch (const std::exception&) {
        }
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

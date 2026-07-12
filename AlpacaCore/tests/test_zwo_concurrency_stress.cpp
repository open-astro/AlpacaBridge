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
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

using alpacacore::AlpacaDriver;

TEST_CASE("ZWO EFW - concurrent connect/disconnect/operate stress", "[zwo][filterwheel][stress]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        static_cast<void>(wheel.get_position());
        wheel.set_position(1);
        static_cast<void>(wheel.get_names());
        static_cast<void>(wheel.get_focus_offsets());
    });

    // Still alive and coherent after the storm (Connected reflects whether a
    // physical wheel is attached; both outcomes are valid here).
    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
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

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        static_cast<void>(camera.get_camera_state());
        static_cast<void>(camera.get_ccd_temperature());
        camera.set_gain(50);
        static_cast<void>(camera.get_image_ready());
        camera.stop_exposure();
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("ZWO camera - destruction races an in-flight connect", "[zwo][camera][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::zwo::create_zwo_camera_by_index(0, 0); });
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

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
        static_cast<void>(scope.get_tracking());
        static_cast<void>(scope.get_right_ascension());
        static_cast<void>(scope.get_declination());
        static_cast<void>(scope.get_slewing());
        // The newly-fixed thread paths: async GOTO (goto_thread_) and pulse
        // guiding (pulse thread queue) issued while other threads disconnect.
        scope.slew_to_coordinates_async(5.0, 20.0);
        scope.pulse_guide(0, 50);
        scope.abort_slew();
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
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

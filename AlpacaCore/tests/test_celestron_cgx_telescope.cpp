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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#endif

#include "catch2_compat.h"
#include "cgx_clock_cleanup.h"
#include "driver_rig.h"
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {

using alpacacore::test::require_alpaca_error;

alpacacore::vendor::celestron::CelestronCgxConnectionInfo test_info() {
    alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
    info.port_path = "/dev/null";
    info.site_latitude_degrees = 45.0;
    info.site_longitude_degrees = -75.0;
    return info;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// One state poll on the calling thread: the coordinate getters serve its frame, never the wire (#1940).
void poll_now(alpacacore::TelescopeDriver& driver) {
    alpacacore::vendor::celestron::poll_state_now_for_testing(driver);
}

// Slewing reads false. Until the first stop ACK the link fault raises (#1950), which is not done yet.
bool stop_done(const alpacacore::TelescopeDriver& driver) {
    try {
        return !driver.get_slewing();
    } catch (const alpacacore::AlpacaException& error) {
        if (error.error_code() != alpacacore::AlpacaError::DriverException) throw;
        return false;
    }
}

}  // namespace

TEST_CASE("Celestron CGX USB Telescope Driver - disconnect waits for an in-flight motion handoff",
          "[celestron][cgx][telescope]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto config = test_info();
    config.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, config, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(0, 100);
    REQUIRE(clock.wait_for_wait_count(1, 2s));
    auto home = std::async(std::launch::async, [&] { driver->find_home(); });
    const bool draining = clock.wait_for_wait_count(2, 2s);
    std::promise<void> started;
    auto disconnecting = std::async(std::launch::async, [&] {
        started.set_value();
        driver->set_connected(false);
    });
    started.get_future().wait();
    const auto early = disconnecting.wait_for(100ms);
    // Release the cancelled packet before any assertion can unwind/join its owner.
    mount.complete_axis_guide(0x11);
    mount.set_auto_complete_motions(true);
    clock.advance(20ms);
    alpacacore::test::drain_cgx_clock_task(disconnecting, clock);
    REQUIRE(draining);
    CHECK(early == std::future_status::timeout);
    CHECK_NOTHROW(home.get());
    CHECK_NOTHROW(disconnecting.get());
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Celestron CGX USB Telescope Driver - explicit stop displaces a draining guide handoff",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    std::function<void(alpacacore::TelescopeDriver&)> initiate;
    bool guide_times_out = false;
    SECTION("FindHome") {
        initiate = [](auto& driver) { driver.find_home(); };
    }
    SECTION("FindHome after guide timeout") {
        initiate = [](auto& driver) { driver.find_home(); };
        guide_times_out = true;
    }
    SECTION("Park") {
        initiate = [](auto& driver) { driver.park(); };
    }
    SECTION("SlewToCoordinatesAsync") {
        initiate = [](auto& driver) {
            driver.slew_to_coordinates_async(std::fmod(driver.get_sidereal_time() + 21.0, 24.0), 65.0);
        };
    }
    SECTION("SlewToTargetAsync") {
        initiate = [](auto& driver) { driver.slew_to_target_async(); };
    }
    SECTION("Tracking on") {
        initiate = [](auto& driver) { driver.set_tracking(true); };
    }
    SECTION("Tracking off") {
        initiate = [](auto& driver) { driver.set_tracking(false); };
    }
    SECTION("TrackingRate") {
        initiate = [](auto& driver) { driver.set_tracking_rate(1); };
    }
    SECTION("SyncToCoordinates") {
        initiate = [](auto& driver) { driver.sync_to_coordinates(6.0, 87.0); };
    }
    SECTION("SyncToTarget") {
        initiate = [](auto& driver) { driver.sync_to_target(); };
    }

    for (const bool unpark : {false, true}) {
        INFO("unpark=" << unpark);
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        alpacacore::test::FakeTaskClock clock;
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
        alpacacore::test::CgxClockCleanup cleanup{driver, clock};
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        driver->set_tracking(true);
        const double target_ra = std::fmod(driver->get_sidereal_time() + 21.0, 24.0);
        driver->set_target_right_ascension(target_ra);
        driver->set_target_declination(65.0);
        mount.set_auto_complete_motions(false);
        mount.set_hold_guide_until_completed(true);
        driver->pulse_guide(0, 600000);
        const bool guiding = clock.wait_for_wait_count(1, 2s);
        const auto before = mount.requests();
        const auto count = [](const auto& requests, int axis, int command) {
            return std::count_if(requests.begin(), requests.end(), [&](const auto& request) {
                return request.destination == axis && request.command == command;
            });
        };
        auto initiating = std::async(std::launch::async, [&] { initiate(*driver); });
        const bool draining = clock.wait_for_wait_count(2, 2s);
        const auto held = initiating.wait_for(0ms);
        // Give the queued stop a later deadline than the guide drain, so rejoin can be tested in isolation.
        clock.advance(1ms);
        mount.set_stop_completion_reads(2);
        auto stopping = std::async(std::launch::async, [&] {
            if (unpark)
                driver->unpark();
            else
                driver->abort_slew();
            // Both entry points must coalesce with the already accepted stop.
            driver->abort_slew();
            driver->unpark();
        });
        const auto accepted = stopping.wait_for(100ms);
        const bool queued = clock.wait_for_wait_count(3, 2s);
        const bool pending = driver->get_slewing();
        mount.set_auto_complete_motions(true);
        if (guide_times_out) clock.advance(3050ms);
        auto displaced_early = guide_times_out ? initiating.wait_for(100ms) : std::future_status::timeout;
        mount.complete_axis_guide(0x11);
        if (!guide_times_out) {
            clock.advance(19ms);
            displaced_early = initiating.wait_for(100ms);
        }
        const bool pending_after_handoff = displaced_early == std::future_status::ready && driver->get_slewing();
        auto zero_pulse = std::async(std::launch::async, [&] { driver->pulse_guide(2, 0); });
        const auto zero_returned = zero_pulse.wait_for(100ms);

        // Drive possible Sync/stop waits before any assertion can unwind futures or the stop worker.
        const bool returned = wait_until([&] {
            clock.advance(20ms);
            return initiating.wait_for(0ms) == std::future_status::ready &&
                   stopping.wait_for(0ms) == std::future_status::ready &&
                   zero_pulse.wait_for(0ms) == std::future_status::ready;
        });
        std::exception_ptr completion_failure;
        const bool idle = wait_until([&] {
            clock.advance(20ms);
            try {
                return !driver->get_slewing();
            } catch (...) {
                completion_failure = std::current_exception();
                return true;
            }
        });
        mount.set_stop_completion_reads(0);
        std::exception_ptr displaced;
        try {
            initiating.get();
        } catch (...) {
            displaced = std::current_exception();
        }
        CHECK_NOTHROW(stopping.get());
        require_alpaca_error([&] { zero_pulse.get(); }, alpacacore::AlpacaError::InvalidOperation);
        REQUIRE(returned);
        REQUIRE(idle);
        if (completion_failure) CHECK_NOTHROW(std::rethrow_exception(completion_failure));
        REQUIRE(guiding);
        REQUIRE(draining);
        REQUIRE(queued);
        CHECK((held == std::future_status::timeout));
        CHECK((accepted == std::future_status::ready));
        CHECK((displaced_early == std::future_status::ready));
        CHECK((zero_returned == std::future_status::ready));
        CHECK(pending);
        CHECK(pending_after_handoff);
        REQUIRE(displaced);
        require_alpaca_error([&] { std::rethrow_exception(displaced); }, alpacacore::AlpacaError::InvalidOperation);
        for (const auto axis : {0x10, 0x11}) {
            INFO("axis=" << axis);
            CHECK((mount.count_command(axis, 0x24) == count(before, axis, 0x24) + 1));
            CHECK((mount.count_command(axis, 0x13) >= count(before, axis, 0x13) + 3));
            CHECK((mount.count_command(axis, 0x12) >= count(before, axis, 0x12) + 3));
            CHECK((mount.count_command(axis, 0x27) >= count(before, axis, 0x27) + 3));
            for (const auto command : {0x0b, 0x02, 0x17, 0x04, 0x26, 0x07}) {
                INFO("command=" << command);
                CHECK((mount.count_command(axis, command) == count(before, axis, command)));
            }
            // Only the stop worker's final sidereal restoration may write tracking.
            CHECK((mount.count_command(axis, 0x06) == count(before, axis, 0x06) + 1));
        }
        CHECK_FALSE(driver->get_at_park());
        CHECK_FALSE(driver->get_at_home());
        CHECK_FALSE(driver->get_is_pulse_guiding());
        CHECK(driver->get_tracking());
        CHECK((driver->get_tracking_rate() == 0));
        CHECK((driver->get_target_right_ascension() == target_ra));
        CHECK((driver->get_target_declination() == 65.0));
        CHECK(std::isfinite(driver->get_right_ascension()));
        CHECK(std::isfinite(driver->get_declination()));
        alpacacore::test::set_cgx_connected(*driver, clock, false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - disconnect drains a stop queued behind a guide handoff",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(0, 600000);
    const bool guiding = clock.wait_for_wait_count(1, 2s);
    const int ra_homes = mount.count_command(0x10, 0x0b);
    const int dec_homes = mount.count_command(0x11, 0x0b);
    const int guide_packets = mount.count_command(0x11, 0x26);
    auto home = std::async(std::launch::async, [&] { driver->find_home(); });
    const bool draining = clock.wait_for_wait_count(2, 2s);
    auto stopping = std::async(std::launch::async, [&] { driver->abort_slew(); });
    const auto accepted = stopping.wait_for(100ms);
    const bool queued = clock.wait_for_wait_count(3, 2s);
    std::promise<void> entered;
    auto disconnecting = std::async(std::launch::async, [&] {
        entered.set_value();
        driver->set_connected(false);
    });
    entered.get_future().wait();
    const auto early = disconnecting.wait_for(100ms);
    mount.set_auto_complete_motions(true);
    mount.complete_axis_guide(0x11);
    alpacacore::test::drain_cgx_clock_task(disconnecting, clock);
    const bool returned =
        home.wait_for(0ms) == std::future_status::ready && stopping.wait_for(0ms) == std::future_status::ready;
    std::exception_ptr displaced;
    try {
        home.get();
    } catch (...) {
        displaced = std::current_exception();
    }
    CHECK_NOTHROW(stopping.get());
    CHECK_NOTHROW(disconnecting.get());
    REQUIRE(returned);
    REQUIRE(guiding);
    REQUIRE(draining);
    REQUIRE(queued);
    CHECK((accepted == std::future_status::ready));
    CHECK((early == std::future_status::timeout));
    REQUIRE(displaced);
    require_alpaca_error([&] { std::rethrow_exception(displaced); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(mount.axis_guiding(0x11));
    CHECK((mount.count_command(0x10, 0x0b) == ra_homes));
    CHECK((mount.count_command(0x11, 0x0b) == dec_homes));
    CHECK((mount.count_command(0x11, 0x26) == guide_packets));
}

TEST_CASE("Celestron CGX USB Telescope Driver - parked target methods refuse pre-set coordinates without dispatch",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->set_target_right_ascension(7.5);
    driver->set_target_declination(30.0);
    driver->park();
    REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
    const auto before = mount.requests();
    SECTION("SlewToTargetAsync") {
        require_alpaca_error([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidWhileParked);
    }
    SECTION("SyncToTarget") {
        require_alpaca_error([&] { driver->sync_to_target(); }, alpacacore::AlpacaError::InvalidWhileParked);
    }
    SECTION("SlewToCoordinatesAsync") {
        require_alpaca_error([&] { driver->slew_to_coordinates_async(6.0, 25.0); },
                             alpacacore::AlpacaError::InvalidWhileParked);
    }
    SECTION("SyncToCoordinates") {
        require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 25.0); },
                             alpacacore::AlpacaError::InvalidWhileParked);
    }
    CHECK((mount.requests().size() == before.size()));
    CHECK(driver->get_at_park());
    CHECK_FALSE(driver->get_slewing());
    // Canonical parked accessor guards remain; inspect the saved pair only after Unpark.
    driver->unpark();
    REQUIRE(wait_until([&] { return !driver->get_at_park() && !driver->get_slewing(); }));
    CHECK((driver->get_target_right_ascension() == 7.5));
    CHECK((driver->get_target_declination() == 30.0));
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - disconnect checkpoint leaves cached getters responsive",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    for (const bool fail_checkpoint : {false, true}) {
        INFO("fail_checkpoint=" << fail_checkpoint);
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x01});
        auto disconnect = std::async(std::launch::async, [&] { driver->set_connected(false); });
        const bool collecting = mount.wait_for_reply_fragment(2s);
        auto guide_rate = std::async(std::launch::async, [&] { return driver->get_guide_rate(); });
        const auto readable = guide_rate.wait_for(100ms);
        std::promise<void> entered;
        auto tracking = std::async(std::launch::async, [&] {
            entered.set_value();
            driver->set_tracking(true);
        });
        entered.get_future().wait();
        const auto queued = tracking.wait_for(100ms);
        if (fail_checkpoint) mount.corrupt_next_reply_checksum();
        // Always release the transport before assertions can unwind and join callers.
        mount.release_reply_tail();
        CHECK(collecting);
        CHECK((readable == std::future_status::ready));
        CHECK((guide_rate.get().ra > 0.0));
        CHECK((queued == std::future_status::timeout));
        if (fail_checkpoint)
            require_alpaca_error([&] { disconnect.get(); }, alpacacore::AlpacaError::DriverException);
        else
            CHECK_NOTHROW(disconnect.get());
        require_alpaca_error([&] { tracking.get(); }, alpacacore::AlpacaError::NotConnected);
        CHECK_FALSE(driver->get_connected());
        driver->set_connected(true);
        if (fail_checkpoint)
            require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
        else
            CHECK_NOTHROW(driver->set_tracking(true));
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - modern identity is read-only and preserves the build number",
          "[celestron][cgx][telescope]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_cgx_switches_present(false);
    mount.set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x88});
    mount.set_motor_identity(0x11, {7, 17, 1, 25}, {0x17, 0x88});
    auto config = test_info();
    config.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, config);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_device_firmware() == "RA 7.17.25, Dec 7.17.281");
    const auto requests = mount.requests();
    // The four identity queries, then the state poll's first frame (one position read per axis, #1940).
    REQUIRE(requests.size() == 6);
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        CHECK((request.destination == 0x10 || request.destination == 0x11));
        if (i < 4)
            CHECK((request.command == 0xfe || request.command == 0x05));
        else
            CHECK(request.command == 0x01);
        CHECK(request.payload.empty());
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - modern identity rejects malformed motor models",
          "[celestron][cgx][telescope]") {
    for (const auto axis : {0x10, 0x11}) {
        for (const auto& model : std::vector<std::vector<std::uint8_t>>{{}, {0x17}, {0x17, 0x88, 0}}) {
            alpacacore::test::FakeCelestronCgxMount mount;
            REQUIRE(mount.ok());
            mount.set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x88});
            mount.set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
            mount.set_motor_identity(axis, {7, 17, 0, 25}, model);
            auto config = test_info();
            config.port_path = mount.slave_path();
            auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, config);
            require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
            CHECK_FALSE(driver->get_connected());
            CHECK_FALSE(driver->get_device_firmware());
        }
    }
}

// #2204: encoder scaling is the CGX's, so an unknown motor is added but never driven with CGX constants.
// Falsified by: celestron_cgx_telescope_driver.cpp connect refusing a two-byte motor model other than 0x1788.
TEST_CASE("Celestron CGX USB Telescope Driver - an unknown motor connects generically and refuses motion (#2204)",
          "[celestron][cgx][telescope]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x89});
    mount.set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
    auto config = test_info();
    config.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, config);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_name() == "Celestron Telescope");
    for (const std::function<void()>& motion : std::vector<std::function<void()>>{
             [&] { driver->find_home(); },
             [&] { driver->park(); },
             [&] { driver->pulse_guide(0, 100); },
             [&] { driver->slew_to_coordinates_async(1.0, 1.0); },
             [&] { driver->set_tracking(true); },
         }) {
        require_alpaca_error(motion, alpacacore::AlpacaError::InvalidOperation);
    }
}

// #2204: a stop needs no axis scaling, so an unknown motor must still honour AbortSlew and Unpark.
// Falsified by: celestron_cgx_telescope_driver.cpp start_stop_operation() calling require_known_motor_model().
TEST_CASE("Celestron CGX USB Telescope Driver - an unknown motor still accepts AbortSlew and Unpark (#2204)",
          "[celestron][cgx][telescope]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x89});
    mount.set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
    auto config = test_info();
    config.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, config);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_NOTHROW(driver->abort_slew());
    CHECK_NOTHROW(driver->unpark());
}

TEST_CASE("Celestron CGX USB Telescope Driver - version lengths and mixed formats are refused",
          "[celestron][cgx][telescope]") {
    for (const auto axis : {0x10, 0x11}) {
        for (const auto& version :
             std::vector<std::vector<std::uint8_t>>{{}, {7}, {7, 17}, {7, 17, 0}, {7, 17, 0, 25, 0}}) {
            alpacacore::test::FakeCelestronCgxMount mount;
            REQUIRE(mount.ok());
            mount.set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x88});
            mount.set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
            mount.set_motor_identity(axis, version, {0x17, 0x88});
            auto config = test_info();
            config.port_path = mount.slave_path();
            auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, config);
            require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
            CHECK_FALSE(driver->get_connected());
        }
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - Defaults", "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, test_info());

    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == alpacacore::DeviceType::Telescope);
    CHECK(driver->get_device_number() == 0);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "Celestron CGX/CGX-L");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Celestron_CGX_USB_0");
    CHECK_FALSE(driver->get_can_slew());
    CHECK(driver->get_can_slew_async());
    CHECK(driver->get_can_pulse_guide());
    CHECK(driver->get_can_set_guide_rates());
    CHECK_FALSE(driver->get_can_move_axis(0));
    CHECK_FALSE(driver->get_can_move_axis(1));
    CHECK_FALSE(driver->get_can_move_axis(2));
}

TEST_CASE("Celestron CGX USB Telescope Driver - stop acknowledgement waits for completion",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    mount.set_stop_completion_reads(2);
    CHECK_NOTHROW(driver->abort_slew());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    // Each axis must exhaust both busy replies before publishing completion.
    CHECK((mount.count_command(0x10, 0x13) >= 3));
    CHECK((mount.count_command(0x11, 0x13) >= 3));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    mount.set_stop_completion_reads(0);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - stop initiators return before verification and retain park state",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    for (const bool unpark : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        if (unpark) {
            driver->park();
            REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
        } else {
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        }
        mount.set_stop_completion_reads(10);
        mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x24});
        auto initiator = std::async(std::launch::async, [&] {
            if (unpark)
                driver->unpark();
            else
                driver->abort_slew();
        });
        const bool verifying = mount.wait_for_reply_fragment(2s);
        const auto returned = initiator.wait_for(100ms);
        auto progress = std::async(std::launch::async, [&] { return driver->get_slewing(); });
        const auto progress_early = progress.wait_for(100ms);
        auto tracking = std::async(std::launch::async, [&] { return driver->get_tracking(); });
        const auto tracking_early = tracking.wait_for(100ms);
        // Tracking reads return during the ACK pair (decision 0016) with the
        // state from before the stop, never a normal-dispatch "unknown" failure.
        // Inspect the pending completion state during the scripted post-ACK deceleration.
        mount.release_reply_tail();
        const bool pending_during_ack = progress.get();
        const bool slewing = driver->get_slewing();
        const bool parked = driver->get_at_park();
        const int ra_reads = mount.count_command(0x10, 0x01);
        const int dec_reads = mount.count_command(0x11, 0x01);
        const auto read_started = std::chrono::steady_clock::now();
        bool coordinates_refused = true;
        for (const auto& read : std::vector<std::function<double()>>{
                 [&] { return driver->get_right_ascension(); }, [&] { return driver->get_declination(); },
                 [&] { return driver->get_altitude(); }, [&] { return driver->get_azimuth(); },
                 [&] { return static_cast<double>(driver->get_side_of_pier()); }}) {
            try {
                static_cast<void>(read());
                coordinates_refused = false;
            } catch (const alpacacore::AlpacaException& error) {
                coordinates_refused =
                    coordinates_refused && error.error_code() == alpacacore::AlpacaError::DriverException;
            }
        }
        const auto read_elapsed = std::chrono::steady_clock::now() - read_started;
        const bool no_encoder_reads =
            mount.count_command(0x10, 0x01) == ra_reads && mount.count_command(0x11, 0x01) == dec_reads;
        CHECK_NOTHROW(initiator.get());
        CHECK(tracking_early == std::future_status::ready);
        CHECK_FALSE(tracking.get());
        REQUIRE(verifying);
        CHECK(returned == std::future_status::ready);
        CHECK(progress_early == std::future_status::ready);
        CHECK(pending_during_ack);
        CHECK(slewing);
        CHECK(parked == unpark);
        CHECK(coordinates_refused);
        CHECK(read_elapsed < 100ms);
        CHECK(no_encoder_reads);
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        CHECK_FALSE(driver->get_at_park());
        driver->set_connected(false);
        require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); }, alpacacore::AlpacaError::NotConnected);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - AbortSlew drains only the active guide packet in its worker",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(0, 600000);
    const int packets = mount.count_command(0x11, 0x26);
    const auto started = std::chrono::steady_clock::now();
    driver->abort_slew();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const bool stopping = driver->get_slewing();
    mount.complete_axis_guide(0x11);
    CHECK(elapsed < 100ms);
    CHECK(stopping);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(mount.count_command(0x11, 0x26) == packets);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_tracking());
    const auto requests = mount.requests();
    const auto rate = std::find_if(requests.rbegin(), requests.rend(), [](const auto& frame) {
        return frame.destination == 0x10 && frame.command == 0x06;
    });
    REQUIRE(rate != requests.rend());
    CHECK(rate->payload == std::vector<std::uint8_t>{0xff, 0xff});
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - stop completion has a bounded deadline",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    mount.set_stop_completion_reads(10000);
    const auto started = std::chrono::steady_clock::now();
    CHECK_NOTHROW(driver->abort_slew());
    CHECK(std::chrono::steady_clock::now() - started < 100ms);
    REQUIRE(wait_until(
        [&] {
            try {
                static_cast<void>(driver->get_slewing());
                return false;
            } catch (const alpacacore::AlpacaException& error) {
                return error.error_code() == alpacacore::AlpacaError::DriverException;
            }
        },
        5s));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= 2900ms);
    CHECK(elapsed < 4s);
    CHECK(mount.count_command(0x10, 0x24) == 1);
    CHECK(mount.count_command(0x11, 0x24) == 1);
    require_alpaca_error([&] { static_cast<void>(driver->get_at_home()); }, alpacacore::AlpacaError::DriverException);
    mount.set_stop_completion_reads(0);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - async stop verifies deceleration beyond the cleanup budget",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    for (const bool unpark : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        if (unpark) {
            driver->park();
            REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
        }
        // Sixty valid busy replies, polled 20 ms apart, model ~1.2 s of
        // deceleration. A 500 ms verifier fails; a single 3 s verifier passes.
        mount.set_stop_completion_reads(60);
        const auto started = std::chrono::steady_clock::now();
        if (unpark)
            driver->unpark();
        else
            driver->abort_slew();
        CHECK(std::chrono::steady_clock::now() - started < 100ms);
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 4s));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed >= 1s);
        CHECK(elapsed < 3s);
        CHECK(mount.count_command(0x10, 0x24) == 1);
        CHECK(mount.count_command(0x11, 0x24) == 1);
        CHECK_FALSE(driver->get_at_park());
        CHECK_FALSE(driver->get_tracking());
        mount.set_stop_completion_reads(0);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - disconnect serializes with the stop worker reap",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    mount.fragment_next_reply();
    driver->abort_slew();
    const bool verifying = mount.wait_for_reply_fragment(2s);
    auto disconnect = std::async(std::launch::async, [&] { driver->set_connected(false); });
    const auto early = disconnect.wait_for(100ms);
    const bool connected = driver->get_connected();
    mount.release_reply_tail();
    CHECK_NOTHROW(disconnect.get());
    REQUIRE(verifying);
    CHECK(early == std::future_status::timeout);
    CHECK(connected);
    CHECK_FALSE(driver->get_connected());
}

#ifndef _WIN32
// Issue #1457: a stop retry must not wait for another device's probe on the
// node. Since #1456 a port claim waits out such a probe (up to 20 s per
// attempt); since #1759 the retry runs with mutex_ free, so no getter that
// takes mutex_ waits behind it; since #1940 the retry resyncs the fd it
// already holds and never reopens the port (decision 0009 rule 4), and a
// pulled cable is a lost link at once (rule 2a), so a later stop fails
// because the link is lost, with NotConnected. The probe claim below is the
// wait the stop must never reach.
TEST_CASE("Celestron CGX USB Telescope Driver - a stop retry does not wait for another device's probe",
          "[celestron][cgx][telescope][integration][issue1457]") {
    namespace util = alpacacore::util;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    const std::string path = info.port_path;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);

    // The adapter goes away: the first stop meets the hangup, which is a lost
    // link at once (decision 0009 rule 2a): the wrapper closes the link and
    // releases the port claim, the stop fails and Slewing reads NotConnected.
    mount.sever_link();
    CHECK_NOTHROW(driver->abort_slew());
    REQUIRE(wait_until([&] { return !driver->get_connected(); }, std::chrono::seconds(10)));
    require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE_FALSE(util::is_serial_port_in_use(path));

    // Another device's probe now holds the node for 5 s while a second stop
    // is asked for. The link is lost, so it fails at once with NotConnected
    // and never reopens the port or waits for the probe.
    auto probe = util::SerialPortClaim::acquire(path, util::SerialClaimKind::Probe, "Other probe");
    REQUIRE(probe);
    auto release = std::async(std::launch::async, [&probe] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        probe.release();
    });
    require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::NotConnected);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto started = std::chrono::steady_clock::now();
    try {
        static_cast<void>(driver->get_tracking());
    } catch (const alpacacore::AlpacaException&) {
        // The link is gone, so the getter refuses; only its latency matters.
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(2));
    release.get();
}

TEST_CASE("Celestron CGX USB Telescope Driver - a tracking rollback does not wait for another device's probe",
          "[celestron][cgx][telescope][integration][issue1457]") {
    namespace util = alpacacore::util;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    const std::string path = info.port_path;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);

    // The adapter goes away: the first tracking write meets the hangup, which
    // closes the link and releases the claim; its rollback resyncs the closed
    // fd and fails too.
    mount.sever_link();
    try {
        driver->set_tracking(false);
    } catch (const alpacacore::AlpacaException&) {
        // Expected: the write and its rollback both fail.
    }
    REQUIRE_FALSE(util::is_serial_port_in_use(path));

    // Another device's probe holds the node for 5 s while a second tracking
    // write fails and its rollback resyncs the closed fd: nothing reopens the
    // port, so nothing waits for the probe.
    auto probe = util::SerialPortClaim::acquire(path, util::SerialClaimKind::Probe, "Other probe");
    REQUIRE(probe);
    auto release = std::async(std::launch::async, [&probe] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        probe.release();
    });
    auto write = std::async(std::launch::async, [&driver] {
        try {
            driver->set_tracking(false);
        } catch (const alpacacore::AlpacaException&) {
            // Expected: the link is gone.
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto started = std::chrono::steady_clock::now();
    try {
        static_cast<void>(driver->get_tracking());
    } catch (const alpacacore::AlpacaException&) {
        // The link is gone, so the getter refuses; only its latency matters.
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(2));
    write.get();
    release.get();
}
#endif

TEST_CASE("Celestron CGX USB Telescope Driver - stop communication failures persist through completion properties",
          "[celestron][cgx][telescope][integration]") {
    for (const bool unpark : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        if (unpark) {
            driver->park();
            REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
        } else {
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
            driver->set_tracking(true);
        }
        mount.drop_next_reply(0x11, 0x13, false);
        if (unpark)
            CHECK_NOTHROW(driver->unpark());
        else
            CHECK_NOTHROW(driver->abort_slew());
        REQUIRE(wait_until([&] {
            try {
                static_cast<void>(driver->get_slewing());
                return false;
            } catch (const alpacacore::AlpacaException& error) {
                return error.error_code() == alpacacore::AlpacaError::DriverException;
            }
        }));
        for (int read = 0; read < 2; ++read) {
            require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); },
                                 alpacacore::AlpacaError::DriverException);
            require_alpaca_error([&] { static_cast<void>(driver->get_at_park()); },
                                 alpacacore::AlpacaError::DriverException);
            require_alpaca_error([&] { static_cast<void>(driver->get_tracking()); },
                                 alpacacore::AlpacaError::DriverException);
        }
        // A later verified stop, rather than a property read, clears failure.
        // The fault hides whether the first stop has ended (#1950), and a
        // request while it runs joins it, so repeat it until a new stop is sent.
        const int dec_stops = mount.count_command(0x11, 0x24);
        REQUIRE(wait_until([&] {
            if (unpark)
                driver->unpark();
            else
                driver->abort_slew();
            return mount.count_command(0x11, 0x24) > dec_stops;
        }));
        REQUIRE(wait_until([&] { return stop_done(*driver); }));
        CHECK_FALSE(driver->get_at_park());
        CHECK_FALSE(driver->get_tracking());
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - Slewing raises the link fault while a stop is pending",
          "[celestron][cgx][telescope][integration][issue1950]") {
    for (const bool unpark : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        if (unpark) {
            driver->park();
            REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
        } else {
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        }
        const auto slewing_raises_fault = [&] {
            require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); },
                                 alpacacore::AlpacaError::DriverException);
        };
        // A lost encoder reply faults the link until the next matched reply. The state poll's read meets it: the
        // getters no longer read the axes (#1940).
        mount.drop_next_reply(0x10, 0x01, false);
        poll_now(*driver);
        require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                             alpacacore::AlpacaError::DriverException);
        slewing_raises_fault();
        // The out-of-sync link refuses the first RA stop unsent, and the mount
        // stays silent for the next three stops (Dec, RA, Dec).
        mount.drop_next_reply(0x11, 0x24, false);
        mount.drop_next_reply(0x10, 0x24, false);
        mount.drop_next_reply(0x11, 0x24, false);
        const auto stops = [&] { return mount.count_command(0x10, 0x24) + mount.count_command(0x11, 0x24); };
        const int stops_before = stops();
        if (unpark)
            driver->unpark();
        else
            driver->abort_slew();
        slewing_raises_fault();
        slewing_raises_fault();
        // Only the fourth stop request can get a reply, so both reads above ran
        // with the stop pending and the link still faulted.
        CHECK(stops() < stops_before + 4);
        // The first stop ACK clears the fault, and the stop then completes.
        REQUIRE(wait_until([&] { return stop_done(*driver); }, std::chrono::seconds(10)));
        CHECK_FALSE(driver->get_slewing());
        CHECK(stops() == stops_before + 5);
        CHECK_FALSE(driver->get_at_park());
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - explicit stop owns cancelled motion dispatch and verification",
          "[celestron][cgx][telescope][integration]") {
    for (const bool unpark : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home(); }));
        driver->set_tracking(true);
        mount.set_auto_complete_motions(false);
        driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 21.0, 24.0), 65.0);
        const int ra_stops = mount.count_command(0x10, 0x24);
        const int dec_stops = mount.count_command(0x11, 0x24);
        mount.set_stop_completion_reads(2);
        auto stopping = std::async(std::launch::async, [&] {
            if (unpark)
                driver->unpark();
            else
                driver->abort_slew();
        });
        CHECK_NOTHROW(stopping.get());
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        CHECK(mount.count_command(0x10, 0x24) == ra_stops + 1);
        CHECK(mount.count_command(0x11, 0x24) == dec_stops + 1);
        CHECK_FALSE(driver->get_is_pulse_guiding());
        CHECK(driver->get_tracking());
        // A verified stop followed by a refused tracking restore must surface
        // the restore failure without claiming the old enabled tracking state.
        mount.drop_next_reply(0x10, 0x06, false);
        driver->abort_slew();
        // Wait for the stop's own failure, not the link fault it meets first (#1950).
        REQUIRE(wait_until([&] {
            try {
                static_cast<void>(driver->get_slewing());
                return false;
            } catch (const alpacacore::AlpacaException& error) {
                return error.error_code() == alpacacore::AlpacaError::DriverException &&
                       std::string(error.what()).starts_with("Celestron CGX stop failed: ");
            }
        }));
        CHECK_FALSE(driver->get_tracking());
        driver->abort_slew();
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - explicit stop retains flip cancellation phase cleanup",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.set_auto_complete_motions(false);
    const double target_ra = std::fmod(driver->get_sidereal_time() + 1.0, 24.0);
    driver->slew_to_coordinates_async(target_ra, 65.0);
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    const int ra_labels = mount.count_command(0x10, 0x04);
    const int dec_labels = mount.count_command(0x11, 0x04);
    const int ra_stops = mount.count_command(0x10, 0x24);
    const int dec_stops = mount.count_command(0x11, 0x24);
    driver->abort_slew();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(mount.count_command(0x10, 0x24) == ra_stops + 1);
    CHECK(mount.count_command(0x11, 0x24) == dec_stops + 1);
    CHECK(mount.count_command(0x10, 0x04) == ra_labels);
    CHECK(mount.count_command(0x11, 0x04) == dec_labels);
    CHECK(mount.count_command(0x10, 0x02) == 0);
    CHECK(mount.count_command(0x11, 0x02) == 0);
    CHECK_FALSE(driver->get_at_home());
    CHECK_FALSE(driver->get_at_park());
    CHECK(std::isfinite(driver->get_right_ascension()));
    CHECK(std::isfinite(driver->get_declination()));
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - Sync waits for encoder stability after tracking stops",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home(); }));
    mount.set_ra_stop_drift_reads(8);
    driver->set_tracking(true);
    driver->set_tracking(false);
    CHECK_NOTHROW(driver->sync_to_coordinates(6.0, 87.0));
    CHECK_FALSE(driver->get_tracking());
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 0.001);
    CHECK(std::abs(driver->get_declination() - 87.0) < 0.0001);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - Sync refuses unsettled encoders before writing labels",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home(); }));
    const int ra_writes = mount.count_command(0x10, 0x04);
    const int dec_writes = mount.count_command(0x11, 0x04);
    mount.set_ra_stop_drift_reads(10000);
    driver->set_tracking(true);
    driver->set_tracking(false);
    require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 87.0); }, alpacacore::AlpacaError::DriverException);
    CHECK(mount.count_command(0x10, 0x04) == ra_writes);
    CHECK(mount.count_command(0x11, 0x04) == dec_writes);
    CHECK(std::isfinite(driver->get_right_ascension()));
    // The same perpetual drift also prevents a retained disconnect checkpoint.
    require_alpaca_error([&] { driver->set_connected(false); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Celestron CGX USB Telescope Driver - Sync verifies settled label readback without widening tolerance",
          "[celestron][cgx][telescope][integration]") {
    for (const bool settles : {true, false}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home(); }));
        const int ra_writes = mount.count_command(0x10, 0x04);
        const int dec_writes = mount.count_command(0x11, 0x04);
        mount.set_label_read_offsets(0x10, {4, 3, 1, 0, 0});
        mount.set_label_read_offsets(
            0x11, settles ? std::vector<std::int32_t>{-8, -7, -1, 1, 1, 0, 0} : std::vector<std::int32_t>(40, 8));
        if (settles) {
            CHECK_NOTHROW(driver->sync_to_coordinates(6.0, 87.0));
            CHECK(std::abs(driver->get_right_ascension() - 6.0) < 0.001);
            CHECK(std::abs(driver->get_declination() - 87.0) < 0.0001);
        } else {
            require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 87.0); },
                                 alpacacore::AlpacaError::DriverException);
            require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                                 alpacacore::AlpacaError::DriverException);
        }
        CHECK(mount.count_command(0x10, 0x04) == ra_writes + 1);
        CHECK(mount.count_command(0x11, 0x04) == dec_writes + 1);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - Device metadata", "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(3, test_info());
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_name() == "Celestron CGX/CGX-L");
    CHECK(driver->get_description() == "Celestron CGX/CGX-L direct USB mount");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron CGX AUX Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Celestron_CGX_USB_3");
}

TEST_CASE("Celestron CGX USB Telescope Driver - factory rejects invalid configured ranges",
          "[celestron][cgx][telescope][unit]") {
    const auto rejects =
        [](const std::function<void(alpacacore::vendor::celestron::CelestronCgxConnectionInfo&)>& set) {
            auto info = test_info();
            set(info);
            require_alpaca_error([&] { static_cast<void>(alpacacore::test::create_cgx_telescope_poll_held(0, info)); },
                                 alpacacore::AlpacaError::InvalidValue);
        };
    rejects([](auto& info) { info.site_latitude_degrees = 90.1; });
    rejects([](auto& info) { info.site_longitude_degrees = std::numeric_limits<double>::quiet_NaN(); });
    rejects([](auto& info) { info.site_elevation_meters = -301.0; });
    rejects([](auto& info) { info.aperture_diameter_meters = -0.1; });
    rejects([](auto& info) { info.focal_length_meters = std::numeric_limits<double>::infinity(); });
}

TEST_CASE("Celestron CGX USB Telescope Driver - Not connected errors", "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, test_info());
    require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_declination()); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_tracking()); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->park(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Celestron CGX USB Telescope Driver - Unsupported actions", "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, test_info());
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));
    require_alpaca_error([&] { static_cast<void>(driver->action("unknown", "")); },
                         alpacacore::AlpacaError::ActionNotImplemented);
}

TEST_CASE("Celestron CGX USB Telescope Driver - independent targets and exact errors",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(1, test_info());

    require_alpaca_error([&] { static_cast<void>(driver->get_target_right_ascension()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_target_declination()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    try {
        driver->sync_to_coordinates(24.0, 0.0);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("SyncToCoordinates: RightAscension must be in range") != std::string::npos);
    }
    require_alpaca_error([&] { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&] { driver->set_target_right_ascension(7.5); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Celestron CGX USB Telescope Driver - error codes and input precedence",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, test_info());

    require_alpaca_error([&] { driver->pulse_guide(-1, 100); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(0, 0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move_axis(3, 0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_guide_rate({0.0, 0.0}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_tracking_rate(3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_side_of_pier(2); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_tracking_rate(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_site_latitude()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_site_longitude()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_site_elevation()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_site_latitude(45.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_site_longitude(-75.0); }, alpacacore::AlpacaError::NotConnected);
    for (const double invalid :
         {-90.1, 90.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        require_alpaca_error([&] { driver->set_site_latitude(invalid); }, alpacacore::AlpacaError::InvalidValue);
    for (const double invalid :
         {-180.1, 180.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        require_alpaca_error([&] { driver->set_site_longitude(invalid); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { static_cast<void>(driver->get_aperture_diameter()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_aperture_area()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_aperture_diameter(0.2); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_focal_length()); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_focal_length(1.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_does_refraction()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_does_refraction(false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_slew_settle_time()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_slew_settle_time(2); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension_rate()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_declination_rate()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_right_ascension_rate(0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_declination_rate(0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_side_of_pier(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->action("unsupported", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { static_cast<void>(driver->command_blind("x")); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { static_cast<void>(driver->command_bool("x")); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { static_cast<void>(driver->command_string("x")); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

// open-astro#1786: the base class's individual guide-rate setters read the pair through the connection-checking
// getter first, so an out-of-range GuideRateRightAscension or GuideRateDeclination write on a disconnected CGX
// answered NotConnected instead of InvalidValue.
TEST_CASE("Celestron CGX USB Telescope Driver - individual guide-rate range errors precede NotConnected",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, test_info());
    constexpr double kSidereal = 15.0411 / 3600.0;
    for (const double bad : {kSidereal * 1.5, kSidereal * 0.05, -kSidereal, std::numeric_limits<double>::quiet_NaN()}) {
        require_alpaca_error([&] { driver->set_guide_rate_right_ascension(bad); },
                             alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->set_guide_rate_declination(bad); }, alpacacore::AlpacaError::InvalidValue);
    }
    // An in-range value still reaches the connection check.
    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(kSidereal * 0.5); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_guide_rate_declination(kSidereal * 0.5); },
                         alpacacore::AlpacaError::NotConnected);
}

// open-astro#1786: the CGX stores one rate per axis, so writing one axis keeps the other, and a refused write keeps
// both.
TEST_CASE("Celestron CGX USB Telescope Driver - individual guide-rate writes keep the other axis",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    constexpr double kSidereal = 15.0411 / 3600.0;

    driver->set_guide_rate({kSidereal * 0.25, kSidereal * 0.75});
    driver->set_guide_rate_right_ascension(kSidereal * 0.5);
    CHECK(driver->get_guide_rate().ra == kSidereal * 0.5);
    CHECK(driver->get_guide_rate().dec == kSidereal * 0.75);
    driver->set_guide_rate_declination(kSidereal * 0.9);
    CHECK(driver->get_guide_rate().ra == kSidereal * 0.5);
    CHECK(driver->get_guide_rate().dec == kSidereal * 0.9);

    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(kSidereal * 2.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_guide_rate_declination(0.0); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_guide_rate().ra == kSidereal * 0.5);
    CHECK(driver->get_guide_rate().dec == kSidereal * 0.9);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - UTCDate, SiderealTime and pointing read one time source",
          "[celestron][cgx][telescope][unit]") {
    struct ProbeGuard {
        ~ProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
    } restore_probe;

    std::atomic<std::int64_t> epoch_seconds{1800000000};
    const auto now = [&] { return std::chrono::system_clock::time_point{std::chrono::seconds(epoch_seconds.load())}; };
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver =
        alpacacore::test::create_cgx_telescope_poll_held(0, info, alpacacore::util::default_task_clock(), now);
    driver->set_connected(true);

    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return false; });
    const auto host_utc = now();
    const auto client_utc = host_utc + std::chrono::hours(2);
    driver->set_utc_date(client_utc);
    CHECK(driver->get_utc_date() == client_utc);
    const auto client_lst =
        alpacacore::vendor::celestron::cgx_local_sidereal_time_hours(client_utc, info.site_longitude_degrees.value());
    CHECK(std::abs(std::remainder(driver->get_sidereal_time() - client_lst, 24.0)) < 1e-9);

    // A later system-clock step invalidates the saved offset rather than
    // applying it on top of the corrected host time: all three return to the host clock.
    epoch_seconds.fetch_add(10);
    CHECK(driver->get_utc_date() == now());
    const auto stepped_lst =
        alpacacore::vendor::celestron::cgx_local_sidereal_time_hours(now(), info.site_longitude_degrees.value());
    CHECK(std::abs(std::remainder(driver->get_sidereal_time() - stepped_lst, 24.0)) < 1e-9);

    // Decision 0001 (#2166): a disciplined host refuses the write and changes nothing.
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    const auto synchronized_host_utc = now();
    require_alpaca_error([&] { driver->set_utc_date(synchronized_host_utc + std::chrono::hours(2)); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(driver->get_utc_date() == synchronized_host_utc);
    const auto host_lst = alpacacore::vendor::celestron::cgx_local_sidereal_time_hours(
        synchronized_host_utc, info.site_longitude_degrees.value());
    CHECK(std::abs(std::remainder(driver->get_sidereal_time() - host_lst, 24.0)) < 1e-9);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - UTCDate answers from a fresh connect (#2166)",
          "[celestron][cgx][telescope][unit][utcdate]") {
    struct ProbeGuard {
        ~ProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
    } restore_probe;
    const bool disciplined = GENERATE(false, true);
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(disciplined ? +[] { return true; }
                                                                                : +[] { return false; });
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
    auto driver =
        alpacacore::test::create_cgx_telescope_poll_held(0, info, alpacacore::util::default_task_clock(), now);
    for (int session = 0; session < 2; ++session) {
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        // No client write yet: the host clock is the time source, for UTCDate as for SiderealTime.
        CHECK(driver->get_utc_date() == now());
        const auto host_lst =
            alpacacore::vendor::celestron::cgx_local_sidereal_time_hours(now(), info.site_longitude_degrees.value());
        CHECK(std::abs(std::remainder(driver->get_sidereal_time() - host_lst, 24.0)) < 1e-9);
        const auto client_utc = now() + std::chrono::hours(2);
        if (disciplined) {
            require_alpaca_error([&] { driver->set_utc_date(client_utc); },
                                 alpacacore::AlpacaError::PropertyNotImplemented);
            CHECK(driver->get_utc_date() == now());
        } else {
            driver->set_utc_date(client_utc);
            CHECK(driver->get_utc_date() == client_utc);
        }
        driver->set_connected(false);
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
    }
}

namespace {

// A connected CGX on a fake task clock with an injected system time. pass() moves both clocks
// (time passes); step() moves the system time alone (a host-clock step, decision 0001 rule 3).
struct CgxUtcRig {
    std::atomic<std::int64_t> epoch_ms{1800000000000};
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeCelestronCgxMount mount;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};

    CgxUtcRig() {
        REQUIRE(mount.ok());
        auto info = test_info();
        info.port_path = mount.slave_path();
        driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, [this] { return now(); });
        driver->set_connected(true);
    }
    std::chrono::system_clock::time_point now() const {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds(epoch_ms.load())};
    }
    void pass(std::chrono::milliseconds elapsed) {
        epoch_ms.fetch_add(elapsed.count());
        clock.advance(elapsed);
    }
    void step(std::chrono::milliseconds jump) { epoch_ms.fetch_add(jump.count()); }
    bool points_at(std::chrono::system_clock::time_point utc) const {
        const auto lst = alpacacore::vendor::celestron::cgx_local_sidereal_time_hours(
            utc, test_info().site_longitude_degrees.value());
        return std::abs(std::remainder(driver->get_sidereal_time() - lst, 24.0)) < 1e-9;
    }
};

struct CgxProbeGuard {
    ~CgxProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
};

}  // namespace

TEST_CASE("Celestron CGX USB Telescope Driver - UTCDate step detection reads the task clock",
          "[celestron][cgx][telescope][unit][taskclock]") {
    using namespace std::chrono_literals;
    CgxProbeGuard restore_probe;
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return false; });
    CgxUtcRig rig;
    const auto client_utc = rig.now() + 2h;
    rig.driver->set_utc_date(client_utc);

    rig.pass(60s);
    CHECK(rig.driver->get_utc_date() == client_utc + 60s);

    rig.step(10s);
    CHECK(rig.driver->get_utc_date() == rig.now());
    CHECK(rig.points_at(rig.now()));
}

TEST_CASE("Celestron CGX USB Telescope Driver - host discipline is re-checked every 30 s on the task clock",
          "[celestron][cgx][telescope][unit][taskclock]") {
    using namespace std::chrono_literals;
    CgxProbeGuard restore_probe;
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return false; });
    CgxUtcRig rig;
    const auto client_utc = rig.now() + 2h;
    rig.driver->set_utc_date(client_utc);
    CHECK(rig.points_at(client_utc));

    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    rig.pass(29999ms);
    CHECK(rig.points_at(client_utc + 29999ms));
    rig.pass(1ms);
    CHECK(rig.points_at(rig.now()));
    // One time source: the UTCDate readback moves to the host clock with pointing.
    CHECK(rig.driver->get_utc_date() == rig.now());
}

TEST_CASE("Celestron CGX USB Telescope Driver - a UTCDate write restarts the 30 s re-check window",
          "[celestron][cgx][telescope][unit][taskclock]") {
    using namespace std::chrono_literals;
    CgxProbeGuard restore_probe;
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return false; });
    CgxUtcRig rig;
    rig.driver->set_utc_date(rig.now() + 2h);
    rig.pass(20s);
    const auto client_utc = rig.now() + 2h;
    rig.driver->set_utc_date(client_utc);

    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    rig.pass(29999ms);
    CHECK(rig.points_at(client_utc + 29999ms));
    rig.pass(1ms);
    CHECK(rig.points_at(rig.now()));
}

TEST_CASE("Celestron CGX USB Telescope Driver - unsupported methods use ASCOM errors",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, test_info());
    CHECK_FALSE(driver->get_can_slew());
    CHECK_FALSE(driver->get_can_slew_alt_az_async());
    CHECK_FALSE(driver->get_can_sync_alt_az());
    CHECK_FALSE(driver->get_can_set_park());
    require_alpaca_error([&] { driver->slew_to_coordinates(2.0, 15.0); }, alpacacore::AlpacaError::NotConnected);
    // Disconnected, the connection check comes first (#1919).
    require_alpaca_error([&] { driver->slew_to_alt_az_async(90.0, 45.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->sync_to_alt_az(90.0, 45.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_park(); }, alpacacore::AlpacaError::NotConnected);
}

#ifndef _WIN32
TEST_CASE("Celestron CGX USB Telescope Driver - home timeout names the axis still pending",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    std::string expected = "home timed out: RA";
    mount.set_auto_complete_motions(false);
    SECTION("FindHome") { driver->find_home(); }
    SECTION("meridian home route") {
        expected = "meridian home route timed out: RA";
        driver->set_tracking(true);
        driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 6.0, 24.0), 30.0);
    }
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    mount.complete_axis_home(0x11);
    std::string error;
    for (int second = 0; second < 200 && error.empty(); ++second) {
        try {
            if (!driver->get_slewing()) break;
        } catch (const alpacacore::AlpacaException& failure) {
            error = failure.what();
            break;
        }
        if (!clock.wait_for_waiters(1, 1s)) {
            // The body may have thrown between the Slewing read and the rendezvous; otherwise it missed a wait.
            INFO("no home waiter after " << second << " s of fake time");
            REQUIRE(wait_until([&] {
                try {
                    static_cast<void>(driver->get_slewing());
                    return false;
                } catch (const alpacacore::AlpacaException&) {
                    return true;
                }
            }));
            break;
        }
        clock.advance(1s);
        REQUIRE(clock.wait_for_woken_settled(2s));
    }
    REQUIRE(wait_until([&] {
        try {
            static_cast<void>(driver->get_slewing());
            return false;
        } catch (const alpacacore::AlpacaException& failure) {
            error = failure.what();
            return true;
        }
    }));
    mount.set_auto_complete_motions(true);
    CHECK(error.find(expected + " home_done=0 slew_done=0 position=") != std::string::npos);
    CHECK(error.find("; Dec home_done=1 slew_done=1 position=") != std::string::npos);
}

TEST_CASE("Celestron CGX USB Telescope Driver - home wait logs both axes at DEBUG only",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));

    std::mutex lines_mutex;
    std::vector<std::string> lines;
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
        ~SinkGuard() {
            alpacacore::logging::set_log_sink(previous);
            alpacacore::logging::set_log_level(level);
        }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel, std::string_view component, std::string_view message) {
            if (component != "Celestron CGX" || message.find("Home wait: ") == std::string_view::npos) return;
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.emplace_back(message);
        });
    const auto home_wait_lines = [&] {
        std::lock_guard<std::mutex> lock(lines_mutex);
        return lines;
    };
    const bool debug = GENERATE(true, false);
    INFO("debug=" << debug);
    alpacacore::logging::set_log_level(debug ? alpacacore::logging::LogLevel::Debug
                                             : alpacacore::logging::LogLevel::Info);

    mount.set_auto_complete_motions(false);
    SECTION("FindHome") { driver->find_home(); }
    SECTION("meridian home route") {
        driver->set_tracking(true);
        driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 6.0, 24.0), 30.0);
    }
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    // The body has made its first poll and waits 200 ms; from here each 1 s advance is one loop iteration.
    REQUIRE(clock.wait_for_waiters(1, 2s));
    std::vector<int> before;
    for (const std::uint8_t axis : {std::uint8_t{0x10}, std::uint8_t{0x11}})
        for (const std::uint8_t command : {std::uint8_t{0x12}, std::uint8_t{0x13}, std::uint8_t{0x01}})
            before.push_back(mount.count_command(axis, command));
    const auto step = [&](std::chrono::seconds by) {
        REQUIRE(clock.wait_for_waiters(1, 2s));
        clock.advance(by);
        REQUIRE(clock.wait_for_woken_settled(2s));
    };
    for (int second = 1; second <= 12; ++second) {
        INFO("advanced " << second << " s");
        step(1s);
        REQUIRE(driver->get_slewing());
    }
    // A long stall logs once, then waits a full 10 s again instead of logging on every tick to catch up.
    step(25s);
    step(1s);
    const int iterations = 14;

    const auto logged = home_wait_lines();
    if (debug) {
        REQUIRE(logged.size() == 2);
        for (const auto& line : logged) {
            INFO(line);
            CHECK(line.find("Home wait: RA home_done=0 slew_done=0 position=") != std::string::npos);
            CHECK(line.find("; Dec home_done=0 slew_done=0 position=") != std::string::npos);
        }
    } else {
        CHECK(logged.empty());
        // Below DEBUG the loop reads only RA's MC_LEVEL_DONE (Dec's is short-circuited while RA is pending).
        CHECK(mount.count_command(0x10, 0x12) == before[0] + iterations);
        CHECK(mount.count_command(0x10, 0x13) == before[1]);
        CHECK(mount.count_command(0x10, 0x01) == before[2]);
        CHECK(mount.count_command(0x11, 0x12) == before[3]);
        CHECK(mount.count_command(0x11, 0x13) == before[4]);
        CHECK(mount.count_command(0x11, 0x01) == before[5]);
    }
    mount.set_auto_complete_motions(true);
}

// Issue #1868: the pier-side and rate capabilities and SlewToAltAz, read on a connected driver. The direct USB
// backend has no Alt/Az slew, so a connected SlewToAltAz is MethodNotImplemented and sends no AUX packet.
TEST_CASE("Celestron CGX USB Telescope Driver - connected pier-side and rate capabilities and SlewToAltAz",
          "[celestron][cgx][telescope][unit]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    // Disconnected, the connection check comes first (#1919).
    require_alpaca_error([&] { driver->slew_to_alt_az(45.0, 180.0); }, alpacacore::AlpacaError::NotConnected);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK_FALSE(driver->get_can_set_pier_side());
    CHECK_FALSE(driver->get_can_set_right_ascension_rate());
    CHECK_FALSE(driver->get_can_set_declination_rate());
    CHECK_FALSE(driver->get_can_slew_alt_az());
    const auto before = mount.requests();
    require_alpaca_error([&] { driver->slew_to_alt_az(45.0, 180.0); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK((mount.requests().size() == before.size()));
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - connects and homes both motor controllers",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    info.site_elevation_meters = 0.0;
    info.aperture_diameter_meters = 0.2;
    info.focal_length_meters = 1.0;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // Connected, the unsupported members answer MethodNotImplemented (disconnected: NotConnected, above).
    require_alpaca_error([&] { driver->slew_to_alt_az_async(90.0, 45.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->sync_to_alt_az(90.0, 45.0); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->set_park(); }, alpacacore::AlpacaError::MethodNotImplemented);
    CHECK(driver->get_site_elevation() == 0.0);
    CHECK(driver->get_aperture_diameter() == 0.2);
    CHECK(std::abs(driver->get_aperture_area() - std::numbers::pi * 0.01) < 1e-12);
    CHECK(driver->get_focal_length() == 1.0);
    require_alpaca_error([&] { static_cast<void>(driver->get_target_right_ascension()); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_target_declination()); },
                         alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_right_ascension(7.5);
    CHECK(driver->get_target_right_ascension() == 7.5);
    require_alpaca_error([&] { static_cast<void>(driver->get_target_declination()); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->sync_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_declination(-20.0);
    CHECK(driver->get_target_right_ascension() == 7.5);
    CHECK(driver->get_target_declination() == -20.0);
    CHECK_FALSE(driver->get_at_home());
    driver->find_home();
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!driver->get_at_home() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(driver->get_at_home());
    CHECK_FALSE(driver->get_slewing());
    CHECK(driver->get_declination() > 89.99);

    const auto retry_home = [&] {
        driver->find_home();
        mount.complete_axis_home(0x10);
        mount.complete_axis_home(0x11);
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    };
    mount.drop_next_reply(0x10, 0x0b, true);
    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_at_home());
    CHECK_FALSE(driver->get_slewing());
    CHECK(mount.count_command(0x10, 0x24) > 0);
    retry_home();

    mount.drop_next_reply(0x11, 0x0b, false);
    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_at_home());
    CHECK(mount.count_command(0x11, 0x24) > 0);
    retry_home();

    driver->set_tracking(true);
    CHECK(driver->get_tracking());
    CHECK_FALSE(driver->get_at_home());
    driver->set_tracking_rate(1);
    CHECK(driver->get_tracking_rate() == 1);
    driver->pulse_guide(0, 1000);
    CHECK(driver->get_is_pulse_guiding());
    // A second pulse on the axis supersedes this one (#2263): the motion-owner cases pin it.
    auto frames = mount.requests();
    auto pulse = std::find_if(frames.rbegin(), frames.rend(), [](const auto& frame) { return frame.command == 0x26; });
    REQUIRE(pulse != frames.rend());
    CHECK(pulse->destination == 0x11);
    CHECK(pulse->payload == std::vector<std::uint8_t>{0xce, 100});
    mount.complete_axis_guide(0x11);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));

    driver->pulse_guide(0, 3000);
    CHECK(driver->get_is_pulse_guiding());
    mount.complete_axis_guide(0x11);
    REQUIRE(wait_until([&] { return mount.count_command(0x11, 0x26) == 3; }));
    frames = mount.requests();
    pulse = std::find_if(frames.rbegin(), frames.rend(), [](const auto& frame) { return frame.command == 0x26; });
    REQUIRE(pulse != frames.rend());
    CHECK(pulse->payload[1] == 45);
    mount.complete_axis_guide(0x11);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));

    mount.set_ra_tracking_drift_per_read(100);
    driver->sync_to_coordinates(6.0, 87.0);
    CHECK(driver->get_tracking());
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 0.001);
    CHECK(std::abs(driver->get_declination() - 87.0) < 2.2e-5);
    CHECK(driver->get_target_right_ascension() == 6.0);
    CHECK(driver->get_target_declination() == 87.0);

    const auto tracking_ra_commands = mount.count_command(0x10, 0x06);
    const auto tracking_dec_commands = mount.count_command(0x11, 0x06);
    mount.drop_next_reply(0x10, 0x06, false);
    require_alpaca_error([&] { driver->set_tracking(false); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_tracking());
    require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(mount.count_command(0x10, 0x06) == tracking_ra_commands + 2);
    // The dropped RA acknowledgement poisons AUX matching, so rollback sends
    // the Dec stop after reconnect; no Dec write reaches the wire beforehand.
    CHECK(mount.count_command(0x11, 0x06) == tracking_dec_commands + 1);
    const auto recovered_rates = mount.requests();
    for (const auto axis : {0x10, 0x11}) {
        const auto rate = std::find_if(recovered_rates.rbegin(), recovered_rates.rend(), [axis](const auto& frame) {
            return frame.destination == axis && frame.command == 0x06;
        });
        REQUIRE(rate != recovered_rates.rend());
        CHECK(rate->payload == std::vector<std::uint8_t>{0, 0, 0});
    }

    mount.drop_next_reply(0x10, 0x06, false);
    mount.drop_next_reply(0x10, 0x06, false);
    require_alpaca_error([&] { driver->set_tracking(false); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { static_cast<void>(driver->get_tracking()); }, alpacacore::AlpacaError::DriverException);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Celestron CGX USB Telescope Driver - rejects a Celestron AUX bus without CGX home switches",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_cgx_switches_present(false);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(mount.count_command(0x10, 0x0b) == 0);
    CHECK(mount.count_command(0x11, 0x0b) == 0);
}

TEST_CASE("Celestron CGX USB Telescope Driver - connects without a site and refuses what needs one",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    info.site_latitude_degrees.reset();
    info.site_longitude_degrees.reset();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    const auto refused = [&](const std::function<void()>& call) {
        try {
            call();
            FAIL("expected InvalidOperation");
        } catch (const alpacacore::AlpacaException& error) {
            CHECK(error.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(error.what()).find("Site") != std::string::npos);
        }
    };
    const auto refuse_all = [&] {
        refused([&] { static_cast<void>(driver->get_right_ascension()); });
        refused([&] { static_cast<void>(driver->get_declination()); });
        refused([&] { static_cast<void>(driver->get_altitude()); });
        refused([&] { static_cast<void>(driver->get_azimuth()); });
        refused([&] { static_cast<void>(driver->get_side_of_pier()); });
        refused([&] { static_cast<void>(driver->get_destination_side_of_pier(6.0, 30.0)); });
        refused([&] { static_cast<void>(driver->get_sidereal_time()); });
        refused([&] { driver->find_home(); });
        refused([&] { driver->park(); });
        refused([&] { driver->sync_to_coordinates(6.0, 30.0); });
        refused([&] { driver->slew_to_coordinates_async(6.0, 30.0); });
        refused([&] { driver->set_tracking(true); });
        refused([&] { driver->pulse_guide(0, 100); });
    };
    refused([&] { static_cast<void>(driver->get_site_latitude()); });
    refused([&] { static_cast<void>(driver->get_site_longitude()); });
    refuse_all();
    CHECK_FALSE(driver->get_tracking());
    CHECK_FALSE(driver->get_at_park());
    CHECK(mount.count_command(0x10, 0x24) == 0);  // no motion command was sent
    CHECK(mount.count_command(0x10, 0x26) == 0);

    driver->set_site_latitude(-33.856784);
    refuse_all();  // latitude alone is not enough
    driver->set_site_longitude(151.215297);
    CHECK(driver->get_site_longitude() == 151.215297);
    CHECK_NOTHROW(static_cast<void>(driver->get_sidereal_time()));
    CHECK_NOTHROW(static_cast<void>(driver->get_destination_side_of_pier(6.0, 30.0)));

    // Longitude alone is not enough either: sidereal time and pier side still refuse.
    alpacacore::test::FakeCelestronCgxMount longitude_mount;
    REQUIRE(longitude_mount.ok());
    auto longitude_info = test_info();
    longitude_info.port_path = longitude_mount.slave_path();
    longitude_info.site_latitude_degrees.reset();
    longitude_info.site_longitude_degrees = 151.215297;
    driver = alpacacore::test::create_cgx_telescope_poll_held(0, longitude_info);
    driver->set_connected(true);
    refuse_all();
}

TEST_CASE("Celestron CGX USB Telescope Driver - pins guide polarity in both hemispheres and pointing branches",
          "[celestron][cgx][telescope][integration]") {
    for (const double latitude : {45.0, -45.0}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = latitude;
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        driver->set_tracking(true);

        for (const bool beyond_pole : {false, true}) {
            mount.set_encoder_position(0x11, beyond_pole ? 0x400000 : 0xc00000);
            poll_now(*driver);  // the getters serve the polled frame (#1940)
            CHECK(driver->get_side_of_pier() == (beyond_pole ? 1 : 0));
            for (const auto& [direction, north_normal_velocity] :
                 {std::pair{0, -50}, std::pair{1, 50}, std::pair{2, -50}, std::pair{3, 50}}) {
                const bool dec_axis = direction < 2;
                const int expected_velocity =
                    north_normal_velocity * (latitude < 0.0 ? -1 : 1) * (dec_axis && beyond_pole ? -1 : 1);
                driver->pulse_guide(direction, 100);
                const auto requests = mount.requests();
                const auto pulse = std::find_if(requests.rbegin(), requests.rend(),
                                                [](const auto& frame) { return frame.command == 0x26; });
                REQUIRE(pulse != requests.rend());
                CHECK(pulse->destination == (dec_axis ? 0x11 : 0x10));
                CHECK(pulse->payload ==
                      std::vector<std::uint8_t>{static_cast<std::uint8_t>(static_cast<std::int8_t>(expected_velocity)),
                                                10});
                REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));
                if (dec_axis) {
                    // The bench establishes native positive velocity -> increasing
                    // encoder counts. Apply that physical effect, independently of
                    // the requested ASCOM direction; the fake does not move guides.
                    const double before = driver->get_declination();
                    const auto velocity = static_cast<std::int8_t>(pulse->payload[0]);
                    const auto position = static_cast<std::int64_t>(mount.axis_position(0x11));
                    const int step = velocity > 0 ? 100 : -100;
                    mount.set_encoder_position(0x11, static_cast<std::uint32_t>(position + step) & 0x00ffffffU);
                    poll_now(*driver);
                    const double after = driver->get_declination();
                    if (direction == 0)
                        CHECK(after > before);
                    else
                        CHECK(after < before);
                }
            }
        }
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - rounds and segments guide durations at packet boundaries",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    const std::vector<std::pair<int, std::vector<std::uint8_t>>> cases{
        {1, {1}},      {5, {1}},      {15, {2}},        {2549, {255}},
        {2550, {255}}, {2551, {255}}, {2555, {255, 1}}, {5105, {255, 255, 1}}};
    for (const auto& [milliseconds, expected_chunks] : cases) {
        INFO(milliseconds);
        const int before = mount.count_command(0x11, 0x26);
        driver->pulse_guide(0, milliseconds);
        REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));
        const auto requests = mount.requests();
        std::vector<std::uint8_t> chunks;
        const int sent = mount.count_command(0x11, 0x26) - before;
        CHECK(sent == static_cast<int>(expected_chunks.size()));
        for (auto frame = requests.rbegin(); frame != requests.rend() && chunks.size() < static_cast<std::size_t>(sent);
             ++frame) {
            if (frame->command == 0x26) {
                CHECK(frame->destination == 0x11);
                REQUIRE(frame->payload.size() == 2);
                chunks.push_back(frame->payload[1]);
            }
        }
        std::reverse(chunks.begin(), chunks.end());
        CHECK(chunks == expected_chunks);
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - zero duration guide validates state without starting motion",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    const auto invalid_inputs = [&] {
        require_alpaca_error([&] { driver->pulse_guide(2, -1); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->pulse_guide(-1, 0); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->pulse_guide(4, 0); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->pulse_guide(2, 600001); }, alpacacore::AlpacaError::InvalidValue);
    };
    invalid_inputs();
    require_alpaca_error([&] { driver->pulse_guide(2, 0); }, alpacacore::AlpacaError::NotConnected);
    driver->set_connected(true);
    require_alpaca_error([&] { driver->pulse_guide(2, 0); }, alpacacore::AlpacaError::InvalidOperation);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    require_alpaca_error([&] { driver->pulse_guide(2, 0); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_tracking(true);
    const auto ra_position = mount.axis_physical_position(0x10);
    const auto dec_position = mount.axis_physical_position(0x11);
    const auto requests_before = mount.requests().size();
    for (int direction = 0; direction < 4; ++direction) CHECK_NOTHROW(driver->pulse_guide(direction, 0));
    CHECK(mount.requests().size() == requests_before);
    CHECK(mount.count_command(0x10, 0x26) == 0);
    CHECK(mount.count_command(0x11, 0x26) == 0);
    CHECK(mount.axis_physical_position(0x10) == ra_position);
    CHECK(mount.axis_physical_position(0x11) == dec_position);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    CHECK(driver->get_tracking());

    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(3, 2550);
    // A zero-duration pulse claims nothing (#2263): the RA pulse running keeps its axis and its packet.
    CHECK_NOTHROW(driver->pulse_guide(2, 0));
    invalid_inputs();
    CHECK(mount.axis_guiding(0x10));
    CHECK(driver->get_is_pulse_guiding());
    CHECK(mount.count_command(0x10, 0x26) == 1);
    CHECK(mount.count_command(0x11, 0x26) == 0);
    mount.complete_axis_guide(0x10);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));

    driver->find_home();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    require_alpaca_error([&] { driver->pulse_guide(2, 0); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(mount.axis_homing(0x10));
    CHECK(mount.axis_homing(0x11));
    CHECK(mount.count_command(0x10, 0x26) == 1);
    driver->abort_slew();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    mount.set_auto_complete_motions(true);
    driver->park();
    REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
    require_alpaca_error([&] { driver->pulse_guide(2, 0); }, alpacacore::AlpacaError::InvalidWhileParked);
    invalid_inputs();
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - retains isolated Sync references across logical reconnect and flip",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount first_mount;
    alpacacore::test::FakeCelestronCgxMount second_mount;
    REQUIRE(first_mount.ok());
    REQUIRE(second_mount.ok());
    const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
    first_mount.set_auto_complete_motions(true);
    second_mount.set_auto_complete_motions(true);
    auto first_info = test_info();
    auto second_info = test_info();
    first_info.port_path = first_mount.slave_path();
    second_info.port_path = second_mount.slave_path();
    second_info.site_latitude_degrees = -45.0;
    auto first =
        alpacacore::test::create_cgx_telescope_poll_held(0, first_info, alpacacore::util::default_task_clock(), now);
    auto second =
        alpacacore::test::create_cgx_telescope_poll_held(1, second_info, alpacacore::util::default_task_clock(), now);
    first->set_connected(true);
    second->set_connected(true);
    first->find_home();
    second->find_home();
    REQUIRE(wait_until([&] { return first->get_at_home() && !first->get_slewing(); }));
    REQUIRE(wait_until([&] { return second->get_at_home() && !second->get_slewing(); }));
    first->sync_to_coordinates(6.0, 87.0);
    second->sync_to_coordinates(12.0, -87.0);
    CHECK(std::abs(first->get_declination() - 87.0) < 2.2e-5);
    CHECK(std::abs(second->get_declination() + 87.0) < 2.2e-5);
    const auto ra_rehome_label = first_mount.axis_position(0x10);
    const auto dec_rehome_label = first_mount.axis_position(0x11);
    // Sync happened at physical home, so its labels are the shifted home labels.
    CHECK(first_mount.axis_physical_position(0x10) == 0U);
    CHECK(first_mount.axis_physical_position(0x11) == 0U);
    first->set_tracking(true);
    second->set_tracking(true);
    const auto check_no_reference_writes = [](const auto& mount) {
        for (const auto axis : {0x10, 0x11}) {
            CHECK(mount.count_command(axis, 0x0b) == 1);
            CHECK(mount.count_command(axis, 0x04) == 2);  // FindHome + Sync only.
            CHECK(mount.count_command(axis, 0x02) == 0);
        }
    };
    first->set_connected(false);
    CHECK_FALSE(first->get_connected());
    CHECK(second->get_connected());
    CHECK(second->get_tracking());
    CHECK(std::abs(second->get_declination() + 87.0) < 2.2e-5);

    first->set_connected(true);
    CHECK_FALSE(first->get_at_home());
    CHECK_FALSE(first->get_tracking());
    CHECK(std::abs(std::remainder(first->get_right_ascension() - 6.0, 24.0)) < 0.001);
    CHECK(std::abs(first->get_declination() - 87.0) < 2.2e-5);
    CHECK_NOTHROW(first->set_tracking(true));
    second->set_connected(false);
    CHECK(first->get_tracking());
    CHECK(std::abs(first->get_declination() - 87.0) < 2.2e-5);
    second->set_connected(true);
    CHECK_FALSE(second->get_tracking());
    CHECK(std::abs(std::remainder(second->get_right_ascension() - 12.0, 24.0)) < 0.001);
    CHECK(std::abs(second->get_declination() + 87.0) < 2.2e-5);
    CHECK_NOTHROW(second->set_tracking(true));
    check_no_reference_writes(first_mount);
    check_no_reference_writes(second_mount);

    // Hold the flip at physical home so the retained Sync labels can be
    // inspected before GOTO moves the axes away from that origin.
    first_mount.set_auto_complete_motions(false);
    const double flip_ra =
        std::fmod(first->get_sidereal_time() + (first->get_side_of_pier() == 0 ? 1.0 : -1.0) + 24.0, 24.0);
    first->slew_to_coordinates_async(flip_ra, 25.0);
    REQUIRE(wait_until([&] { return first_mount.axis_homing(0x10) && first_mount.axis_homing(0x11); }));
    first_mount.complete_axis_home(0x10);
    first_mount.complete_axis_home(0x11);
    REQUIRE(wait_until(
        [&] { return first_mount.count_command(0x10, 0x02) == 1 && first_mount.count_command(0x11, 0x02) == 1; }));
    CHECK(first_mount.axis_position(0x10) == ra_rehome_label);
    CHECK(first_mount.axis_position(0x11) == dec_rehome_label);
    for (const auto axis : {0x10, 0x11}) {
        CHECK(first_mount.count_command(axis, 0x0b) == 2);
        CHECK(first_mount.count_command(axis, 0x04) == 3);
    }
    first_mount.complete_axis_slew(0x10);
    first_mount.complete_axis_slew(0x11);
    REQUIRE(wait_until([&] { return !first->get_slewing(); }));
    CHECK(std::abs(std::remainder(first->get_right_ascension() - flip_ra, 24.0)) < 0.001);
    CHECK(std::abs(first->get_declination() - 25.0) < 2.2e-5);
    CHECK(first->get_tracking());
    CHECK(second->get_tracking());
    CHECK(std::abs(second->get_declination() + 87.0) < 2.2e-5);
    check_no_reference_writes(second_mount);
    first->set_connected(false);
    second->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - continuity budget expires on the injected clock",
          "[celestron][cgx][telescope][continuity-clock]") {
    using namespace std::chrono_literals;
    bool reconnect = false;
    SECTION("disconnect checkpoint") {}
    SECTION("retained-reference reconnect") { reconnect = true; }

    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    if (reconnect) alpacacore::test::set_cgx_connected(*driver, clock, false);

    const int ra_reads = mount.count_command(0x10, 0x01);
    const int dec_reads = mount.count_command(0x11, 0x01);
    mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x10, 0x01});
    auto transition = std::async(std::launch::async, [&] { driver->set_connected(reconnect); });
    const bool held = mount.wait_for_reply_fragment(2s);
    // Advance the shared driver budget, not the OS timeout of the in-flight exchange.
    clock.advance(501ms);
    mount.release_reply_tail();
    alpacacore::test::drain_cgx_clock_task(transition, clock);
    CHECK(held);
    try {
        transition.get();
        FAIL_CHECK("Expected reference continuity budget timeout");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(error.what()).find("reference continuity check timed out") != std::string::npos);
    }
    CHECK(mount.count_command(0x10, 0x01) == ra_reads + 1);
    CHECK(mount.count_command(0x11, 0x01) == dec_reads);
    CHECK_FALSE(driver->get_connected());
    // A fresh Connect may recover transport, never the failed reference.
    alpacacore::test::set_cgx_connected(*driver, clock, true);
    CHECK_FALSE(driver->get_at_home());
    CHECK_FALSE(driver->get_tracking());
    require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
    for (const auto axis : {0x10, 0x11}) {
        CHECK(mount.count_command(axis, 0x0b) == 1);
        CHECK(mount.count_command(axis, 0x04) == 1);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - continuity settling and cleanup use virtual sleeps",
          "[celestron][cgx][telescope][continuity-clock]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    REQUIRE(clock.waiter_count() == 0);

    for (const bool connected : {false, true}) {
        const auto waits = clock.wait_count();
        const int ra_reads = mount.count_command(0x10, 0x01);
        const int dec_reads = mount.count_command(0x11, 0x01);
        auto transition = std::async(std::launch::async, [&] { driver->set_connected(connected); });
        bool registered = true;
        for (int interval = 1; interval <= 3; ++interval) {
            const bool waiting = clock.wait_for_wait_count(waits + interval, 2s);
            registered = registered && waiting;
            if (waiting) {
                CHECK(mount.count_command(0x10, 0x01) == ra_reads + interval);
                CHECK(mount.count_command(0x11, 0x01) == dec_reads + interval);
                CHECK(transition.wait_for(0ms) == std::future_status::timeout);
                clock.advance(20ms);
            }
        }
        alpacacore::test::drain_cgx_clock_task(transition, clock);
        CHECK_NOTHROW(transition.get());
        CHECK(registered);
        CHECK(clock.wait_count() == waits + 3);
        // Four checkpoint reads per axis; a connect adds the state poll's first frame, one read per axis (#1940).
        const int first_frame = connected ? 1 : 0;
        CHECK(mount.count_command(0x10, 0x01) == ra_reads + 4 + first_frame);
        CHECK(mount.count_command(0x11, 0x01) == dec_reads + 4 + first_frame);
        CHECK(driver->get_connected() == connected);
    }
    CHECK(driver->get_at_home());
    CHECK_NOTHROW(driver->set_tracking(true));
    driver->set_tracking(false);
    for (const auto axis : {0x10, 0x11}) {
        CHECK(mount.count_command(axis, 0x0b) == 1);
        CHECK(mount.count_command(axis, 0x04) == 1);
    }

    const auto waits = clock.wait_count();
    SECTION("implicit driver destruction") {
        // No operation waiters remain: destruction's only waits are checkpoint sleepers.
        auto destroying = std::async(std::launch::async, [&] { driver.reset(); });
        alpacacore::test::drain_cgx_clock_task(destroying, clock);
        CHECK_NOTHROW(destroying.get());
    }
    SECTION("fixture cleanup during exception unwinding") {
        try {
            alpacacore::test::CgxClockCleanup unwinding{driver, clock};
            throw std::runtime_error("exercise assertion-unwind lifetime order");
        } catch (const std::runtime_error&) {
        }
        CHECK_FALSE(driver->get_connected());
        alpacacore::test::set_cgx_connected(*driver, clock, true);
        CHECK(driver->get_declination() > 89.99);
        CHECK(clock.wait_count() == waits + 6);
    }
    CHECK(clock.wait_count() >= waits + 3);
    CHECK(clock.waiter_count() == 0);
}

TEST_CASE("Celestron CGX USB Telescope Driver - disconnected continuity requires each axis within two counts",
          "[celestron][cgx][telescope][integration]") {
    for (const auto axis : {0x10, 0x11}) {
        for (const int delta : {-3, -2, 2, 3}) {
            INFO("axis=" << axis << " delta=" << delta);
            alpacacore::test::FakeCelestronCgxMount mount;
            REQUIRE(mount.ok());
            mount.set_auto_complete_motions(true);
            auto info = test_info();
            info.port_path = mount.slave_path();
            auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
            driver->set_connected(true);
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
            driver->sync_to_coordinates(6.0, 87.0);
            driver->set_connected(false);
            mount.set_encoder_position(axis, (mount.axis_position(axis) + delta) & 0x00ffffffU);
            driver->set_connected(true);
            REQUIRE(driver->get_connected());
            for (const auto motor : {0x10, 0x11}) {
                CHECK(mount.count_command(motor, 0x0b) == 1);
                CHECK(mount.count_command(motor, 0x04) == 2);
            }
            if (std::abs(delta) <= 2) {
                CHECK(std::isfinite(driver->get_right_ascension()));
                CHECK(std::abs(driver->get_declination() - 87.0) < 0.0001);
                CHECK_NOTHROW(driver->set_tracking(true));
            } else {
                require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                                     alpacacore::AlpacaError::DriverException);
                require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                                     alpacacore::AlpacaError::DriverException);
                require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
                require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 87.0); },
                                     alpacacore::AlpacaError::InvalidOperation);
                driver->find_home();
                REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
                CHECK(driver->get_declination() > 89.99);
                CHECK_NOTHROW(driver->set_tracking(true));
            }
            driver->set_connected(false);
        }
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - firmware identity change drops the retained reference",
          "[celestron][cgx][telescope][integration]") {
    for (const auto axis : {0x10, 0x11}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        for (const auto motor : {0x10, 0x11}) mount.set_motor_identity(motor, {7, 17, 0, 25}, {0x17, 0x88});
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        driver->set_connected(false);
        mount.set_motor_identity(axis, {7, 17, 0, 26}, {0x17, 0x88});
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(driver->get_device_firmware() == (axis == 0x10 ? "RA 7.17.26, Dec 7.17.25" : "RA 7.17.25, Dec 7.17.26"));
        CHECK_FALSE(driver->get_at_home());
        require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                             alpacacore::AlpacaError::DriverException);
        require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                             alpacacore::AlpacaError::DriverException);
        require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
        for (const auto motor : {0x10, 0x11}) {
            CHECK(mount.count_command(motor, 0x0b) == 1);
            CHECK(mount.count_command(motor, 0x04) == 1);
        }
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        CHECK(driver->get_declination() > 89.99);
        CHECK_NOTHROW(driver->set_tracking(true));
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - desync and owned-link recovery require explicit FindHome",
          "[celestron][cgx][telescope][integration]") {
    for (const bool checksum_failure : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        if (checksum_failure)
            mount.corrupt_next_reply_checksum();
        else
            mount.drop_next_reply(0x10, 0x01, false);
        poll_now(*driver);  // the state poll's read meets the fault; the getters no longer read the axes (#1940)
        require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                             alpacacore::AlpacaError::DriverException);
        const auto reads = mount.count_command(0x10, 0x01);
        const auto refuse_reference = [&] {
            require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                                 alpacacore::AlpacaError::DriverException);
            require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                                 alpacacore::AlpacaError::DriverException);
            require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
            require_alpaca_error([&] { driver->slew_to_coordinates_async(6.0, 30.0); },
                                 alpacacore::AlpacaError::InvalidOperation);
            require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 30.0); },
                                 alpacacore::AlpacaError::InvalidOperation);
        };
        refuse_reference();
        CHECK(mount.count_command(0x10, 0x01) == reads);
        // A verified stop recovers transport matching, not encoder calibration.
        driver->abort_slew();
        REQUIRE(wait_until([&] { return stop_done(*driver); }));
        REQUIRE(driver->get_connected());
        refuse_reference();
        driver->set_connected(false);
        driver->set_connected(true);
        refuse_reference();
        for (const auto motor : {0x10, 0x11}) {
            CHECK(mount.count_command(motor, 0x0b) == 1);
            CHECK(mount.count_command(motor, 0x04) == 1);
        }
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        CHECK(driver->get_declination() > 89.99);
        CHECK_NOTHROW(driver->set_tracking(true));
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - retains exclusive claim until destruction and releases failed init",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    const auto close_file = [](FILE* file) { std::fclose(file); };
    const int observer_fd = ::open(mount.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    REQUIRE(observer_fd >= 0);
    std::unique_ptr<FILE, decltype(close_file)> observer(::fdopen(observer_fd, "r+"), close_file);
    if (!observer) ::close(observer_fd);
    REQUIRE(observer);
    const auto check_exclusive = [&](int expected) {
        int exclusive = -1;
        REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
        CHECK(exclusive == expected);
    };
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto first = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    auto second = alpacacore::test::create_cgx_telescope_poll_held(1, info);
    first->set_connected(true);
    first->set_connected(false);
    check_exclusive(1);
    require_alpaca_error([&] { second->set_connected(true); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(second->get_connected());
    first.reset();
    check_exclusive(0);
    CHECK_NOTHROW(second->set_connected(true));
    second->set_connected(false);
    second.reset();
    check_exclusive(0);

    // Fail after the fd has been opened and claimed, but before publishing Connected.
    mount.set_motor_identity(0x10, {7}, {0x17, 0x88});
    auto failed = alpacacore::test::create_cgx_telescope_poll_held(2, info);
    require_alpaca_error([&] { failed->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(failed->get_connected());
    CHECK_FALSE(failed->get_device_firmware());
    check_exclusive(0);
    mount.set_motor_identity(0x10, {7, 18}, {0x17, 0x88});
    auto replacement = alpacacore::test::create_cgx_telescope_poll_held(3, info);
    CHECK_NOTHROW(replacement->set_connected(true));
    check_exclusive(1);
    replacement.reset();
    check_exclusive(0);
}

TEST_CASE("Celestron CGX USB Telescope Driver - TrackingRate cannot enable an invalid recovered reference",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.drop_next_reply(0x11, 0x26, true);
    require_alpaca_error([&] { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::DriverException);
    // Join the packet through a reference-gated initiator, after its owned-fd
    // recovery. No successful stop or zero-rate write hides the cached true.
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_tracking()); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { static_cast<void>(driver->get_tracking_rate()); },
                         alpacacore::AlpacaError::DriverException);
    const auto ra_rates = mount.count_command(0x10, 0x06);
    const auto dec_rates = mount.count_command(0x11, 0x06);
    for (int rate = 0; rate < 3; ++rate)
        require_alpaca_error([&] { driver->set_tracking_rate(rate); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(mount.count_command(0x10, 0x06) == ra_rates);
    CHECK(mount.count_command(0x11, 0x06) == dec_rates);
    CHECK_NOTHROW(driver->set_tracking(false));
    CHECK_FALSE(driver->get_tracking());
    require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - cached hardware state refuses faulted and closed transport",
          "[celestron][cgx][telescope][integration]") {
    for (const int fault : {0, 1, 2}) {      // Timeout, checksum, removed device.
        for (const int state : {0, 1, 2}) {  // Home, Park, tracking.
            INFO("fault=" << fault << " state=" << state);
            alpacacore::test::FakeCelestronCgxMount mount;
            REQUIRE(mount.ok());
            mount.set_auto_complete_motions(true);
            auto info = test_info();
            info.port_path = mount.slave_path();
            auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
            driver->set_connected(true);
            if (state == 1)
                driver->park();
            else
                driver->find_home();
            REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
            if (state == 2) driver->set_tracking(true);
            if (fault == 0)
                mount.drop_next_reply(0x10, 0x01, false);
            else if (fault == 1)
                mount.corrupt_next_reply_checksum();
            else
                mount.sever_link();
            poll_now(*driver);  // the state poll's read meets the fault; the getters no longer read the axes (#1940)
            const std::vector<std::function<void()>> hardware_reads{[&] { static_cast<void>(driver->get_at_home()); },
                                                                    [&] { static_cast<void>(driver->get_at_park()); },
                                                                    [&] { static_cast<void>(driver->get_tracking()); },
                                                                    [&] { static_cast<void>(driver->get_tracking_rate()); },
                                                                    [&] { static_cast<void>(driver->get_slewing()); },
                                                                    [&] { static_cast<void>(driver->get_is_pulse_guiding()); }};
            const auto requests = mount.requests().size();
            if (fault == 2) {
                // A removed device is a lost link at once (decision 0009 rule 2a, #1940), not a fault: every cached
                // read is NotConnected and sends nothing.
                require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                                     alpacacore::AlpacaError::NotConnected);
                for (const auto& read : hardware_reads)
                    require_alpaca_error(read, alpacacore::AlpacaError::NotConnected);
                CHECK(mount.requests().size() == requests);
                CHECK_FALSE(driver->get_connected());
                continue;
            }
            require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                                 alpacacore::AlpacaError::DriverException);
            for (const auto& read : hardware_reads)
                require_alpaca_error(read, alpacacore::AlpacaError::DriverException);
            CHECK(mount.requests().size() == requests);
            CHECK(driver->get_connected());
            CHECK(driver->get_can_find_home());
            CHECK(driver->get_site_latitude() == 45.0);
            CHECK(driver->get_guide_rate().dec > 0.0);
            // The failed zero write recovers matching and rolls back to off,
            // but must not revalidate a prior true Home/Park observation.
            require_alpaca_error([&] { driver->set_tracking(false); }, alpacacore::AlpacaError::DriverException);
            CHECK_FALSE(driver->get_tracking());
            if (state != 2)
                require_alpaca_error([&] { static_cast<void>(driver->get_at_home()); },
                                     alpacacore::AlpacaError::DriverException);
            if (state == 1)
                require_alpaca_error([&] { static_cast<void>(driver->get_at_park()); },
                                     alpacacore::AlpacaError::DriverException);
            driver->unpark();
            REQUIRE(wait_until([&] {
                try {
                    return !driver->get_slewing();
                } catch (const alpacacore::AlpacaException&) {
                    return false;  // The stop worker first recovers matching.
                }
            }));
            CHECK_FALSE(driver->get_tracking());
            CHECK_FALSE(driver->get_at_park());
            CHECK_FALSE(driver->get_is_pulse_guiding());
            if (state != 2)
                require_alpaca_error([&] { static_cast<void>(driver->get_at_home()); },
                                     alpacacore::AlpacaError::DriverException);
            else
                CHECK_FALSE(driver->get_at_home());
            require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                                 alpacacore::AlpacaError::DriverException);
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
            CHECK(driver->get_declination() > 89.99);
            driver->set_connected(false);
        }
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - reference publication cannot adopt a concurrent fault generation",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    for (const bool flip : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        std::atomic<bool> hold_utc{false};
        std::promise<void> utc_entered, release_utc;
        const auto released = release_utc.get_future().share();
        auto driver =
            alpacacore::test::create_cgx_telescope_poll_held(0, info, alpacacore::util::default_task_clock(), [&] {
                if (hold_utc.exchange(false)) {
                    utc_entered.set_value();
                    released.wait();
                }
                return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
            });
        driver->set_connected(true);
        if (flip) {
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
            driver->set_tracking(true);
        }
        const auto target_ra = std::fmod(driver->get_sidereal_time() + 1.0, 24.0);
        mount.fragment_next_reply(std::pair<std::uint8_t, std::uint8_t>{0x11, 0x04});
        if (flip)
            driver->slew_to_coordinates_async(target_ra, 65.0);
        else
            driver->find_home();
        const bool label_ack_held = mount.wait_for_reply_fragment(2s);
        hold_utc.store(true);
        auto utc = std::async(std::launch::async, [&] { return driver->get_utc_date(); });
        const bool mutex_held = utc_entered.get_future().wait_for(2s) == std::future_status::ready;
        // Both labels will now be acknowledged, but publication needs mutex_,
        // held by UTCDate. The state poll takes no driver lock: its dropped read
        // faults AUX (#1940), and the lock-free IsPulseGuiding raises that fault.
        mount.drop_next_reply(0x10, 0x01, false);
        mount.release_reply_tail();
        poll_now(*driver);
        bool guide_read_failed = false;
        try {
            static_cast<void>(driver->get_is_pulse_guiding());
        } catch (const alpacacore::AlpacaException& error) {
            guide_read_failed = error.error_code() == alpacacore::AlpacaError::DriverException;
        }
        release_utc.set_value();  // Release every blocker before assertions/unwind.
        CHECK_NOTHROW(utc.get());
        REQUIRE(label_ack_held);
        REQUIRE(mutex_held);
        REQUIRE(guide_read_failed);
        REQUIRE(wait_until([&] {
            try {
                static_cast<void>(driver->get_slewing());
                return false;
            } catch (const alpacacore::AlpacaException&) {
                // A successful setter waits for the motion body to finish.
                try {
                    driver->set_tracking(false);
                    return true;
                } catch (const alpacacore::AlpacaException& error) {
                    return error.error_code() != alpacacore::AlpacaError::InvalidOperation;
                }
            }
        }));
        driver->unpark();
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        CHECK_FALSE(driver->get_at_home());
        require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                             alpacacore::AlpacaError::DriverException);
        require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
        CHECK(mount.count_command(0x10, 0x02) == 0);
        CHECK(mount.count_command(0x11, 0x02) == 0);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        CHECK(driver->get_declination() > 89.99);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - keeps Slewing true through the configured settle interval",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->set_slew_settle_time(10);

    driver->find_home();
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(2)));
    CHECK(driver->get_slewing());
    CHECK_FALSE(driver->get_at_home());
    clock.advance(std::chrono::seconds(10));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(driver->get_at_home());

    driver->set_tracking(true);
    const double target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(target_ra, 25.0);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(2)));
    CHECK(driver->get_tracking());
    CHECK(driver->get_slewing());
    clock.advance(std::chrono::seconds(10));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(driver->get_tracking());
    alpacacore::test::set_cgx_connected(*driver, clock, false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - the asynchronous equatorial slews are refused while Tracking is false",
          "[celestron][cgx][telescope][integration][tracking-guard]") {
    // CanSlew is false on this mount, so SlewToCoordinates and SlewToTarget are
    // MethodNotImplemented; the two Async forms share start_slew_operation().
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    REQUIRE_FALSE(driver->get_tracking());

    const double target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    const auto motion_commands = [&] {
        return mount.count_command(0x10, 0x02) + mount.count_command(0x11, 0x02) + mount.count_command(0x10, 0x0b) +
               mount.count_command(0x11, 0x0b);
    };
    const int before = motion_commands();
    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_alpaca_error([&] { driver->slew_to_coordinates_async(target_ra, 95.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(target_ra, 25.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_right_ascension(target_ra);
    driver->set_target_declination(25.0);
    require_alpaca_error([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_coordinates(target_ra, 25.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    CHECK(motion_commands() == before);
    CHECK_FALSE(driver->get_slewing());

    driver->set_tracking(true);
    CHECK_NOTHROW(driver->slew_to_coordinates_async(target_ra, 25.0));
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - failed tracking shutdown leaves flip calibration idle",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + (driver->get_side_of_pier() == 0 ? 1.0 : -1.0) + 24.0, 24.0);
    const int homes_before = mount.count_command(0x10, 0x0b) + mount.count_command(0x11, 0x0b);
    mount.drop_next_reply(0x10, 0x06, false);

    require_alpaca_error([&] { driver->slew_to_coordinates_async(target_ra, 25.0); },
                         alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_slewing());
    CHECK_FALSE(driver->get_tracking());
    require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(mount.count_command(0x10, 0x0b) + mount.count_command(0x11, 0x0b) == homes_before);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    CHECK(std::isfinite(driver->get_right_ascension()));
    CHECK(std::isfinite(driver->get_declination()));
    CHECK_NOTHROW(driver->set_tracking(true));
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - a failed slew start keeps its error when the tracking restore fails",
          "[celestron][cgx][telescope][integration]") {
    // A start that fails on the wire moves the session generation, and the handler then skips the restore. An
    // MC_GOTO_FAST ack with a payload fails the start and leaves the reference valid, so the restore runs. The
    // southern site makes the restore's RA command 0x07, which the slew's own tracking-off write never sends.
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    info.site_latitude_degrees = -33.9;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + (driver->get_side_of_pier() == 0 ? -1.0 : 1.0) + 24.0, 24.0);
    const int restores_before = mount.count_command(0x10, 0x07);
    mount.ack_next_with_payload(0x10, 0x02, {0x00});
    bool restore_fails = false;
    SECTION("the restore fails") {
        restore_fails = true;
        mount.drop_next_reply(0x10, 0x07, false);
    }
    SECTION("the restore succeeds") {}

    std::vector<std::string> errors;
    {
        struct SinkGuard {
            alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
            alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
            ~SinkGuard() {
                alpacacore::logging::set_log_sink(previous);
                alpacacore::logging::set_log_level(level);
            }
        } sink_guard;
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Error);
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Error && component == "Celestron CGX")
                    errors.emplace_back(message);
            });
        try {
            driver->slew_to_coordinates_async(target_ra, -25.0);
            FAIL_CHECK("Expected the slew start to fail");
        } catch (const alpacacore::AlpacaException& error) {
            INFO(error.what());
            CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(error.what()).find("MC_GOTO_FAST returned an unexpected payload") != std::string::npos);
        }
    }
    CHECK(mount.count_command(0x10, 0x07) == restores_before + 1);
    CHECK_FALSE(driver->get_slewing());
    CHECK(driver->get_tracking() == !restore_fails);
    CHECK(std::any_of(errors.begin(), errors.end(), [](const std::string& message) {
              return message.find("Could not restore tracking") != std::string::npos;
          }) == restore_fails);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - Unpark cancels a pending Park completion",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);

    driver->park();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    driver->unpark();

    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK_FALSE(driver->get_at_park());
    CHECK_FALSE(driver->get_slewing());
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    CHECK_FALSE(driver->get_at_park());
    CHECK(mount.count_command(0x10, 0x24) > 0);
    CHECK(mount.count_command(0x11, 0x24) > 0);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - disconnect waits only for the active pulse packet",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    driver->pulse_guide(0, 600000);
    REQUIRE(driver->get_is_pulse_guiding());

    const auto started = std::chrono::steady_clock::now();
    driver->set_connected(false);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(5));
    CHECK_FALSE(driver->get_connected());

    // If the first guide packet applied but its acknowledgement was lost,
    // retain ownership until hardware reports the packet inactive. A motion
    // initiator must wait rather than starting a later guide chunk or Home.
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.set_hold_guide_until_completed(true);
    mount.drop_next_reply(0x11, 0x26, true);
    // The first half's disconnect read the guide status too (#2431): wait for a read of this pulse's packet, or
    // the Home below can start before the pulse body's resync and fail on the dropped reply's timeout.
    const int guide_reads = mount.count_command(0x11, 0x27);
    require_alpaca_error([&] { driver->pulse_guide(0, 600000); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(wait_until([&] { return mount.count_command(0x11, 0x27) > guide_reads; }));
    CHECK(mount.axis_guiding(0x11));
    const int home_commands = mount.count_command(0x10, 0x0b);
    auto home = std::async(std::launch::async, [&] { driver->find_home(); });
    CHECK(home.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
    CHECK(mount.count_command(0x10, 0x0b) == home_commands);
    mount.complete_axis_guide(0x11);
    REQUIRE(home.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    home.get();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    CHECK(mount.count_command(0x11, 0x26) == 2);
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - routes a pointing-state change through home",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    // This case controls routing stages, not elapsed arrival corrections.
    // Keep pointing UTC fixed so manual completion cannot leave a new RA
    // correction unserviced; the elapsed-arrival case advances both clocks.
    const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
    auto driver =
        alpacacore::test::create_cgx_telescope_poll_held(0, info, alpacacore::util::default_task_clock(), now);
    driver->set_connected(true);
    require_alpaca_error([&] { driver->slew_to_coordinates(2.0, 15.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    driver->find_home();

    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_site_latitude(-45.0);
    driver->set_site_longitude(15.0);
    CHECK(driver->get_site_latitude() == -45.0);
    CHECK(driver->get_site_longitude() == 15.0);

    driver->set_tracking(true);
    const double ra = std::fmod(driver->get_sidereal_time() + 6.0, 24.0);
    driver->slew_to_coordinates_async(ra, 30.0);
    CHECK(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    CHECK(mount.count_command(0x10, 0x02) == 0);
    CHECK(mount.count_command(0x11, 0x02) == 0);

    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x02) == 1 && mount.count_command(0x11, 0x02) == 1; }));
    mount.complete_axis_slew(0x10);
    mount.complete_axis_slew(0x11);
    CHECK(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(driver->get_tracking());

    const auto ra_before_sync = mount.axis_position(0x10);
    const auto dec_before_sync = mount.axis_position(0x11);
    driver->sync_to_coordinates(ra, 31.0);
    CHECK(driver->get_tracking());
    const auto encoder_revolution = alpacacore::vendor::celestron::kCgxEncoderStepsPerRevolution;
    const auto expected_rehome_label = [encoder_revolution](std::uint32_t home, std::uint32_t before,
                                                            std::uint32_t after) {
        const auto shift = alpacacore::vendor::celestron::signed_cgx_encoder_delta(after, before);
        return static_cast<std::uint32_t>((static_cast<std::int64_t>(home) + shift + encoder_revolution) %
                                          encoder_revolution);
    };
    const auto expected_ra_home =
        expected_rehome_label(encoder_revolution / 4, ra_before_sync, mount.axis_position(0x10));
    const auto expected_dec_home =
        expected_rehome_label(encoder_revolution / 2, dec_before_sync, mount.axis_position(0x11));

    const double same_side_ra = std::fmod(driver->get_sidereal_time() + 1.0, 24.0);
    driver->slew_to_coordinates_async(same_side_ra, 25.0);
    CHECK(driver->get_slewing());
    CHECK(mount.count_command(0x10, 0x0b) == 2);
    CHECK(mount.count_command(0x11, 0x0b) == 2);
    mount.complete_axis_slew(0x10);
    mount.complete_axis_slew(0x11);
    CHECK(wait_until([&] { return !driver->get_slewing(); }));

    const double opposite_side_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(opposite_side_ra, 25.0);
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    CHECK(mount.count_command(0x10, 0x0b) == 3);
    CHECK(mount.count_command(0x11, 0x0b) == 3);
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x02) == 3 && mount.count_command(0x11, 0x02) == 3; }));
    CHECK(mount.axis_position(0x10) == expected_ra_home);
    CHECK(mount.axis_position(0x11) == expected_dec_home);
    mount.complete_axis_slew(0x10);
    mount.complete_axis_slew(0x11);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(driver->get_tracking());
    CHECK(std::abs(std::remainder(driver->get_right_ascension() - opposite_side_ra, 24.0) * 15.0 *
                   std::cos(25.0 * std::numbers::pi / 180.0)) < 0.02);
    CHECK(std::abs(driver->get_declination() - 25.0) < 2.2e-5);

    const double abort_target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(abort_target_ra, 25.0);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x02) == 4; }));
    CHECK_FALSE(driver->get_tracking());
    driver->abort_slew();
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - site writes after Sync preserve mechanical hemisphere",
          "[celestron][cgx][telescope][integration]") {
    using namespace alpacacore::vendor::celestron;
    const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
    for (const double home_latitude : {45.0, -45.0}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = -home_latitude;
        auto driver =
            alpacacore::test::create_cgx_telescope_poll_held(0, info, alpacacore::util::default_task_clock(), now);
        driver->set_connected(true);
        // The reference uses the site at FindHome, not the constructor's site.
        driver->set_site_latitude(home_latitude);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        CHECK(std::abs(driver->get_declination() - (home_latitude > 0.0 ? 90.0 : -90.0)) < 2.2e-5);
        driver->set_tracking(true);
        driver->sync_to_coordinates(6.0, home_latitude > 0.0 ? 87.0 : -87.0);
        const double ra = driver->get_right_ascension();
        const double dec = driver->get_declination();
        const int side = driver->get_side_of_pier();
        const double lst = driver->get_sidereal_time();
        const auto ra_label = mount.axis_position(0x10);
        const auto dec_label = mount.axis_position(0x11);
        const auto writes_before = mount.requests().size();
        for (const double latitude : {-90.0, 0.0, 90.0, -home_latitude}) {
            driver->set_site_latitude(latitude);
            CHECK(driver->get_site_latitude() == latitude);
            CHECK(std::abs(driver->get_declination() - dec) < 1e-9);
            CHECK(std::abs(std::remainder(driver->get_right_ascension() - ra, 24.0)) < 1e-9);
        }
        const double altitude = driver->get_altitude();
        const double azimuth = driver->get_azimuth();
        for (const double longitude : {-180.0, 0.0, 180.0, 15.0}) {
            driver->set_site_longitude(longitude);
            CHECK(driver->get_site_longitude() == longitude);
            const double shift = (longitude + 75.0) / 15.0;
            CHECK(std::abs(std::remainder(driver->get_sidereal_time() - lst - shift, 24.0)) < 1e-9);
            CHECK(std::abs(std::remainder(driver->get_right_ascension() - ra - shift, 24.0)) < 1e-9);
            CHECK(std::abs(driver->get_declination() - dec) < 1e-9);
            CHECK(std::abs(driver->get_altitude() - altitude) < 1e-9);
            CHECK(std::abs(std::remainder(driver->get_azimuth() - azimuth, 360.0)) < 1e-9);
            CHECK(driver->get_side_of_pier() == side);
        }
        CHECK(driver->get_tracking());
        CHECK_FALSE(driver->get_slewing());
        CHECK(driver->get_target_right_ascension() == 6.0);
        CHECK(driver->get_target_declination() == (home_latitude > 0.0 ? 87.0 : -87.0));
        CHECK(mount.axis_position(0x10) == ra_label);
        CHECK(mount.axis_position(0x11) == dec_label);
        const auto requests = mount.requests();
        for (auto index = writes_before; index < requests.size(); ++index)
            CHECK(requests[index].command == 0x01);  // Coordinate reads only; no site-triggered command.

        driver->set_tracking_rate(1);
        const auto rates = mount.requests();
        const auto rate = std::find_if(rates.rbegin(), rates.rend(), [](const auto& frame) {
            return frame.destination == 0x10 && (frame.command == 0x06 || frame.command == 0x07);
        });
        REQUIRE(rate != rates.rend());
        CHECK(rate->command == (home_latitude > 0.0 ? 0x06 : 0x07));
        CHECK(rate->payload == std::vector<std::uint8_t>{0xff, 0xfd});
        for (const int direction : {0, 1, 2, 3}) {
            driver->pulse_guide(direction, 100);
            const auto guides = mount.requests();
            const auto guide =
                std::find_if(guides.rbegin(), guides.rend(), [](const auto& frame) { return frame.command == 0x26; });
            REQUIRE(guide != guides.rend());
            int velocity = (direction == 1 || direction == 3) ? 50 : -50;
            if (home_latitude < 0.0) velocity = -velocity;
            if (direction < 2 && side == 1) velocity = -velocity;
            CHECK(guide->payload ==
                  std::vector<std::uint8_t>{static_cast<std::uint8_t>(static_cast<std::int8_t>(velocity)), 10});
            REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));
        }
        // Only a new explicit reference adopts the now opposite site hemisphere.
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        CHECK(std::abs(driver->get_declination() - (home_latitude > 0.0 ? -90.0 : 90.0)) < 2.2e-5);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - site writes refuse active motion without cancelling it",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    require_alpaca_error([&] { driver->set_site_latitude(-45.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->set_site_longitude(15.0); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK(driver->get_site_latitude() == 45.0);
    CHECK(driver->get_site_longitude() == -75.0);
    CHECK(mount.axis_homing(0x10));
    CHECK(mount.axis_homing(0x11));
    CHECK(mount.count_command(0x10, 0x24) == 0);
    CHECK(mount.count_command(0x11, 0x24) == 0);
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(0, 100);
    require_alpaca_error([&] { driver->set_site_latitude(-45.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->set_site_longitude(15.0); }, alpacacore::AlpacaError::InvalidOperation);
    const bool guide_still_active = mount.axis_guiding(0x11);
    mount.complete_axis_guide(0x11);
    CHECK(guide_still_active);
    CHECK(driver->get_site_latitude() == 45.0);
    CHECK(driver->get_site_longitude() == -75.0);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }));
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - reports a completed GOTO that misses its target",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    std::atomic<std::int64_t> epoch_seconds{1800000000};
    const auto now = [&] { return std::chrono::system_clock::time_point{std::chrono::seconds(epoch_seconds.load())}; };
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver =
        alpacacore::test::create_cgx_telescope_poll_held(0, info, alpacacore::util::default_task_clock(), now);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);

    const double target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(target_ra, 25.0);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x02) == 1 && mount.count_command(0x11, 0x02) == 1; }));
    const auto decode_position = [](const auto& payload) {
        return (static_cast<std::uint32_t>(payload[0]) << 16) | (static_cast<std::uint32_t>(payload[1]) << 8) |
               static_cast<std::uint32_t>(payload[2]);
    };
    const auto latest_target = [&](std::uint8_t axis, std::uint8_t command = 0x02) {
        const auto frames = mount.requests();
        const auto found = std::find_if(frames.rbegin(), frames.rend(), [axis, command](const auto& frame) {
            return frame.destination == axis && frame.command == command;
        });
        REQUIRE(found != frames.rend());
        return decode_position(found->payload);
    };
    const auto first_ra_target = latest_target(0x10);
    const auto first_dec_target = latest_target(0x11);

    // The axes finish at the dispatch-time encoder counts while pointing UTC
    // advances by 20 s. The bounded arrival refinement must retarget them.
    epoch_seconds.fetch_add(20);
    mount.complete_axis_slew_at(0x10, first_ra_target);
    mount.complete_axis_slew_at(0x11, first_dec_target);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x17) == 1; }));
    CHECK(mount.count_command(0x10, 0x02) == 1);
    CHECK(mount.count_command(0x11, 0x02) == 1);
    CHECK(mount.count_command(0x11, 0x17) == 0);
    const auto corrected_ra_target = latest_target(0x10, 0x17);
    const auto corrected_dec_target = latest_target(0x11);
    CHECK(corrected_ra_target != first_ra_target);
    mount.complete_axis_slew_at(0x10, corrected_ra_target);
    mount.complete_axis_slew_at(0x11, corrected_dec_target);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK(driver->get_tracking());
    CHECK(std::abs(std::remainder(driver->get_right_ascension() - target_ra, 24.0)) * 15.0 * 3600.0 <= 3.0);
    CHECK(std::abs(driver->get_declination() - 25.0) < 2.2e-5);

    const double bad_target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    const int stops_before = mount.count_command(0x10, 0x24);
    driver->slew_to_coordinates_async(bad_target_ra, 25.0);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x02) == 2 && mount.count_command(0x11, 0x02) == 2; }));
    const auto bad_ra_target = latest_target(0x10);
    const auto bad_dec_target = latest_target(0x11);
    mount.complete_axis_slew_at(0x10, bad_ra_target + 0x100000U);
    mount.complete_axis_slew_at(0x11, bad_dec_target);

    REQUIRE(wait_until([&] {
        try {
            return !driver->get_slewing();
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
    }));
    require_alpaca_error([&] { static_cast<void>(driver->get_slewing()); }, alpacacore::AlpacaError::DriverException);
    CHECK(mount.count_command(0x10, 0x24) > stops_before);
    CHECK(mount.count_command(0x11, 0x24) > 0);
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - elapsed arrival uses native slow correction without restarting Dec",
          "[celestron][cgx][telescope][integration]") {
    using namespace std::chrono_literals;
    for (const double declination : {14.04, 85.0}) {
        for (const bool flip : {false, true}) {
            INFO("Dec=" << declination << " flip=" << flip);
            alpacacore::test::FakeCelestronCgxMount mount;
            REQUIRE(mount.ok());
            mount.set_auto_complete_motions(true);
            std::atomic<std::int64_t> epoch_milliseconds{1800000000000};
            const auto now = [&] {
                return std::chrono::system_clock::time_point{std::chrono::milliseconds(epoch_milliseconds.load())};
            };
            auto info = test_info();
            info.port_path = mount.slave_path();
            alpacacore::test::FakeTaskClock clock;
            auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, now);
            alpacacore::test::CgxClockCleanup cleanup{driver, clock};
            driver->set_connected(true);
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
            driver->set_tracking(true);
            mount.set_auto_complete_motions(false);
            mount.set_axis_velocity_counts_per_second(0x10, 10000000);
            mount.set_axis_velocity_counts_per_second(0x11, 10000000);
            const double target_ra = std::fmod(driver->get_sidereal_time() + (flip ? 1.0 : 23.0), 24.0);
            driver->slew_to_coordinates_async(target_ra, declination);
            if (flip) {
                REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
                REQUIRE(clock.wait_for_waiters(1, 2s));
                mount.complete_axis_home(0x10);
                mount.complete_axis_home(0x11);
                clock.advance(200ms);
                REQUIRE(clock.wait_for_woken_settled(2s));
            }
            REQUIRE(wait_until(
                [&] { return mount.count_command(0x10, 0x02) == 1 && mount.count_command(0x11, 0x02) == 1; }));
            REQUIRE(clock.wait_for_waiters(1, 2s));
            // Finish at the dispatch-time target after Earth has advanced
            // ~18.3 arcsec in RA. At Dec 85 the projected miss is only ~1.6
            // arcsec, so projection would falsely accept even a 3-arcsec gate.
            epoch_milliseconds.fetch_add(1220);
            mount.advance_motion(1220ms);
            clock.advance(1220ms);
            REQUIRE(clock.wait_for_woken_settled(2s));
            const auto ra_error_arcseconds = [&] {
                return std::abs(std::remainder(driver->get_right_ascension() - target_ra, 24.0)) * 15.0 * 3600.0;
            };
            CHECK(ra_error_arcseconds() > 18.0);
            CHECK(ra_error_arcseconds() < 19.0);
            REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x17) == 1; }));
            CHECK(mount.count_command(0x10, 0x02) == 1);
            CHECK(mount.count_command(0x11, 0x02) == 1);
            CHECK(mount.count_command(0x11, 0x17) == 0);
            CHECK(driver->get_slewing());
            CHECK_FALSE(driver->get_tracking());
            // The correction targets current UTC; allow another 20 ms of
            // elapsed motion/time and verify actual, not substituted, reads.
            epoch_milliseconds.fetch_add(20);
            mount.advance_motion(20ms);
            REQUIRE(clock.wait_for_waiters(1, 2s));
            clock.advance(20ms);
            REQUIRE(clock.wait_for_woken_settled(2s));
            REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
            CHECK(ra_error_arcseconds() <= 3.0);
            CHECK(std::abs(driver->get_declination() - declination) * 3600.0 <= 3.0);
            CHECK(mount.count_command(0x10, 0x02) == 1);
            CHECK(mount.count_command(0x10, 0x17) == 1);
            CHECK(mount.count_command(0x11, 0x02) == 1);
            CHECK(mount.count_command(0x11, 0x17) == 0);
            CHECK(driver->get_tracking());
            alpacacore::test::set_cgx_connected(*driver, clock, false);
        }
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - stalled GOTO times out and stops both axes",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    mount.set_auto_complete_motions(false);
    driver->set_tracking(true);

    const double target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(target_ra, 25.0);
    REQUIRE(wait_until([&] { return mount.count_command(0x10, 0x02) == 1 && mount.count_command(0x11, 0x02) == 1; }));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(2)));

    const int ra_stops_before = mount.count_command(0x10, 0x24);
    const int dec_stops_before = mount.count_command(0x11, 0x24);
    clock.advance(std::chrono::seconds(180));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    REQUIRE(wait_until([&] {
        try {
            static_cast<void>(driver->get_slewing());
            return false;
        } catch (const alpacacore::AlpacaException& error) {
            return error.error_code() == alpacacore::AlpacaError::DriverException;
        }
    }));
    CHECK(mount.count_command(0x10, 0x24) > ra_stops_before);
    CHECK(mount.count_command(0x11, 0x24) > dec_stops_before);
    alpacacore::test::set_cgx_connected(*driver, clock, false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - stops both axes when either GOTO acknowledgement is lost",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);

    const auto try_failed_goto = [&](std::uint8_t axis, bool command_applies) {
        // Each lost ACK changes the AUX session generation. A new Home is
        // required before the next injection can actually reach its GOTO.
        driver->find_home();
        REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
        mount.complete_axis_home(0x10);
        mount.complete_axis_home(0x11);
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        driver->set_tracking(true);
        const int goto_before = mount.count_command(axis, 0x02);
        const int ra_stops_before = mount.count_command(0x10, 0x24);
        const int dec_stops_before = mount.count_command(0x11, 0x24);
        mount.drop_next_reply(axis, 0x02, command_applies);
        const double target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
        require_alpaca_error([&] { driver->slew_to_coordinates_async(target_ra, 25.0); },
                             alpacacore::AlpacaError::DriverException);
        CHECK_FALSE(driver->get_slewing());
        CHECK_FALSE(driver->get_tracking());
        CHECK(mount.count_command(axis, 0x02) == goto_before + 1);
        CHECK(mount.count_command(0x10, 0x24) > ra_stops_before);
        CHECK(mount.count_command(0x11, 0x24) > dec_stops_before);
        CHECK_FALSE(mount.axis_homing(0x10));
        CHECK_FALSE(mount.axis_homing(0x11));
        require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                             alpacacore::AlpacaError::DriverException);
        require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                             alpacacore::AlpacaError::DriverException);
        require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
    };

    try_failed_goto(0x10, true);
    try_failed_goto(0x11, false);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - stops the responsive axis when the sibling stop replies are lost",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    const auto verify_partial_stop = [&](std::uint8_t silent_axis, std::uint8_t responsive_axis) {
        driver->find_home();
        REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
        const int silent_stops_before = mount.count_command(silent_axis, 0x24);
        const int stops_before = mount.count_command(responsive_axis, 0x24);
        for (int attempt = 0; attempt < 2; ++attempt) mount.drop_next_reply(silent_axis, 0x24, false);
        // The final command applies but its ACK is lost. Verification, not a
        // second cancelled-worker stop, must establish that Home has stopped.
        mount.drop_next_reply(silent_axis, 0x24, true);

        auto abort = std::async(std::launch::async, [&] { driver->abort_slew(); });
        REQUIRE(wait_until([&] { return mount.count_command(silent_axis, 0x24) > silent_stops_before; }));
        CHECK(abort.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
        abort.get();

        // Three lost stop ACKs cost 3 * 500 ms, followed by the sole stop
        // owner's 3 s busy-verification budget. The held-ACK test above pins
        // intermediate Slewing without racing this worker's completion.
        // Bound that worker drain separately from the nonblocking initiator.
        REQUIRE(wait_until([&] { return stop_done(*driver); }, std::chrono::seconds(5)));

        CHECK(mount.count_command(silent_axis, 0x24) == silent_stops_before + 3);
        CHECK(mount.count_command(responsive_axis, 0x24) == stops_before + 3);
        CHECK_FALSE(mount.axis_homing(0x10));
        CHECK_FALSE(mount.axis_homing(0x11));
        // Missing stop ACKs cannot justify a cached tracking-state claim.
        require_alpaca_error([&] { static_cast<void>(driver->get_tracking()); },
                             alpacacore::AlpacaError::DriverException);
        CHECK_FALSE(driver->get_slewing());
    };
    verify_partial_stop(0x10, 0x11);
    verify_partial_stop(0x11, 0x10);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - unresolved guide failure cannot be replaced by another pulse",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
    driver->set_tracking(true);

    mount.set_hold_guide_until_completed(true);
    mount.drop_next_reply(0x11, 0x26, true);
    mount.drop_next_reply(0x11, 0x27, false);
    require_alpaca_error([&] { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(wait_until([&] { return mount.count_command(0x11, 0x27) > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    require_alpaca_error([&] { static_cast<void>(driver->get_is_pulse_guiding()); },
                         alpacacore::AlpacaError::DriverException);
    const int guide_commands = mount.count_command(0x11, 0x26);
    require_alpaca_error([&] { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::DriverException);
    CHECK(mount.count_command(0x11, 0x26) == guide_commands);

    mount.complete_axis_guide(0x11);
    driver->abort_slew();
    REQUIRE(wait_until([&] { return stop_done(*driver); }));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// The motion owner (#2263): a command owns only the axes it moves. The cases below run on the fake clock with the state
// poll held, so every clock waiter is a motion or guide body.
namespace {

// The code and text a call threw, or an empty text when it returned.
struct CallResult {
    int code = 0;
    std::string text;
};

CallResult call_result(const std::function<void()>& call) {
    try {
        call();
    } catch (const alpacacore::AlpacaException& error) {
        return {error.error_code(), error.what()};
    }
    return {};
}

constexpr const char* kOverlapRefusal = "PulseGuide cannot overlap telescope motion or another guide pulse";

// Connect, home (the fake completes it at once) and turn Tracking on.
void connect_home_and_track(alpacacore::test::FakeCelestronCgxMount& mount, alpacacore::TelescopeDriver& driver) {
    mount.set_auto_complete_motions(true);
    driver.set_connected(true);
    driver.find_home();
    REQUIRE(wait_until([&] { return driver.get_at_home() && !driver.get_slewing(); }));
    driver.set_tracking(true);
}

}  // namespace

TEST_CASE("Celestron CGX USB Telescope Driver - a slew, Park or FindHome supersedes a slew in flight (#2263)",
          "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    // A second slew, Park or FindHome during a slew; then a Park during a FindHome, and a FindHome during a Park.
    const int leg = GENERATE(0, 1, 2, 3, 4);
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // The first command's GOTO or home stays in flight: its body waits on the clock for the mount to report it done.
    mount.set_auto_complete_motions(false);
    if (leg <= 2) driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0), 25.0);
    if (leg == 3) driver->find_home();
    if (leg == 4) driver->park();
    REQUIRE(clock.wait_for_waiters(1, 2s));
    const int ra_stops = mount.count_command(0x10, 0x24);
    const int dec_stops = mount.count_command(0x11, 0x24);
    const int ra_gotos = mount.count_command(0x10, 0x02);
    const int dec_gotos = mount.count_command(0x11, 0x02);

    mount.set_auto_complete_motions(true);
    const double second_ra = std::fmod(driver->get_sidereal_time() - 2.0 + 24.0, 24.0);
    const auto second = call_result([&] {
        if (leg == 0) driver->slew_to_coordinates_async(second_ra, 30.0);
        if (leg == 1 || leg == 3) driver->park();
        if (leg == 2 || leg == 4) driver->find_home();
    });
    const bool landed = wait_until([&] {
        clock.advance(20ms);
        return stop_done(*driver);
    });

    INFO("second command: " << second.text);
    CHECK(second.text.empty());
    CHECK(landed);
    // The superseded slew sent no stop: its axes belong to the new command.
    CHECK(mount.count_command(0x10, 0x24) == ra_stops);
    CHECK(mount.count_command(0x11, 0x24) == dec_stops);
    if (leg == 0) {
        CHECK(mount.count_command(0x10, 0x02) == ra_gotos + 1);
        CHECK(mount.count_command(0x11, 0x02) == dec_gotos + 1);
        CHECK(driver->get_tracking());
        CHECK(driver->get_target_right_ascension() == second_ra);
        CHECK(driver->get_target_declination() == 30.0);
    }
    if (leg == 1 || leg == 3) CHECK(driver->get_at_park());
    if (leg == 2 || leg == 4) CHECK(driver->get_at_home());
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a PulseGuide on the same axis supersedes the older pulse without a join "
    "(#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(0, 600000);  // North: a chain of 255-centisecond Dec packets, the first one held
    REQUIRE(clock.wait_for_waiters(1, 2s));
    const int dec_packets = mount.count_command(0x11, 0x26);

    // South on the same axis. A join would wait for the held packet to end.
    auto second = std::async(std::launch::async, [&] { driver->pulse_guide(1, 100); });
    const auto returned = second.wait_for(2s);
    const bool guiding_while_held = driver->get_is_pulse_guiding();
    const bool done = wait_until([&] {
        mount.complete_axis_guide(0x11);
        clock.advance(20ms);
        return second.wait_for(0ms) == std::future_status::ready && !driver->get_is_pulse_guiding();
    });
    const auto result = call_result([&] { second.get(); });

    INFO("second pulse: " << result.text);
    CHECK(result.text.empty());
    CHECK((returned == std::future_status::ready));
    CHECK(guiding_while_held);
    REQUIRE(done);
    // The older pulse chained nothing more on the axis it lost: one packet each.
    CHECK(mount.count_command(0x11, 0x26) == dec_packets + 1);
    CHECK(mount.count_command(0x10, 0x26) == 0);
    const auto frames = mount.requests();
    const auto last =
        std::find_if(frames.rbegin(), frames.rend(), [](const auto& frame) { return frame.command == 0x26; });
    REQUIRE(last != frames.rend());
    CHECK(last->destination == 0x11);
    CHECK((last->payload.size() == 2 && last->payload[1] == 10));
}

TEST_CASE("Celestron CGX USB Telescope Driver - a PulseGuide on the other axis runs beside a pulse (#2263)",
          "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    driver->pulse_guide(0, 2000);  // North: one Dec packet, held
    REQUIRE(clock.wait_for_waiters(1, 2s));
    const auto east = call_result([&] { driver->pulse_guide(2, 100); });  // East: RA
    const bool both_guiding = mount.axis_guiding(0x10) && mount.axis_guiding(0x11);

    // The RA pulse ends; the Dec pulse still runs.
    mount.complete_axis_guide(0x10);
    for (int i = 0; i < 10; ++i) {
        clock.advance(20ms);
        static_cast<void>(clock.wait_for_woken_settled(100ms));
    }
    const bool dec_still_guiding = mount.axis_guiding(0x11) && driver->get_is_pulse_guiding();
    const bool done = wait_until([&] {
        mount.complete_axis_guide(0x11);
        clock.advance(20ms);
        return !driver->get_is_pulse_guiding();
    });

    INFO("RA pulse: " << east.text);
    CHECK(east.text.empty());
    CHECK(both_guiding);
    CHECK(dec_still_guiding);
    CHECK(done);
    CHECK(mount.count_command(0x10, 0x26) == (east.text.empty() ? 1 : 0));
    CHECK(mount.count_command(0x11, 0x26) == 1);
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - PulseGuide during a slew, Park or FindHome is refused and changes "
    "nothing (#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a slew in its settle (Tracking on), a Park, a FindHome
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    if (leg == 0) {
        driver->set_slew_settle_time(10);
        driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0), 25.0);
        REQUIRE(clock.wait_for_waiters(1, 2s));  // landed, tracking restored, in the 10 s settle
        REQUIRE(driver->get_tracking());
    } else {
        mount.set_auto_complete_motions(false);
        if (leg == 1) driver->park();
        if (leg == 2) driver->find_home();
        REQUIRE(clock.wait_for_waiters(1, 2s));  // homing
    }
    const auto pulse = call_result([&] { driver->pulse_guide(0, 100); });
    const bool still_slewing = driver->get_slewing();
    const int ra_packets = mount.count_command(0x10, 0x26);
    const int dec_packets = mount.count_command(0x11, 0x26);
    const int stops = mount.count_command(0x10, 0x24) + mount.count_command(0x11, 0x24);

    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    const bool landed = wait_until([&] {
        clock.advance(1s);
        return stop_done(*driver);
    });

    CHECK(pulse.code == alpacacore::AlpacaError::InvalidOperation);
    CHECK(pulse.text == kOverlapRefusal);
    CHECK(still_slewing);
    CHECK(ra_packets == 0);
    CHECK(dec_packets == 0);
    CHECK(stops == 0);
    CHECK(landed);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    if (leg == 1) CHECK(driver->get_at_park());
    if (leg == 2) CHECK(driver->get_at_home());
}

TEST_CASE("Celestron CGX USB Telescope Driver - a slew or FindHome that fails leaves PulseGuide accepted (#2263)",
          "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a stalled GOTO times out, a FindHome times out, a GOTO ACK is lost
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);
    mount.set_auto_complete_motions(false);

    const double target_ra = std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    if (leg == 2) {
        mount.drop_next_reply(0x10, 0x02, true);
        require_alpaca_error([&] { driver->slew_to_coordinates_async(target_ra, 25.0); },
                             alpacacore::AlpacaError::DriverException);
    } else {
        if (leg == 0) driver->slew_to_coordinates_async(target_ra, 25.0);
        if (leg == 1) driver->find_home();
        REQUIRE(clock.wait_for_waiters(1, 2s));
        clock.advance(180s);
        REQUIRE(clock.wait_for_woken_settled(2s));
        REQUIRE(wait_until([&] {
            return call_result([&] { static_cast<void>(driver->get_slewing()); }).code ==
                   alpacacore::AlpacaError::DriverException;
        }));
    }

    if (leg == 0) {
        // The reference survives a stalled GOTO's stop: Tracking on, and the pulse goes out.
        driver->set_tracking(true);
        mount.set_hold_guide_until_completed(true);
        const auto pulse = call_result([&] { driver->pulse_guide(0, 100); });
        const bool done = wait_until([&] {
            mount.complete_axis_guide(0x11);
            clock.advance(20ms);
            return !driver->get_is_pulse_guiding();
        });
        INFO("pulse: " << pulse.text);
        CHECK(pulse.text.empty());
        CHECK(done);
    } else {
        // The reference is gone, so Tracking stays off: the refusal is Tracking's, not the motion overlap's.
        const auto pulse = call_result([&] { driver->pulse_guide(0, 100); });
        CHECK(pulse.code == alpacacore::AlpacaError::InvalidOperation);
        CHECK(pulse.text == "PulseGuide requires tracking to be enabled");
    }
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a PulseGuide on an axis whose pulse start was not acknowledged is "
    "refused (#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    mount.drop_next_reply(0x11, 0x26, true);  // the packet applies, its ACK is lost
    require_alpaca_error([&] { driver->pulse_guide(0, 600000); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(clock.wait_for_waiters(1, 2s));  // the body watches the unacknowledged packet
    const int dec_packets = mount.count_command(0x11, 0x26);
    const auto again = call_result([&] { driver->pulse_guide(1, 100); });
    const int dec_packets_after = mount.count_command(0x11, 0x26);
    const bool done = wait_until([&] {
        mount.complete_axis_guide(0x11);
        clock.advance(20ms);
        return !driver->get_is_pulse_guiding();
    });

    CHECK(again.code == alpacacore::AlpacaError::InvalidOperation);
    CHECK(again.text == kOverlapRefusal);
    CHECK(dec_packets_after == dec_packets);
    CHECK(done);
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a slew sent while a meridian-flip slew, FindHome or Park homes is "
    "refused (#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a flip slew, FindHome, Park
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // Each first command homes both axes, which drops the pointing reference. A negative hour angle puts the flip
    // slew's target on the BeyondPole side, so it routes through home and rebases there.
    mount.set_auto_complete_motions(false);
    const double flip_ra = std::fmod(driver->get_sidereal_time() + 1.0, 24.0);
    if (leg == 0) driver->slew_to_coordinates_async(flip_ra, 25.0);
    if (leg == 1) driver->find_home();
    if (leg == 2) driver->park();
    REQUIRE(wait_until([&] { return mount.axis_homing(0x10) && mount.axis_homing(0x11); }));
    REQUIRE(clock.wait_for_waiters(1, 2s));
    const auto frames_before = mount.requests().size();

    const auto second = call_result(
        [&] { driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 2.0, 24.0), 30.0); });
    const auto frames_after = mount.requests().size();

    INFO("second slew: " << second.text);
    CHECK(second.code == alpacacore::AlpacaError::InvalidOperation);
    if (leg == 0) {
        // Not FindHome advice: the client only has to wait for the flip, or abort it.
        CHECK(second.text == "Celestron CGX is homing for a meridian flip; wait for it or AbortSlew first");
    } else {
        CHECK(second.text == "Run FindHome before slewing the Celestron CGX");
    }
    CHECK(frames_after == frames_before);
    CHECK(driver->get_slewing());

    // The first command is untouched and still finishes.
    mount.set_auto_complete_motions(true);
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    CHECK(wait_until([&] {
        clock.advance(20ms);
        return stop_done(*driver);
    }));
    if (leg == 0) {
        CHECK(driver->get_tracking());
        CHECK(driver->get_target_right_ascension() == flip_ra);
    }
    if (leg == 1) CHECK(driver->get_at_home());
    if (leg == 2) CHECK(driver->get_at_park());
}

TEST_CASE("Celestron CGX USB Telescope Driver - a body whose stop finds it superseded writes no state (#2263)",
          "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a flip slew superseded by FindHome, FindHome by Park, a slew by FindHome
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    // Declared before the driver, so they outlive its threads: the body's INFO for a throw after it was superseded,
    // the slot's WARN for a body that rethrew instead, and the latch the body's stop blocks on.
    std::atomic<int> superseded_infos{0};
    std::atomic<int> stale_warns{0};
    struct SinkRestore {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkRestore() { alpacacore::logging::set_log_sink(previous); }
    } restore;
    alpacacore::test::Latch latch;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (message.find("ended after a newer command took its axes") != std::string_view::npos)
                superseded_infos.fetch_add(1);
            if (level == alpacacore::logging::LogLevel::Warn &&
                message.find("stale operation failed") != std::string_view::npos)
                stale_warns.fetch_add(1);
        });

    // The first command's home or GOTO stays in flight; its body waits on the clock between completion reads.
    mount.set_auto_complete_motions(false);
    if (leg == 0) driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 1.0, 24.0), 25.0);
    if (leg == 1) driver->find_home();
    if (leg == 2) driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0), 25.0);
    REQUIRE(clock.wait_for_waiters(1, 2s));

    // Its next RA completion read gets a malformed status and throws. The body checks it is not superseded, then
    // its stop blocks here, before it takes the lock every claim is made under; the second command claims meanwhile.
    alpacacore::vendor::celestron::set_stop_worker_hook_for_testing(*driver, [&] { latch.block(); });
    mount.ack_next_with_payload(0x10, leg == 2 ? 0x13 : 0x12, {0x01});
    clock.advance(200ms);
    REQUIRE(latch.wait_entered());
    const int ra_stops = mount.count_command(0x10, 0x24);
    if (leg == 1) {
        driver->park();
    } else {
        driver->find_home();
    }
    latch.release();
    const bool body_ended = wait_until([&] { return superseded_infos.load() + stale_warns.load() > 0; });
    alpacacore::vendor::celestron::set_stop_worker_hook_for_testing(*driver, {});

    CHECK(body_ended);
    CHECK(superseded_infos.load() == 1);
    CHECK(stale_warns.load() == 0);
    CHECK(mount.count_command(0x10, 0x24) == ra_stops);
    // The FindHome or Park in flight dropped the reference; the superseded body did not restore it.
    const auto slew = call_result(
        [&] { driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 2.0, 24.0), 30.0); });
    INFO("slew: " << slew.text);
    CHECK(slew.text == "Run FindHome before slewing the Celestron CGX");

    mount.set_auto_complete_motions(true);
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    CHECK(wait_until([&] {
        clock.advance(20ms);
        return stop_done(*driver);
    }));
    if (leg == 1) {
        CHECK(driver->get_at_park());
    } else {
        CHECK(driver->get_at_home());
    }
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a slew refused inside its guide drain leaves the running slew's stage alone "
    "(#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1);  // a flip slew's rebase stage, a slew's arrival stage
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::Latch latch;  // before the driver: it outlives the initiator thread blocked on it
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // Slew A: a flip slew homing (a BeyondPole target), or a slew whose GOTO is in flight (a Normal target).
    mount.set_auto_complete_motions(false);
    const double ra_a = leg == 0 ? std::fmod(driver->get_sidereal_time() + 1.0, 24.0)
                                 : std::fmod(driver->get_sidereal_time() - 1.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(ra_a, 25.0);
    REQUIRE(clock.wait_for_waiters(1, 2s));
    const int gotos = mount.count_command(0x10, 0x02);

    // Slew B opens its motion transition and blocks in the guide drain, before any claim.
    alpacacore::vendor::celestron::set_drain_hook_for_testing(*driver, [&] { latch.block(); });
    auto second = std::async(std::launch::async, [&] {
        return call_result(
            [&] { driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 2.0, 24.0), 30.0); });
    });
    REQUIRE(latch.wait_entered());
    alpacacore::vendor::celestron::set_drain_hook_for_testing(*driver, {});

    // A's poll sees its home or GOTO done and runs its next stage meanwhile: the rebase and GOTO, or the arrival
    // check and the tracking restore.
    if (leg == 0) {
        mount.complete_axis_home(0x10);
        mount.complete_axis_home(0x11);
    } else {
        mount.complete_axis_slew(0x10);
        mount.complete_axis_slew(0x11);
    }
    const bool staged = wait_until([&] {
        clock.advance(20ms);
        return leg == 0 ? mount.count_command(0x10, 0x02) > gotos : driver->get_tracking();
    });

    // B's position read then fails, so B is refused without claiming anything.
    mount.ack_next_with_payload(0x10, 0x01, {0x00});
    latch.release();
    const auto refused = second.get();

    INFO("slew B: " << refused.text);
    CHECK(staged);
    CHECK(refused.code == alpacacore::AlpacaError::DriverException);
    // A still owns the axes and lands: Slewing goes false and Tracking is restored.
    mount.set_auto_complete_motions(true);
    mount.complete_axis_slew(0x10);
    mount.complete_axis_slew(0x11);
    CHECK(wait_until([&] {
        clock.advance(20ms);
        return stop_done(*driver);
    }));
    CHECK(driver->get_tracking());
    CHECK(driver->get_target_right_ascension() == ra_a);
}

namespace {

// Starts the leg's motion command: a slew (0), Park (1) or FindHome (2).
void start_leg(alpacacore::TelescopeDriver& driver, int leg) {
    if (leg == 0) driver.slew_to_coordinates_async(std::fmod(driver.get_sidereal_time() - 1.0 + 24.0, 24.0), 25.0);
    if (leg == 1) driver.park();
    if (leg == 2) driver.find_home();
}

// True when every request after the first `from` is an MC_AUX_GUIDE_ACTIVE read: nothing that moves the mount.
bool only_guide_reads_since(const alpacacore::test::FakeCelestronCgxMount& mount, std::size_t from) {
    const auto frames = mount.requests();
    return std::all_of(frames.begin() + static_cast<std::ptrdiff_t>(std::min(from, frames.size())), frames.end(),
                       [](const auto& frame) { return frame.command == 0x27; });
}

}  // namespace

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a slew, Park or FindHome supersedes a running pulse without a join and "
    "waits for the mount to end its packet (#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a slew, a Park, a FindHome
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    // Before the driver: they outlive the pulse body blocked on the latch.
    alpacacore::test::Latch latch;
    std::atomic<bool> body_resumed{false};
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    // A Dec pulse chain whose first packet the mount holds active; its body blocks at its first guide poll, so a
    // join of it could not return.
    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);
    alpacacore::vendor::celestron::set_pulse_poll_hook_for_testing(*driver, [&] {
        latch.block();
        body_resumed.store(true);
    });
    driver->pulse_guide(0, 600000);
    REQUIRE(latch.wait_entered());
    alpacacore::vendor::celestron::set_pulse_poll_hook_for_testing(*driver, {});
    const auto frames_before = mount.requests().size();
    const int dec_packets = mount.count_command(0x11, 0x26);
    const int position_reads = mount.count_command(0x10, 0x01) + mount.count_command(0x11, 0x01);

    auto second = std::async(std::launch::async, [&] { return call_result([&] { start_leg(*driver, leg); }); });
    // The command reads the mount's guide report, which still says active, and sleeps on the clock between reads.
    const bool waiting = clock.wait_for_waiters(1, 2s);
    const auto returned_while_active = second.wait_for(0ms);
    const bool nothing_moved_while_active = only_guide_reads_since(mount, frames_before);

    // The mount ends the packet; the command sends its own commands at its next read.
    mount.complete_axis_guide(0x11);
    mount.set_auto_complete_motions(true);
    const bool returned = wait_until([&] {
        clock.advance(20ms);
        return second.wait_for(0ms) == std::future_status::ready;
    });
    const bool body_still_held = !body_resumed.load();
    latch.release();
    const auto result = second.get();
    // The superseded body resumes, finds itself superseded and ends without sending its axis anything.
    const bool pulse_ended = wait_until([&] { return !driver->get_is_pulse_guiding(); });
    const bool landed = wait_until([&] {
        clock.advance(20ms);
        return stop_done(*driver);
    });

    INFO("second command: " << result.text);
    CHECK(result.text.empty());
    CHECK(waiting);
    CHECK((returned_while_active == std::future_status::timeout));
    CHECK(nothing_moved_while_active);
    CHECK(returned);
    CHECK(body_still_held);
    CHECK(pulse_ended);
    CHECK(landed);
    CHECK(mount.count_command(0x11, 0x26) == dec_packets);
    CHECK(mount.count_command(0x10, 0x26) == 0);
    if (leg == 0) {
        CHECK(mount.count_command(0x10, 0x02) > 0);
        CHECK(mount.count_command(0x11, 0x02) > 0);
        CHECK(driver->get_tracking());
    } else {
        // Park and FindHome read no position, so every read is the pulse's end-of-pulse read: there is none.
        CHECK(mount.count_command(0x10, 0x01) + mount.count_command(0x11, 0x01) == position_reads);
        CHECK(driver->get_at_home());
        CHECK(driver->get_at_park() == (leg == 1));
    }
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a slew, Park or FindHome fails when the mount never ends the superseded "
    "pulse's packet (#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a slew, a Park, a FindHome
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);  // the Dec packet stays active until the case ends it
    driver->pulse_guide(0, 2000);
    REQUIRE(clock.wait_for_wait_count(1, 2s));  // the pulse body, between its guide reads
    const auto frames_before = mount.requests().size();
    const auto started = clock.now();

    auto second = std::async(std::launch::async, [&] { return call_result([&] { start_leg(*driver, leg); }); });
    REQUIRE(clock.wait_for_wait_count(2, 2s));  // the command, between its reads of the mount's guide report
    bool guiding_while_waiting = true;
    bool returned_early = false;
    while (clock.now() - started < 3000ms) {
        clock.advance(20ms);
        static_cast<void>(clock.wait_for_woken_settled(100ms));
        guiding_while_waiting = guiding_while_waiting && driver->get_is_pulse_guiding();
        returned_early = returned_early || second.wait_for(0ms) == std::future_status::ready;
    }
    const bool returned = wait_until([&] {
        clock.advance(20ms);
        static_cast<void>(clock.wait_for_woken_settled(100ms));
        return second.wait_for(0ms) == std::future_status::ready;
    });
    const auto waited = clock.now() - started;
    const bool nothing_moved = only_guide_reads_since(mount, frames_before);
    mount.complete_axis_guide(0x11);
    const auto result = returned ? second.get() : CallResult{};
    if (!returned) {
        static_cast<void>(wait_until([&] {
            clock.advance(20ms);
            return second.wait_for(0ms) == std::future_status::ready;
        }));
    }

    INFO("second command: " << result.text);
    CHECK(guiding_while_waiting);
    CHECK_FALSE(returned_early);
    REQUIRE(returned);
    CHECK(waited >= 3050ms);
    CHECK(waited <= 3100ms);
    CHECK(result.code == alpacacore::AlpacaError::DriverException);
    CHECK(result.text == "Celestron CGX still reports a Dec guide packet active after 3050 ms; nothing was sent");
    CHECK(nothing_moved);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    CHECK(driver->get_tracking());
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - IsPulseGuiding stays true from the supersede of a pulse to the wait on the "
    "mount (#2263)",
    "[celestron][cgx][telescope][motion-owner]") {
    using namespace std::chrono_literals;
    const int leg = GENERATE(0, 1, 2);  // a slew, a Park, a FindHome
    INFO("leg=" << leg);
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    connect_home_and_track(mount, *driver);

    mount.set_auto_complete_motions(false);
    mount.set_hold_guide_until_completed(true);  // the Dec packet stays active until the case ends it
    driver->pulse_guide(0, 2000);
    REQUIRE(clock.wait_for_wait_count(1, 2s));  // the pulse body, between its guide reads

    // The command stops in its handoff, after the supersede woke the pulse body and before its wait on the mount.
    // The body returns on Superseded meanwhile, so its slot ends while the packet is still active.
    std::atomic<bool> guiding_in_handoff{true};
    std::atomic<bool> hook_ran{false};
    alpacacore::vendor::celestron::set_drain_hook_for_testing(*driver, [&] {
        const auto until = std::chrono::steady_clock::now() + 300ms;
        while (std::chrono::steady_clock::now() < until) {
            if (!driver->get_is_pulse_guiding()) guiding_in_handoff = false;
            std::this_thread::yield();
        }
        hook_ran = true;
    });
    auto second = std::async(std::launch::async, [&] { return call_result([&] { start_leg(*driver, leg); }); });
    REQUIRE(wait_until([&] { return hook_ran.load(); }));
    alpacacore::vendor::celestron::set_drain_hook_for_testing(*driver, {});
    mount.complete_axis_guide(0x11);
    REQUIRE(wait_until([&] {
        clock.advance(20ms);
        static_cast<void>(clock.wait_for_woken_settled(100ms));
        return second.wait_for(0ms) == std::future_status::ready;
    }));
    INFO("second command: " << second.get().text);
    CHECK(guiding_in_handoff);
}

namespace {

// Steps the fake clock by `step` each time a task waits on it (the state poll is held, so only the motion bodies do)
// until `done` holds, for at most 20 s of real time. A 1 s step runs the home loop's 200 ms poll once per step; a
// stop's 20 ms verification polls need 20 ms steps to stay inside their budget.
bool step_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& done,
                std::chrono::milliseconds step = std::chrono::seconds(1)) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!clock.wait_for_waiters(1, 1ms)) continue;
        clock.advance(step);
        if (!clock.wait_for_woken_settled(2s)) return false;
    }
    return true;
}

// The stored motion failure Slewing raises, or empty while it reads a value.
std::string slewing_failure(const alpacacore::TelescopeDriver& driver) {
    try {
        static_cast<void>(driver.get_slewing());
        return {};
    } catch (const alpacacore::AlpacaException& error) {
        return error.what();
    }
}

bool motion_ended(const alpacacore::TelescopeDriver& driver) {
    return !slewing_failure(driver).empty() || !driver.get_slewing();
}

// The axis got exactly one stop between its last two MC_LEVEL_STARTs.
bool one_stop_before_last_home_start(const std::vector<int>& stops_before) {
    const auto n = stops_before.size();
    return n >= 2 && stops_before[n - 1] == stops_before[n - 2] + 1;
}

}  // namespace

TEST_CASE("Celestron CGX USB Telescope Driver - FindHome stops and re-homes once an axis whose search misses the bound",
          "[celestron][cgx][telescope][home-search]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    mount.set_crawling_home_searches(0x10, 1);
    std::atomic<int> rehome_infos{0};
    struct SinkRestore {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkRestore() { alpacacore::logging::set_log_sink(previous); }
    } restore;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Info && message.find("re-homing RA") != std::string_view::npos)
                rehome_infos.fetch_add(1);
        });
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);

    const auto start = clock.now();
    driver->find_home();
    REQUIRE(step_until(clock, [&] { return mount.count_command(0x10, 0x0b) == 2 || motion_ended(*driver); }));
    INFO("Slewing failure: " << slewing_failure(*driver));
    REQUIRE(mount.count_command(0x10, 0x0b) == 2);
    const auto rehomed_after = clock.now() - start;
    CHECK(rehomed_after >= 60s);
    CHECK(rehomed_after <= 61s);
    REQUIRE(step_until(clock, [&] { return motion_ended(*driver); }));
    CHECK(slewing_failure(*driver).empty());
    CHECK(driver->get_at_home());
    CHECK(mount.stops_before_home_starts(0x10) == std::vector<int>{0, 1});
    // The Dec search homed inside the bound: it gets no stop and no second home.
    CHECK(mount.count_command(0x11, 0x0b) == 1);
    CHECK(mount.count_command(0x11, 0x24) == 0);
    CHECK(rehome_infos.load() == 1);
}

TEST_CASE("Celestron CGX USB Telescope Driver - a home that ends inside the bound sends nothing more",
          "[celestron][cgx][telescope][home-search]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);

    const auto start = clock.now();
    driver->find_home();
    REQUIRE(step_until(clock, [&] { return clock.now() - start >= 59s; }));
    mount.complete_axis_home(0x10);
    mount.complete_axis_home(0x11);
    REQUIRE(step_until(clock, [&] { return motion_ended(*driver); }));
    CHECK(slewing_failure(*driver).empty());
    CHECK(driver->get_at_home());
    for (const auto axis : {0x10, 0x11}) {
        CHECK(mount.count_command(axis, 0x0b) == 1);
        CHECK(mount.count_command(axis, 0x24) == 0);
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - a home whose second search also misses the bound fails after two sends",
          "[celestron][cgx][telescope][home-search]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    mount.set_crawling_home_searches(0x10, 2);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);

    const auto start = clock.now();
    driver->find_home();
    REQUIRE(step_until(clock, [&] { return !slewing_failure(*driver).empty(); }));
    const auto failed_after = clock.now() - start;
    CHECK(slewing_failure(*driver).find("Celestron CGX home timed out") != std::string::npos);
    CHECK(failed_after >= 120s);
    CHECK(failed_after <= 121s);
    for (int second = 0; second < 200; ++second) clock.advance(1s);
    CHECK(mount.count_command(0x10, 0x0b) == 2);
    CHECK(one_stop_before_last_home_start(mount.stops_before_home_starts(0x10)));
    CHECK(mount.count_command(0x11, 0x0b) == 1);
    CHECK_FALSE(driver->get_at_home());
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE("Celestron CGX USB Telescope Driver - AbortSlew during a re-home sends no further home",
          "[celestron][cgx][telescope][home-search]") {
    using namespace std::chrono_literals;
    bool before_rehome = false;
    SECTION("while the stopped axis comes to rest") { before_rehome = true; }
    SECTION("during the second search") {}

    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    mount.set_crawling_home_searches(0x10, 2);
    // Each stop reads busy for five slew-done polls, so the body waits on the clock before its second home.
    mount.set_stop_completion_reads(5);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);

    const auto start = clock.now();
    driver->find_home();
    if (before_rehome) {
        REQUIRE(step_until(clock, [&] { return mount.count_command(0x10, 0x24) == 1 || motion_ended(*driver); }));
        REQUIRE(mount.count_command(0x10, 0x24) == 1);
        REQUIRE(mount.count_command(0x10, 0x0b) == 1);
    } else {
        REQUIRE(step_until(clock, [&] { return mount.count_command(0x10, 0x0b) == 2 || motion_ended(*driver); }));
        REQUIRE(mount.count_command(0x10, 0x0b) == 2);
    }
    REQUIRE(clock.now() - start < 120s);  // the bound's stop, not the 180 s deadline's
    const int home_sends = mount.count_command(0x10, 0x0b);
    // Async with the clock driven: an AbortSlew that met a body stopping the mount would wait for that stop.
    auto abort = std::async(std::launch::async, [&] { driver->abort_slew(); });
    REQUIRE(step_until(
        clock, [&] { return abort.wait_for(0ms) == std::future_status::ready && stop_done(*driver); }, 20ms));
    CHECK_NOTHROW(abort.get());
    for (int second = 0; second < 200; ++second) clock.advance(1s);
    CHECK(slewing_failure(*driver).empty());
    CHECK(mount.count_command(0x10, 0x0b) == home_sends);
    CHECK(mount.count_command(0x11, 0x0b) == 1);
    CHECK_FALSE(driver->get_at_home());
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - a newer FindHome or Park or a disconnect during a re-home ends its homes",
    "[celestron][cgx][telescope][home-search]") {
    using namespace std::chrono_literals;
    bool before_rehome = false;
    SECTION("while the stopped axis comes to rest") { before_rehome = true; }
    SECTION("during the second search") {}
    enum class Next { FindHome, Park, Disconnect };
    Next next = Next::FindHome;
    SECTION("a newer FindHome") {}
    SECTION("a newer Park") { next = Next::Park; }
    SECTION("a disconnect") { next = Next::Disconnect; }

    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    mount.set_crawling_home_searches(0x10, before_rehome ? 1 : 2);
    // Each stop reads busy for five slew-done polls, so the body waits on the clock before its second home.
    mount.set_stop_completion_reads(5);
    auto info = test_info();
    info.port_path = mount.slave_path();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);

    driver->find_home();
    if (before_rehome) {
        REQUIRE(step_until(clock, [&] { return mount.count_command(0x10, 0x24) == 1 || motion_ended(*driver); }));
        REQUIRE(mount.count_command(0x10, 0x24) == 1);
        REQUIRE(mount.count_command(0x10, 0x0b) == 1);
    } else {
        REQUIRE(step_until(clock, [&] { return mount.count_command(0x10, 0x0b) == 2 || motion_ended(*driver); }));
        REQUIRE(mount.count_command(0x10, 0x0b) == 2);
    }
    const int ra_homes = mount.count_command(0x10, 0x0b);
    const int ra_stops = mount.count_command(0x10, 0x24);
    // Async with the clock driven: the newer command may wait for the old body's motion stage.
    auto newer = std::async(std::launch::async, [&] {
        if (next == Next::FindHome) driver->find_home();
        if (next == Next::Park) driver->park();
        if (next == Next::Disconnect) driver->set_connected(false);
    });
    REQUIRE(step_until(clock, [&] { return newer.wait_for(0ms) == std::future_status::ready; }, 20ms));
    CHECK_NOTHROW(newer.get());

    if (next == Next::Disconnect) {
        for (int second = 0; second < 200; ++second) clock.advance(1s);
        CHECK(mount.count_command(0x10, 0x0b) == ra_homes);
        CHECK(mount.count_command(0x11, 0x0b) == 1);
        return;
    }
    REQUIRE(step_until(clock, [&] { return motion_ended(*driver); }));
    for (int second = 0; second < 200; ++second) clock.advance(1s);
    // The newer command sent its own home on each axis; the superseded body sent no stop and no home after it.
    CHECK(slewing_failure(*driver).empty());
    CHECK(mount.count_command(0x10, 0x0b) == ra_homes + 1);
    CHECK(mount.count_command(0x11, 0x0b) == 2);
    CHECK(mount.count_command(0x10, 0x24) == ra_stops);
    CHECK(driver->get_at_home());
    CHECK(driver->get_at_park() == (next == Next::Park));
}

TEST_CASE(
    "Celestron CGX USB Telescope Driver - the meridian home route re-homes once an axis whose search misses the bound",
    "[celestron][cgx][telescope][home-search]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    const auto now = [] { return std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)}; };
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::test::create_cgx_telescope_poll_held(0, info, clock, now);
    alpacacore::test::CgxClockCleanup cleanup{driver, clock};
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(step_until(clock, [&] { return motion_ended(*driver); }));
    REQUIRE(driver->get_at_home());
    driver->set_tracking(true);

    mount.set_crawling_home_searches(0x10, 1);
    const auto start = clock.now();
    driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 6.0, 24.0), 30.0);
    REQUIRE(mount.count_command(0x10, 0x0b) == 2);  // the pier-side change routes through home
    REQUIRE(step_until(clock, [&] { return mount.count_command(0x10, 0x0b) == 3 || motion_ended(*driver); }));
    INFO("Slewing failure: " << slewing_failure(*driver));
    REQUIRE(mount.count_command(0x10, 0x0b) == 3);
    const auto rehomed_after = clock.now() - start;
    CHECK(rehomed_after >= 60s);
    CHECK(rehomed_after <= 61s);
    REQUIRE(step_until(clock, [&] { return motion_ended(*driver); }));
    CHECK(slewing_failure(*driver).empty());
    CHECK(driver->get_tracking());
    CHECK(mount.count_command(0x10, 0x02) == 1);
    CHECK(one_stop_before_last_home_start(mount.stops_before_home_starts(0x10)));
    CHECK(mount.count_command(0x11, 0x0b) == 2);
}
#endif

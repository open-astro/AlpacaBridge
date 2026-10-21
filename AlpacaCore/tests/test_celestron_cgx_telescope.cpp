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
#include <numbers>
#include <string>
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
#include "fake_celestron_cgx_mount.h"
#include "fake_task_clock.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& error) {
        REQUIRE(error.error_code() == expected_code);
    }
}

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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, config, clock);
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
        initiate = [](auto& driver) { driver.sync_to_coordinates(6.0, 30.0); };
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock);
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
        mount.set_stop_completion_reads(0);  // Teardown has no clock-advancing caller.
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
        driver->set_connected(false);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock);
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
    const bool returned = wait_until([&] {
        clock.advance(20ms);
        return home.wait_for(0ms) == std::future_status::ready && stopping.wait_for(0ms) == std::future_status::ready &&
               disconnecting.wait_for(0ms) == std::future_status::ready;
    });
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, config);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_device_firmware() == "RA 7.17.25, Dec 7.17.281");
    const auto requests = mount.requests();
    REQUIRE(requests.size() == 4);
    for (const auto& request : requests) {
        CHECK((request.destination == 0x10 || request.destination == 0x11));
        CHECK((request.command == 0xfe || request.command == 0x05));
        CHECK(request.payload.empty());
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - modern identity rejects malformed and non-CGX motors",
          "[celestron][cgx][telescope]") {
    for (const auto axis : {0x10, 0x11}) {
        for (const auto& model :
             std::vector<std::vector<std::uint8_t>>{{}, {0x17}, {0x17, 0x89}, {0x14, 0x85}, {0x17, 0x88, 0}}) {
            alpacacore::test::FakeCelestronCgxMount mount;
            REQUIRE(mount.ok());
            mount.set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x88});
            mount.set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
            mount.set_motor_identity(axis, {7, 17, 0, 25}, model);
            auto config = test_info();
            config.port_path = mount.slave_path();
            auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, config);
            require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
            CHECK_FALSE(driver->get_connected());
            CHECK_FALSE(driver->get_device_firmware());
        }
    }
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
            auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, config);
            require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
            CHECK_FALSE(driver->get_connected());
        }
    }
}

TEST_CASE("Celestron CGX USB Telescope Driver - Defaults", "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, test_info());

    REQUIRE(driver != nullptr);
    CHECK(driver->get_device_type() == alpacacore::DeviceType::Telescope);
    CHECK(driver->get_device_number() == 0);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "Celestron CGX/CGX-L USB Telescope");
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
        driver->set_connected(true);
        if (unpark) {
            driver->park();
            REQUIRE(wait_until([&] { return driver->get_at_park() && !driver->get_slewing(); }));
        } else {
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        }
        mount.set_stop_completion_reads(10);
        mount.fragment_next_reply();
        auto initiator = std::async(std::launch::async, [&] {
            if (unpark)
                driver->unpark();
            else
                driver->abort_slew();
        });
        const bool verifying = mount.wait_for_reply_fragment(2s);
        const auto returned = initiator.wait_for(100ms);
        auto tracking = std::async(std::launch::async, [&] { return driver->get_tracking(); });
        const auto tracking_early = tracking.wait_for(100ms);
        // Tracking reads must wait through the complete ACK pair, rather than
        // see stale true or a normal-dispatch "unknown" failure. Inspect the
        // pending completion state during the scripted post-ACK deceleration.
        mount.release_reply_tail();
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
        CHECK(tracking_early == std::future_status::timeout);
        CHECK_FALSE(tracking.get());
        REQUIRE(verifying);
        CHECK(returned == std::future_status::ready);
        CHECK(slewing);
        CHECK(parked == unpark);
        CHECK(coordinates_refused);
        CHECK(read_elapsed < 100ms);
        CHECK(no_encoder_reads);
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        CHECK_FALSE(driver->get_at_park());
        driver->set_connected(false);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
// Issue #1457: a stop retry reconnects under the driver mutex_. Since #1456
// the reconnect's port claim waits out another device's probe (up to 20 s per
// attempt), so the retry must not wait there, or every getter that takes
// mutex_ blocks behind it.
TEST_CASE("Celestron CGX USB Telescope Driver - a stop retry does not wait for another device's probe",
          "[celestron][cgx][telescope][integration][issue1457]") {
    namespace util = alpacacore::util;
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    const std::string path = info.port_path;
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
    driver->set_connected(true);

    // The adapter goes away: the first stop finds the node gone, closes the
    // link and fails, which releases the port claim.
    mount.sever_link();
    CHECK_NOTHROW(driver->abort_slew());
    REQUIRE(wait_until(
        [&] {
            try {
                static_cast<void>(driver->get_slewing());
                return false;
            } catch (const alpacacore::AlpacaException& error) {
                return error.error_code() == alpacacore::AlpacaError::DriverException;
            }
        },
        std::chrono::seconds(10)));
    REQUIRE_FALSE(util::is_serial_port_in_use(path));

    // Another device's probe now holds the node for 3 s while a second stop
    // retries its reconnect.
    auto probe = util::SerialPortClaim::acquire(path, util::SerialClaimKind::Probe, "Other probe");
    REQUIRE(probe);
    auto release = std::async(std::launch::async, [&probe] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        probe.release();
    });
    CHECK_NOTHROW(driver->abort_slew());
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
    driver->set_connected(true);

    // The adapter goes away: the first tracking write fails, and its rollback
    // reconnect finds the node gone, closes the link and releases the claim.
    mount.sever_link();
    try {
        driver->set_tracking(false);
    } catch (const alpacacore::AlpacaException&) {
        // Expected: the write and its rollback both fail.
    }
    REQUIRE_FALSE(util::is_serial_port_in_use(path));

    // Another device's probe holds the node for 5 s while a second tracking
    // write fails and its rollback reconnects under mutex_.
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        if (unpark)
            driver->unpark();
        else
            driver->abort_slew();
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
        CHECK_FALSE(driver->get_at_park());
        CHECK_FALSE(driver->get_tracking());
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        REQUIRE(wait_until([&] {
            try {
                static_cast<void>(driver->get_slewing());
                return false;
            } catch (const alpacacore::AlpacaException& error) {
                return error.error_code() == alpacacore::AlpacaError::DriverException;
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home(); }));
    mount.set_ra_stop_drift_reads(8);
    driver->set_tracking(true);
    driver->set_tracking(false);
    CHECK_NOTHROW(driver->sync_to_coordinates(6.0, 30.0));
    CHECK_FALSE(driver->get_tracking());
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 0.001);
    CHECK(std::abs(driver->get_declination() - 30.0) < 0.0001);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - Sync refuses unsettled encoders before writing labels",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_auto_complete_motions(true);
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
    driver->set_connected(true);
    driver->find_home();
    REQUIRE(wait_until([&] { return driver->get_at_home(); }));
    const int ra_writes = mount.count_command(0x10, 0x04);
    const int dec_writes = mount.count_command(0x11, 0x04);
    mount.set_ra_stop_drift_reads(10000);
    driver->set_tracking(true);
    driver->set_tracking(false);
    require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 30.0); }, alpacacore::AlpacaError::DriverException);
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home(); }));
        const int ra_writes = mount.count_command(0x10, 0x04);
        const int dec_writes = mount.count_command(0x11, 0x04);
        mount.set_label_read_offsets(0x10, {4, 3, 1, 0, 0});
        mount.set_label_read_offsets(
            0x11, settles ? std::vector<std::int32_t>{-8, -7, -1, 1, 1, 0, 0} : std::vector<std::int32_t>(40, 8));
        if (settles) {
            CHECK_NOTHROW(driver->sync_to_coordinates(6.0, 30.0));
            CHECK(std::abs(driver->get_right_ascension() - 6.0) < 0.001);
            CHECK(std::abs(driver->get_declination() - 30.0) < 0.0001);
        } else {
            require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 30.0); },
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(3, test_info());
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_name() == "Celestron CGX/CGX-L USB Telescope");
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
            require_alpaca_error(
                [&] { static_cast<void>(alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info)); },
                alpacacore::AlpacaError::InvalidValue);
        };
    rejects([](auto& info) { info.site_latitude_degrees = 90.1; });
    rejects([](auto& info) { info.site_longitude_degrees = std::numeric_limits<double>::quiet_NaN(); });
    rejects([](auto& info) { info.site_elevation_meters = -301.0; });
    rejects([](auto& info) { info.aperture_diameter_meters = -0.1; });
    rejects([](auto& info) { info.focal_length_meters = std::numeric_limits<double>::infinity(); });
}

TEST_CASE("Celestron CGX USB Telescope Driver - Not connected errors", "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, test_info());
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, test_info());
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));
    require_alpaca_error([&] { static_cast<void>(driver->action("unknown", "")); },
                         alpacacore::AlpacaError::ActionNotImplemented);
}

TEST_CASE("Celestron CGX USB Telescope Driver - independent targets and exact errors",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(1, test_info());

    require_alpaca_error([&] { static_cast<void>(driver->get_target_right_ascension()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { static_cast<void>(driver->get_target_declination()); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&] { driver->set_target_right_ascension(7.5); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Celestron CGX USB Telescope Driver - error codes and input precedence",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, test_info());

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

TEST_CASE("Celestron CGX USB Telescope Driver - UTCDate readback and pointing follow HostClock policy",
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(
        0, info, alpacacore::util::default_task_clock(), now);
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
    // applying it on top of the corrected host time.
    epoch_seconds.fetch_add(10);
    CHECK(driver->get_utc_date() == now());

    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    const auto synchronized_host_utc = now();
    const auto disagreeing_client_utc = synchronized_host_utc + std::chrono::hours(2);
    driver->set_utc_date(disagreeing_client_utc);
    CHECK(driver->get_utc_date() == disagreeing_client_utc);
    const auto host_lst = alpacacore::vendor::celestron::cgx_local_sidereal_time_hours(
        synchronized_host_utc, info.site_longitude_degrees.value());
    CHECK(std::abs(std::remainder(driver->get_sidereal_time() - host_lst, 24.0)) < 1e-9);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - unsupported methods use ASCOM errors",
          "[celestron][cgx][telescope][unit]") {
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, test_info());
    CHECK_FALSE(driver->get_can_slew());
    CHECK_FALSE(driver->get_can_slew_alt_az_async());
    CHECK_FALSE(driver->get_can_sync_alt_az());
    CHECK_FALSE(driver->get_can_set_park());
    require_alpaca_error([&] { driver->slew_to_coordinates(2.0, 15.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->slew_to_alt_az_async(90.0, 45.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->sync_to_alt_az(90.0, 45.0); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->set_park(); }, alpacacore::AlpacaError::MethodNotImplemented);
}

#ifndef _WIN32
TEST_CASE("Celestron CGX USB Telescope Driver - connects and homes both motor controllers",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    info.site_elevation_meters = 0.0;
    info.aperture_diameter_meters = 0.2;
    info.focal_length_meters = 1.0;
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
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
    require_alpaca_error([&] { driver->pulse_guide(1, 100); }, alpacacore::AlpacaError::InvalidOperation);
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
    driver->sync_to_coordinates(6.0, 30.0);
    CHECK(driver->get_tracking());
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 0.001);
    CHECK(std::abs(driver->get_declination() - 30.0) < 2.2e-5);
    CHECK(driver->get_target_right_ascension() == 6.0);
    CHECK(driver->get_target_declination() == 30.0);

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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);

    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(mount.count_command(0x10, 0x0b) == 0);
    CHECK(mount.count_command(0x11, 0x0b) == 0);
}

TEST_CASE("Celestron CGX USB Telescope Driver - pins guide polarity in both hemispheres and pointing branches",
          "[celestron][cgx][telescope][integration]") {
    for (const double latitude : {45.0, -45.0}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        mount.set_auto_complete_motions(true);
        auto info = test_info();
        info.port_path = mount.slave_path();
        info.site_latitude_degrees = latitude;
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        driver->set_tracking(true);

        for (const bool beyond_pole : {false, true}) {
            mount.set_encoder_position(0x11, beyond_pole ? 0x400000 : 0xc00000);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    require_alpaca_error([&] { driver->pulse_guide(2, 0); }, alpacacore::AlpacaError::InvalidOperation);
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
    auto first = alpacacore::vendor::celestron::create_celestron_cgx_telescope(
        0, first_info, alpacacore::util::default_task_clock(), now);
    auto second = alpacacore::vendor::celestron::create_celestron_cgx_telescope(
        1, second_info, alpacacore::util::default_task_clock(), now);
    first->set_connected(true);
    second->set_connected(true);
    first->find_home();
    second->find_home();
    REQUIRE(wait_until([&] { return first->get_at_home() && !first->get_slewing(); }));
    REQUIRE(wait_until([&] { return second->get_at_home() && !second->get_slewing(); }));
    first->sync_to_coordinates(6.0, 30.0);
    second->sync_to_coordinates(12.0, -30.0);
    CHECK(std::abs(first->get_declination() - 30.0) < 2.2e-5);
    CHECK(std::abs(second->get_declination() + 30.0) < 2.2e-5);
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
    CHECK(std::abs(second->get_declination() + 30.0) < 2.2e-5);

    first->set_connected(true);
    CHECK_FALSE(first->get_at_home());
    CHECK_FALSE(first->get_tracking());
    CHECK(std::abs(std::remainder(first->get_right_ascension() - 6.0, 24.0)) < 0.001);
    CHECK(std::abs(first->get_declination() - 30.0) < 2.2e-5);
    CHECK_NOTHROW(first->set_tracking(true));
    second->set_connected(false);
    CHECK(first->get_tracking());
    CHECK(std::abs(first->get_declination() - 30.0) < 2.2e-5);
    second->set_connected(true);
    CHECK_FALSE(second->get_tracking());
    CHECK(std::abs(std::remainder(second->get_right_ascension() - 12.0, 24.0)) < 0.001);
    CHECK(std::abs(second->get_declination() + 30.0) < 2.2e-5);
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
    CHECK(std::abs(second->get_declination() + 30.0) < 2.2e-5);
    check_no_reference_writes(second_mount);
    first->set_connected(false);
    second->set_connected(false);
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
            auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
            driver->set_connected(true);
            driver->find_home();
            REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
            driver->sync_to_coordinates(6.0, 30.0);
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
                CHECK(std::abs(driver->get_declination() - 30.0) < 0.0001);
                CHECK_NOTHROW(driver->set_tracking(true));
            } else {
                require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                                     alpacacore::AlpacaError::DriverException);
                require_alpaca_error([&] { static_cast<void>(driver->get_declination()); },
                                     alpacacore::AlpacaError::DriverException);
                require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::InvalidOperation);
                require_alpaca_error([&] { driver->sync_to_coordinates(6.0, 30.0); },
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
        driver->set_connected(true);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        if (checksum_failure)
            mount.corrupt_next_reply_checksum();
        else
            mount.drop_next_reply(0x10, 0x01, false);
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
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
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
    auto first = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
    auto second = alpacacore::vendor::celestron::create_celestron_cgx_telescope(1, info);
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
    auto failed = alpacacore::vendor::celestron::create_celestron_cgx_telescope(2, info);
    require_alpaca_error([&] { failed->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(failed->get_connected());
    CHECK_FALSE(failed->get_device_firmware());
    check_exclusive(0);
    mount.set_motor_identity(0x10, {7, 18}, {0x17, 0x88});
    auto replacement = alpacacore::vendor::celestron::create_celestron_cgx_telescope(3, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
            auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
            require_alpaca_error([&] { static_cast<void>(driver->get_right_ascension()); },
                                 alpacacore::AlpacaError::DriverException);
            const std::vector<std::function<void()>> hardware_reads{[&] { static_cast<void>(driver->get_at_home()); },
                                                                    [&] { static_cast<void>(driver->get_at_park()); },
                                                                    [&] { static_cast<void>(driver->get_tracking()); },
                                                                    [&] { static_cast<void>(driver->get_tracking_rate()); },
                                                                    [&] { static_cast<void>(driver->get_slewing()); },
                                                                    [&] { static_cast<void>(driver->get_is_pulse_guiding()); }};
            const auto requests = mount.requests().size();
            for (const auto& read : hardware_reads)
                require_alpaca_error(read, alpacacore::AlpacaError::DriverException);
            CHECK(mount.requests().size() == requests);
            CHECK(driver->get_connected());
            CHECK(driver->get_can_find_home());
            CHECK(driver->get_site_latitude() == 45.0);
            CHECK(driver->get_guide_rate().dec > 0.0);
            if (fault == 2) continue;
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
        auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(
            0, info, alpacacore::util::default_task_clock(), [&] {
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
        // held by UTCDate. IsPulseGuiding bypasses that mutex and faults AUX.
        mount.drop_next_reply(0x10, 0x27, false);
        mount.release_reply_tail();
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock);
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
    driver->set_connected(false);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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

TEST_CASE("Celestron CGX USB Telescope Driver - Unpark cancels a pending Park completion",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    require_alpaca_error([&] { driver->pulse_guide(0, 600000); }, alpacacore::AlpacaError::DriverException);
    REQUIRE(wait_until([&] { return mount.count_command(0x11, 0x27) > 0; }));
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(
        0, info, alpacacore::util::default_task_clock(), now);
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
    driver->sync_to_coordinates(6.0, 30.0);
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
        auto driver = create_celestron_cgx_telescope(0, info, alpacacore::util::default_task_clock(), now);
        driver->set_connected(true);
        // The reference uses the site at FindHome, not the constructor's site.
        driver->set_site_latitude(home_latitude);
        driver->find_home();
        REQUIRE(wait_until([&] { return driver->get_at_home() && !driver->get_slewing(); }));
        CHECK(std::abs(driver->get_declination() - (home_latitude > 0.0 ? 90.0 : -90.0)) < 2.2e-5);
        driver->set_tracking(true);
        driver->sync_to_coordinates(6.0, 30.0);
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
        CHECK(driver->get_target_declination() == 30.0);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(
        0, info, alpacacore::util::default_task_clock(), now);
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
            auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock, now);
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
            driver->set_connected(false);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info, clock);
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
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB Telescope Driver - stops both axes when either GOTO acknowledgement is lost",
          "[celestron][cgx][telescope][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    auto info = test_info();
    info.port_path = mount.slave_path();
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
        CHECK(driver->get_slewing());
        CHECK(abort.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
        abort.get();

        // Three lost stop ACKs cost 3 * 500 ms, followed by the sole stop
        // owner's 500 ms busy-verification budget.
        // Bound that worker drain separately from the nonblocking initiator.
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }, std::chrono::seconds(5)));

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
    auto driver = alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
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
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}
#endif

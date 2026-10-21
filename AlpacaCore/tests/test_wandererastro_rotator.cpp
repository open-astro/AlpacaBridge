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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/unique_id.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>
#include <alpacacore/version.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_pty_write.h"
#include "rotator_sync_offset_test_dir.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// Plays a WandererRotator Mini with no DC power: it answers the "1500001"
// handshake and then stays silent for every move (MCU alive, motor dead).
class SilentMotorRotator {
public:
    SilentMotorRotator() : pty_("SilentMotorRotator") {
        th_ = std::thread([this] { run(); });
    }
    ~SilentMotorRotator() {
        stop_ = true;
        th_.join();
    }
    std::string slave_path() const { return pty_.slave_path(); }
    int keepalive_fd() const { return pty_.keepalive_fd(); }
    /// Write raw bytes to the driver (a test that must unpark a reader).
    void send(const std::string& bytes) {
        alpacacore::test::pty_write_bounded(pty_.master_fd(), bytes.data(), bytes.size(), stop_);
    }

private:
    void run() {
        std::string acc;
        while (!stop_) {
            pollfd p{pty_.master_fd(), POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) {
                continue;
            }
            char buf[64];
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            for (ssize_t i = 0; i < n; ++i) {
                acc += buf[i];
            }
            while (acc.size() >= 7) {
                const std::string cmd = acc.substr(0, 7);
                acc.erase(0, 7);
                if (!acc.empty() && acc[0] == '\n') {
                    acc.erase(0, 1);
                }
                if (cmd == "1500001") {
                    const std::string reply = "WandererRotatorMiniA20250101A0A0A0A";
                    alpacacore::test::pty_write_bounded(pty_.master_fd(), reply.data(), reply.size(), stop_);
                }  // any move command: no reply
            }
        }
    }
    alpacacore::test::PtyPair pty_;
    std::thread th_;
    std::atomic<bool> stop_{false};
};

}  // namespace

TEST_CASE("WandererAstro Rotator Driver - Defaults", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Rotator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "WandererAstro WandererRotator Mini");
    CHECK(driver->get_can_reverse() == true);
    // One full step of the 1142 steps/degree worm drive; static, no hardware.
    CHECK(driver->get_step_size() == 1.0 / alpacacore::vendor::wandererastro::kRotatorMiniStepsPerDegree);
}

TEST_CASE("WandererAstro Rotator Driver - Device metadata", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "WandererAstro WandererRotator Mini Rotator Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore WandererAstro Rotator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "WANDERERASTRO_ROTATOR_3");
    // No firmware known before a connection handshake.
    CHECK(!driver->get_device_firmware().has_value());
}

TEST_CASE("WandererAstro Rotator Driver - Not connected throws", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(1, "/dev/null");

    REQUIRE(driver->get_connected() == false);
    require_alpaca_error([&]() { driver->get_reverse(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_mechanical_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_target_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_target_position(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_absolute(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_mechanical(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->sync(10.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro Rotator Driver - Unsupported actions", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("WandererAstro Rotator Driver - Device Number Assignment", "[wandererastro][rotator][unit]") {
    auto driver0 = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);
    auto driver1 = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(1, 0);
    auto driver5 = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}

TEST_CASE("WandererAstro Rotator Driver - State machine", "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator_by_index(0, 0);

    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; there must be no non-compliant
    // "Connected" entry.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "IsMoving");  // omitted, not fabricated, when unknown
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("WandererAstro Rotator Protocol Wrapper - Value range validation", "[wandererastro][rotator][unit]") {
    alpacacore::vendor::wandererastro::WandererRotatorProtocolWrapper wrapper;

    // Backlash range is validated BEFORE the connection check so the [0, 3]
    // boundary is testable without hardware (equivalent when connected).
    require_alpaca_error([&]() { wrapper.set_backlash(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_backlash(3.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("WandererAstro Rotator Protocol Wrapper - Disconnected behavior", "[wandererastro][rotator][unit]") {
    alpacacore::vendor::wandererastro::WandererRotatorProtocolWrapper wrapper;

    REQUIRE(wrapper.is_connected() == false);
    CHECK(!wrapper.get_firmware_date().has_value());

    const auto state = wrapper.get_state();
    CHECK(state.valid == false);
    CHECK(state.moving == false);

    require_alpaca_error([&]() { wrapper.move_relative(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_backlash(1.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_zero(); }, alpacacore::AlpacaError::NotConnected);

    // Connecting to a missing port must fail with NotConnected and leave the
    // wrapper disconnected.
    alpacacore::vendor::wandererastro::RotatorConnectionConfig config;
    config.serial_port = "/nonexistent/wanderer-rotator-test-port";
    config.serial_timeout_s = 1;
    require_alpaca_error([&]() { wrapper.connect(config); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(wrapper.is_connected() == false);
}

// Falsified by: wandererastro_rotator_protocol_wrapper.cpp no-report branch
// setting mechanical_angle = move_start_angle_ + move_delta_ and clearing
// completion_missing (the old behaviour) makes Position read the target.
TEST_CASE("WandererAstro Rotator Driver - No completion report keeps Position and faults IsMoving",
          "[wandererastro][rotator][unit]") {
    SilentMotorRotator fake;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, fake.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const double start = driver->get_position();

    // One step: the monitor budget is the 5 s floor, the shortest the driver allows.
    driver->move_absolute(0.001);
    REQUIRE(driver->get_is_moving());
    // Poll rather than sleep: the monitor exits between 5 s and about 6 s
    // depending on read-window rounding, so wait up to 10 s for the fault.
    bool faulted = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            (void)driver->get_is_moving();
        } catch (const alpacacore::AlpacaException& e) {
            REQUIRE(e.error_code() == alpacacore::AlpacaError::DriverException);
            faulted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // No report: IsMoving names the fault instead of claiming an arrival.
    REQUIRE(faulted);
    require_alpaca_error([&]() { (void)driver->get_is_moving(); }, alpacacore::AlpacaError::DriverException);
    // Position stays at the last confirmed angle (the move start).
    CHECK(std::abs(driver->get_position() - start) < 1e-9);

    // Halt acknowledges the fault.
    driver->halt();
    CHECK_FALSE(driver->get_is_moving());
    driver->set_connected(false);
}

TEST_CASE("WandererAstro Rotator Driver - Sync offset persists", "[wandererastro][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());

    driver->set_connected(true);
    REQUIRE(driver->get_position() == 0.0);
    driver->sync(30.0);
    CHECK(driver->get_position() == 30.0);

    // Disconnect + reconnect keeps the offset.
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(driver->get_position() == 30.0);
    driver->set_connected(false);

    // A new driver instance (service restart) reads the same state file.
    auto restarted = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    restarted->set_connected(true);
    CHECK(restarted->get_position() == 30.0);
    CHECK(restarted->get_mechanical_position() == 0.0);
    restarted->set_connected(false);
}

// A client may connect before the router's upgrade-start adopt runs; the adopt
// then refreshes Position, not only the file (review of #1493).
TEST_CASE("WandererAstro Rotator Driver - adopt_legacy_state while connected refreshes Position",
          "[wandererastro][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::save("WANDERERASTRO_ROTATOR_0", 30.0));
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    driver->assign_unique_id(alpacacore::util::generate_unique_id());
    driver->set_connected(true);
    REQUIRE(driver->get_position() == 0.0);
    driver->adopt_legacy_state();
    CHECK(driver->get_position() == 30.0);
    driver->set_connected(false);
}

// Two Syncs at once must leave the stored offset and Position agreeing: one
// Sync's save and in-memory write may not straddle the other's, or a restart
// moves Position to the offset the file kept (review of #1493). The first Sync
// is held between its save and its in-memory write while the second runs.
TEST_CASE("WandererAstro Rotator Driver - Concurrent Syncs leave the file and Position agreeing",
          "[wandererastro][rotator][sync-offset][unit]") {
    using alpacacore::util::RotatorSyncOffsetStore;
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    driver->set_connected(true);
    std::atomic<bool> first_saved{false};
    std::atomic<bool> release{false};
    std::atomic<int> saves{0};
    RotatorSyncOffsetStore::set_after_save_hook_for_testing([&] {
        if (saves.fetch_add(1) == 0) {
            first_saved.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
    std::thread first([&] { driver->sync(10.0); });
    while (!first_saved.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::thread second([&] { driver->sync(20.0); });
    // Without serialisation the second Sync saves and publishes in this window.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    release.store(true);
    first.join();
    second.join();
    RotatorSyncOffsetStore::set_after_save_hook_for_testing({});
    CHECK(RotatorSyncOffsetStore::load(driver->get_unique_id()) == driver->get_position());
    driver->set_connected(false);
}

// Decision 0008 (#1473): IRotatorV4 requires the offset to persist, so a Sync
// that cannot store it fails and the previous offset stays.
TEST_CASE("WandererAstro Rotator Driver - Sync that cannot save throws and keeps the offset",
          "[wandererastro][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    driver->set_connected(true);
    driver->sync(30.0);
    REQUIRE(driver->get_position() == 30.0);

    // A regular file where the store's directory should be: every save fails.
    const auto blocker = dir.file().parent_path().parent_path() / "blocker";
    { std::ofstream(blocker) << "x"; }
    alpacacore::util::RotatorSyncOffsetStore::set_default_path((blocker / "offsets.json").string());
    require_alpaca_error([&] { driver->sync(50.0); }, alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_position() == 30.0);
    driver->set_connected(false);
}

// Decision 0018 (#1471): keyed by the assigned GUID. The offset saved under the
// device-number ID moves only when the router asks, at the start that gives the
// stored entry its first GUID; a connect never moves it, so a rotator added later
// on the same device number cannot inherit it.
TEST_CASE("WandererAstro Rotator Driver - Connect does not adopt the offset saved under the old ID",
          "[wandererastro][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::save("WANDERERASTRO_ROTATOR_0", 30.0));
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    const std::string guid = alpacacore::util::generate_unique_id();
    driver->assign_unique_id(guid);
    driver->set_connected(true);
    CHECK(driver->get_position() == 0.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == 0.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load("WANDERERASTRO_ROTATOR_0") == 30.0);
    driver->set_connected(false);
}

TEST_CASE("WandererAstro Rotator Driver - adopt_legacy_state moves the offset saved under the old ID",
          "[wandererastro][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::save("WANDERERASTRO_ROTATOR_0", 30.0));
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    const std::string guid = alpacacore::util::generate_unique_id();
    driver->assign_unique_id(guid);
    driver->adopt_legacy_state();
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == 30.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load("WANDERERASTRO_ROTATOR_0") == 0.0);
    driver->set_connected(true);
    CHECK(driver->get_position() == 30.0);
    driver->set_connected(false);
}

// #508 item 1: a saved serial entry with an empty portPath is registered so it stays listed; its
// connect refuses in words the operator can act on instead of opening "" or auto-detecting.
TEST_CASE("WandererAstro Rotator Driver - Connect with empty serial port names portPath",
          "[wandererastro][rotator][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, "");
    std::string message;
    try {
        driver->set_connected(true);
    } catch (const alpacacore::AlpacaException& e) {
        message = e.what();
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    CHECK(message.find("portPath") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
}

// ---------------------------------------------------------------------------
// Issue #1202: the rotator opens its port through util::SerialLink, so a port
// another device holds is refused untouched, its own claim is released on
// disconnect, and a retuned port cannot park the move monitor in read() and
// hang Disconnect (the WandererRotator hang reported in #1202).
// ---------------------------------------------------------------------------

#include <alpacacore/util/serial_port_registry.h>

#include "wandererastro_serial_ownership_helpers.h"

TEST_CASE("WandererAstro Rotator Driver - Refuses a port another device holds (issue #1202)",
          "[wandererastro][rotator][serial][issue1202]") {
    namespace util = alpacacore::util;
    SilentMotorRotator rotator;
    const speed_t before = alpacacore::test::wanderer::line_speed(rotator.keepalive_fd());
    auto claim = util::SerialPortClaim::acquire(rotator.slave_path(), util::SerialClaimKind::Link, "Other device");
    REQUIRE(claim);

    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    const std::string error = alpacacore::test::wanderer::connect_error([&] { driver->set_connected(true); });
    CHECK(error.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    // Refused before the open: the holder's line settings are untouched.
    CHECK(alpacacore::test::wanderer::line_speed(rotator.keepalive_fd()) == before);
}

TEST_CASE("WandererAstro Rotator Driver - Holds its port while connected and releases it (issue #1202)",
          "[wandererastro][rotator][serial][issue1202]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(alpacacore::util::is_serial_port_in_use(rotator.slave_path()));
    driver->set_connected(false);
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(rotator.slave_path()));
    // TIOCEXCL and the claim are both gone: the same port connects again.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE(
    "WandererAstro Rotator Protocol Wrapper - Disconnect returns after the port is retuned to VMIN=1 mid-move "
    "(issue #1202)",
    "[wandererastro][rotator][serial][issue1202]") {
    SilentMotorRotator rotator;
    alpacacore::vendor::wandererastro::WandererRotatorProtocolWrapper wrapper;
    alpacacore::vendor::wandererastro::RotatorConnectionConfig config;
    config.serial_port = rotator.slave_path();
    REQUIRE(wrapper.connect(config) == "WandererRotatorMini");

    // 30 degrees: the monitor's own budget is about 19 s, so only its stop
    // flag can end it inside the bound below. The fake never reports.
    wrapper.move_relative(30.0);
    REQUIRE(wrapper.get_state().moving);
    alpacacore::test::wanderer::retune_to_blocking_read(rotator.keepalive_fd());
    // Outlast one read window so the monitor's next read runs under VMIN=1.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // The wrapper's disconnect() is synchronous: it joins the monitor.
    CHECK(alpacacore::test::wanderer::returns_within([&] { wrapper.disconnect(); }, std::chrono::seconds(3),
                                                     [&] { rotator.send("A"); }));
    CHECK_FALSE(wrapper.is_connected());
}

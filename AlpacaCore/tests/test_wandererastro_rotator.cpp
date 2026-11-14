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
#include <alpacacore/util/logging.h>
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_pty_write.h"
#include "pty_responder.h"
#include "rotator_sync_offset_test_dir.h"

namespace {

using alpacacore::test::require_alpaca_error;

// Plays a WandererRotator Mini with no DC power: it answers the "1500001"
// handshake and then stays silent for every move (MCU alive, motor dead). With
// @p move_reply it answers a move with those bytes instead (firmware 20261025's
// "NP\n" and "LIMIT\n" lines, as INDI's wanderer_rotator_base.cpp reads them).
class SilentMotorRotator {
public:
    explicit SilentMotorRotator(std::string move_reply = {})
        : pty_("SilentMotorRotator"), move_reply_(std::move(move_reply)) {
        th_ = std::thread([this] { run(); });
    }
    ~SilentMotorRotator() { sever_link(); }
    std::string slave_path() const { return pty_.slave_path(); }
    int keepalive_fd() const { return pty_.keepalive_fd(); }
    /// Pull the cable (idempotent): stop the worker, then close both pty ends, so the driver's writes fail with
    /// EIO and its reads return 0 at once from here on. slave_path() is empty afterwards.
    void sever_link() {
        stop_ = true;
        if (th_.joinable()) {
            th_.join();
        }
        pty_.sever();
    }
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
                } else if (!move_reply_.empty()) {
                    alpacacore::test::pty_write_bounded(pty_.master_fd(), move_reply_.data(), move_reply_.size(),
                                                        stop_);
                }  // otherwise any move command: no reply
            }
        }
    }
    alpacacore::test::PtyPair pty_;
    std::string move_reply_;
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

// A LIMIT refusal is logged below WARNING because a client's move can cause it (decision 0007
// rule 9); NP is a hardware fault and stays a warning.
// Falsified by: monitor_move ignoring the NP/LIMIT check leaves the bytes in
// carry until the 5 s budget ends, and IsMoving reports the check-DC-power text.
TEST_CASE("WandererAstro Rotator Driver - NP and LIMIT replies end the move with a named fault",
          "[wandererastro][rotator][unit]") {
    const auto [reply, cause, warns] =
        GENERATE(table<std::string, std::string, bool>({{"NP\n", "motor power", true}, {"LIMIT\n", "limit", false}}));
    std::atomic<bool> warned{false};
    const auto previous_sink = alpacacore::logging::get_log_sink();
    alpacacore::logging::set_log_sink([&warned, cause = cause](auto level, auto, auto message) {
        if (level >= alpacacore::logging::LogLevel::Warn && message.find(cause) != std::string_view::npos) {
            warned = true;
        }
    });
    const std::shared_ptr<void> restore_sink(nullptr, [&](void*) { alpacacore::logging::set_log_sink(previous_sink); });
    SilentMotorRotator fake(reply);
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, fake.slave_path());
    driver->set_connected(true);
    const double start = driver->get_position();

    const auto began = std::chrono::steady_clock::now();
    driver->move_absolute(0.001);
    std::string fault;
    while (fault.empty() && std::chrono::steady_clock::now() - began < std::chrono::seconds(4)) {
        try {
            (void)driver->get_is_moving();
        } catch (const alpacacore::AlpacaException& e) {
            REQUIRE(e.error_code() == alpacacore::AlpacaError::DriverException);
            fault = e.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Inside 4 s, so before the 5 s no-report budget could have run out.
    INFO(fault);
    REQUIRE(fault.find(cause) != std::string::npos);
    CHECK(std::abs(driver->get_position() - start) < 1e-9);
    CHECK(warned.load() == warns);

    // The fault stays on the operation until it is acknowledged.
    require_alpaca_error([&]() { (void)driver->get_is_moving(); }, alpacacore::AlpacaError::DriverException);
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

// ASCOM Rotator: Position and MechanicalPosition are 0 <= x < 360. A target a
// hair below zero must not round up to exactly 360.
TEST_CASE("WandererAstro Rotator Driver - A tiny negative target stays below 360", "[wandererastro][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    driver->set_connected(true);

    driver->set_target_position(-1e-15);
    CHECK(driver->get_target_position() < 360.0);
    CHECK(driver->get_target_position() >= 0.0);
    driver->set_connected(false);
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

// Falsified by: wandererastro_rotator_protocol_wrapper.cpp read_handshake_section retries a LinkDead read
TEST_CASE("WandererAstro Rotator Protocol Wrapper - Connect fails at once when the port hangs up mid-handshake",
          "[wandererastro][rotator][serial][unit]") {
    // A rotator that never answers; the pty is severed 300 ms into connect().
    // Each handshake section waits serial_timeout_s (3 s) and the handshake
    // retries once, so a connect that returns well before 3 s saw the hangup.
    alpacacore::test::PtyPair pty("HangupRotator");
    std::thread severer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        pty.sever();
    });
    alpacacore::vendor::wandererastro::WandererRotatorProtocolWrapper wrapper;
    alpacacore::vendor::wandererastro::RotatorConnectionConfig config;
    config.serial_port = pty.slave_path();
    config.serial_timeout_s = 3;
    const auto started = std::chrono::steady_clock::now();
    require_alpaca_error([&]() { wrapper.connect(config); }, alpacacore::AlpacaError::NotConnected);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    severer.join();
    CHECK(elapsed < std::chrono::milliseconds(2500));
    CHECK_FALSE(wrapper.is_connected());
}

#ifndef _WIN32
// #2203: opening a CH340 resets the MCU, so the handshake written during its boot is lost and only the retry lands.
// Falsified by: probe_rotator_port() handshaking with a shorter window than connect()'s serial_timeout_s.
TEST_CASE("WandererRotator auto-detect finds a rotator that is silent through its boot after open (#2203)",
          "[wandererastro][rotator][autodetect]") {
    const auto opened = std::chrono::steady_clock::now();
    alpacacore::test::PtyResponder tty([&](const std::string& chunk) -> std::string {
        if (chunk.find("1500001") == std::string::npos) return "";
        if (std::chrono::steady_clock::now() - opened < std::chrono::seconds(2)) return "";  // still booting
        return "WandererRotatorMiniA20250101A0A0A0A";
    });
    const auto found = alpacacore::vendor::wandererastro::enumerate_wanderer_rotator_ports({tty.slave_path()});
    REQUIRE(found.size() == 1);
    CHECK(found.front().model == "WandererRotatorMini");
}

// #2203: the longer window must not turn a dead port into more traffic than the handshake and its one retry.
// Falsified by: do_handshake() sending a third "1500001" after the retry window.
TEST_CASE("WandererRotator auto-detect skips a port that never answers after only the handshake (#2203)",
          "[wandererastro][rotator][autodetect]") {
    std::mutex mutex;
    std::string received;
    alpacacore::test::PtyResponder tty([&](const std::string& chunk) -> std::string {
        std::lock_guard<std::mutex> lock(mutex);
        received += chunk;
        return "";
    });
    CHECK(alpacacore::vendor::wandererastro::enumerate_wanderer_rotator_ports({tty.slave_path()}).empty());
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(received == "15000011500001");
}
#endif

#ifndef _WIN32
// #2204: Falsified by: wandererastro_rotator_protocol_wrapper.cpp kRotatorModelPrefix naming only the Mini.
TEST_CASE("WandererRotator - another rotator version connects under a generic name (#2204)",
          "[wandererastro][rotator][unit]") {
    alpacacore::test::PtyResponder tty([](const std::string& chunk) -> std::string {
        return chunk.find("1500001") != std::string::npos ? "WandererRotatorProA20250101A0A0A0A" : "";
    });
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, tty.slave_path());
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_name() == "WandererAstro Rotator");
    driver->set_connected(false);
}

// #2204: the step scale is the Mini's (a Lite differs), so another version is added but never moved with it.
// Falsified by: wandererastro_rotator_driver.cpp require_known_model() not refusing an unlisted model.
TEST_CASE("WandererRotator - another rotator version refuses every move (#2204)", "[wandererastro][rotator][unit]") {
    std::mutex mutex;
    std::vector<std::string> received;
    alpacacore::test::PtyResponder tty([&](const std::string& chunk) -> std::string {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(chunk);
        return chunk.find("1500001") != std::string::npos ? "WandererRotatorLiteA20250101A0A0A0A" : "";
    });
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, tty.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    std::size_t before = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        before = received.size();
    }
    namespace AlpacaError = alpacacore::AlpacaError;
    require_alpaca_error([&] { driver->move(10.0); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->move_absolute(10.0); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->move_mechanical(10.0); }, AlpacaError::InvalidOperation);
    CHECK_NOTHROW(driver->halt());  // a stop needs no step scale
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = before; i < received.size(); ++i) {
            CHECK(received[i].find("Stop") != std::string::npos);
        }
    }
    driver->set_connected(false);
}

// #2204: Falsified by: wandererastro_rotator_protocol_wrapper.cpp connect() dropping the family prefix check.
TEST_CASE("WandererRotator - a handshake from another product is refused (#2204)", "[wandererastro][rotator][unit]") {
    alpacacore::test::PtyResponder tty([](const std::string& chunk) -> std::string {
        return chunk.find("1500001") != std::string::npos ? "OtherDeviceA20250101A0A0A0A" : "";
    });
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, tty.slave_path());
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
}
#endif

// ---------------------------------------------------------------------------
// Decision 0009 (issue #1989): the rotator is request/response and silent
// between moves, so it has no silence latch and no staleness bound; EIO, ENXIO
// or ENODEV on a command write or a move monitor read is a lost link at once.
// ---------------------------------------------------------------------------

#include <alpacacore/util/serial_port_registry.h>

namespace {

template <typename Pred>
bool wait_until_rotator(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// Every operational member answers NotConnected over a lost link; the static
// capabilities keep answering, and DeviceState is the empty list.
void require_rotator_lost(alpacacore::RotatorDriver& driver) {
    CHECK_FALSE(driver.get_connected());
    require_alpaca_error([&]() { (void)driver.get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver.get_mechanical_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver.get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver.get_reverse(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver.halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver.move_absolute(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver.sync(10.0); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver.get_can_reverse());
    CHECK(driver.get_device_state().empty());
}

}  // namespace

// Falsified by: AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp write_command_locked
// throwing DriverException "Serial write failed" for every errno (dropping the is_hangup_errno branch).
TEST_CASE("WandererAstro Rotator Driver - a write to a pulled adapter loses the link at once (#1989)",
          "[wandererastro][rotator][serial][unit][link-lost]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, rotator.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string port = rotator.slave_path();  // emptied by the cut

    // Real time: an idle rotator says nothing between moves, and that is not a fault or a loss.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());

    rotator.sever_link();
    // Idle: nothing reads the port, so the next command is the one that finds the pull.
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(port));  // no monitor runs: released at once
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);
    CHECK_FALSE(driver->get_device_firmware().has_value());
    require_rotator_lost(*driver);
    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

// Falsified by: AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp read_section not
// reporting the errno of a failing read (the old back-off), so monitor_move waits out its budget.
TEST_CASE("WandererAstro Rotator Driver - a cable pulled during a move loses the link at once (#1989)",
          "[wandererastro][rotator][serial][unit][link-lost]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    SilentMotorRotator replugged;
    alpacacore::test::ReplugPort path(rotator);
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, path.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string port = rotator.slave_path();

    // 90 degrees: the monitor's own budget is about 48 s and the fake never reports, so only the hangup can end it.
    driver->move_absolute(90.0);
    REQUIRE(driver->get_is_moving());
    rotator.sever_link();
    CHECK(wait_until_rotator([&] { return !driver->get_connected(); }, std::chrono::milliseconds(2000)));
    require_rotator_lost(*driver);  // its move_absolute: a Move right after the loss neither waits nor deadlocks
    CHECK(driver->get_link_fault().find("Serial link lost") != std::string::npos);
    CHECK(wait_until_rotator([&] { return !alpacacore::util::is_serial_port_in_use(port); },
                             std::chrono::milliseconds(2000)));

    // A loss is not the missing-report fault: after a reconnect IsMoving answers.
    path.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(driver->get_is_moving());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

// Falsified by: AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp disconnect not
// clearing loss_reason_, so the reason outlives the Disconnect.
TEST_CASE("WandererAstro Rotator Driver - the loss reason stays until the next Disconnect or Connect (#1989)",
          "[wandererastro][rotator][serial][unit][link-lost]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    SilentMotorRotator replugged;
    alpacacore::test::ReplugPort path(rotator);
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, path.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_link_fault().empty());

    rotator.sever_link();
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    const std::string reason = driver->get_link_fault();
    CHECK(reason.find("Serial link lost") != std::string::npos);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_link_fault() == reason);  // a later call does not replace or clear it

    driver->set_connected(false);
    CHECK(driver->get_link_fault().empty());

    path.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

// Falsified by: AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp set_connected keeping the
// idempotency check `connected == connected_.load()`, so a Connect over a lost link returns as already connected.
TEST_CASE("WandererAstro Rotator Driver - a Connect over a lost link reopens without a Disconnect (#1989)",
          "[wandererastro][rotator][serial][unit][link-lost]") {
    alpacacore::test::TempSyncOffsetDir dir;
    SilentMotorRotator rotator;
    SilentMotorRotator replugged;
    alpacacore::test::ReplugPort path(rotator);
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, path.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    rotator.sever_link();
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE_FALSE(driver->get_connected());

    // No Disconnect: a Connect while the raw flag is still set must not be taken as already connected.
    path.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_NOTHROW(driver->set_reverse(true));  // the replugged rotator takes commands
    CHECK(driver->get_link_fault().empty());

    // A Disconnect over a lost link completes and clears the reason.
    const std::string replugged_port = replugged.slave_path();
    replugged.sever_link();
    require_alpaca_error([&]() { driver->set_reverse(false); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_link_fault().empty());
    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(replugged_port));
}

// Falsified by: AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp read_handshake_section
// retrying a LinkDead read, so the Connect waits out its handshake windows.
TEST_CASE("WandererAstro Rotator Driver - a Connect that fails by hangup keeps no loss reason (#1989)",
          "[wandererastro][rotator][serial][unit][link-lost]") {
    // A rotator that never answers; the pty is severed 300 ms into the Connect.
    alpacacore::test::PtyPair pty("HangupRotator");
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_rotator(0, pty.slave_path());
    std::thread severer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // real time: the cut lands mid-handshake
        pty.sever();
    });
    const auto started = std::chrono::steady_clock::now();
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    severer.join();
    CHECK(elapsed < std::chrono::milliseconds(2500));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // a connect that never completed is not a lost session
}

// Falsified by: AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp raw_rotator_skip_reason
// returning std::nullopt for an unreadable descriptor, so the scan would probe the node.
TEST_CASE("WandererRotator auto-detect names why it skips a raw node (#2212)", "[wandererastro][rotator][autodetect]") {
    using alpacacore::util::UsbTtyDescriptor;
    using alpacacore::vendor::wandererastro::detail::raw_rotator_skip_reason;

    // No descriptor in sysfs: skipped, and the log says so.
    const auto unreadable = raw_rotator_skip_reason(std::nullopt);
    REQUIRE(unreadable.has_value());
    CHECK(*unreadable == "USB descriptor unreadable");

    // Another adapter class: skipped, naming its vendor and product.
    const auto ftdi = raw_rotator_skip_reason(UsbTtyDescriptor{"0403", "FTDI", "FT232R USB UART"});
    REQUIRE(ftdi.has_value());
    CHECK(*ftdi == "not a CH340/CH341 device (vendor 0403, product \"FT232R USB UART\")");

    // The rotator's CH340 (raw sysfs spelling, with its space): probed, so no reason.
    CHECK_FALSE(raw_rotator_skip_reason(UsbTtyDescriptor{"1a86", "", "USB Serial"}).has_value());
    CHECK_FALSE(raw_rotator_skip_reason(UsbTtyDescriptor{"0000", "", "USB2.0-Serial CH340"}).has_value());
}

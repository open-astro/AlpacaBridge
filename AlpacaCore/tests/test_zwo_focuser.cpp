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
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_zwo_eaf_sdk.h"
#include "getter_stall_probe.h"

namespace {

// Every WARN logged while it lives; restores the previous sink on any exit.
struct WarnCapture {
    std::vector<std::string> messages;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    WarnCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) messages.emplace_back(message);
            });
    }
    ~WarnCapture() { alpacacore::logging::set_log_sink(previous); }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
};

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

using FakeEAFSDK = alpacacore::test::FakeZWOEAFSDK;

void wait_past_status_ttl() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("ZWO EAF Focuser Driver - Defaults", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO EAF");
}

TEST_CASE("ZWO EAF Focuser Driver - Disconnected Behavior", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; the old non-compliant "Connected"
    // entry is gone.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    // open-astro#309: the connection check precedes the not-implemented
    // answer, so a disconnected write refuses on connection grounds.
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO EAF Focuser Driver - Device metadata", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO EAF Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO EAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "ZWO_EAF_3");
}

TEST_CASE("ZWO EAF Focuser Driver - Device Number Assignment", "[zwo][focuser][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);
    auto driver5 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
}

TEST_CASE("ZWO EAF Focuser Driver - Unique IDs", "[zwo][focuser][unit]") {
    auto driver_a = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);
    auto driver_b = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);

    REQUIRE(!driver_a->get_unique_id().empty());
    REQUIRE(!driver_b->get_unique_id().empty());
    CHECK(driver_a->get_unique_id() != driver_b->get_unique_id());
}

// open-astro#294: DeviceState used to round-trip the device once per property.
// Falsified by: zwo_focuser_driver.cpp read_status() calling the SDK on every
// read instead of through status_cache_.get() (position_calls 2, not 1).
TEST_CASE("ZWO EAF Focuser Driver - DeviceState is one device read, served from the TTL cache",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_device_state().size() == 4);  // IsMoving, Position, Temperature, TimeStamp
    CHECK(sdk.moving_calls == 1);
    CHECK(sdk.position_calls == 1);
    CHECK(sdk.temperature_calls == 1);

    (void)driver->get_device_state();
    CHECK(driver->get_position() == 500);
    CHECK(sdk.position_calls == 1);  // inside the TTL: no device I/O

    wait_past_status_ttl();
    (void)driver->get_device_state();
    CHECK(sdk.position_calls == 2);
}

// Falsified by: zwo_focuser_driver.cpp get_max_step() calling sdk_.get_max_step()
// instead of returning max_step_ (max_step_calls 3 after the reads, not 1).
TEST_CASE("ZWO EAF Focuser Driver - Static members are read at connect and reset on disconnect",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(sdk.max_step_calls == 1);
    CHECK(sdk.step_range_calls == 1);

    for (int i = 0; i < 3; ++i) {
        CHECK(driver->get_max_step() == 60000);
        CHECK(driver->get_max_increment() == 1000);
        CHECK(driver->get_name() == "EAF");
    }
    CHECK(sdk.max_step_calls == 1);
    CHECK(sdk.step_range_calls == 1);

    driver->set_connected(false);
    sdk.max_step = 30000;
    driver->set_connected(true);
    CHECK(driver->get_max_step() == 30000);
    CHECK(driver->get_max_increment() == 1000);
    CHECK(sdk.max_step_calls == 2);

    driver->move(100);  // range check uses the cached MaxStep
    require_alpaca_error([&]() { driver->move(30001); }, alpacacore::AlpacaError::InvalidValue);
}

// Falsified by: zwo_focuser_driver.cpp move() dropping its
// status_cache_.invalidate() (Position keeps the pre-move 500 for a TTL).
TEST_CASE("ZWO EAF Focuser Driver - A move invalidates the cached frame", "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 500);
    driver->move(900);
    CHECK(driver->get_position() == 900);
}

// Falsified by: ttl_status_cache.h get() dropping its health_.faulted() throw
// (the third failure reports the raw SDK error, not "communications compromised").
TEST_CASE("ZWO EAF Focuser Driver - A dead link refuses reads instead of serving the cache",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 500);

    sdk.fail_reads = true;
    wait_past_status_ttl();
    for (int i = 0; i < 2; ++i) {
        CHECK_THROWS(driver->get_position());
    }
    try {
        (void)driver->get_position();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_device_state().empty());

    sdk.fail_reads = false;
    CHECK(driver->get_position() == 500);
}

// Falsified by: zwo_focuser_driver.cpp halt() dropping its
// status_cache_.invalidate() (IsMoving keeps answering true for a TTL).
TEST_CASE("ZWO EAF Focuser Driver - Halt drops the cached frame", "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.moving = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_is_moving() == true);
    driver->halt();
    CHECK(driver->get_is_moving() == false);
}

// Falsified by: zwo_focuser_driver.cpp read_status() letting the temperature
// failure escape the refill (Position stops answering) or dropping the SDK text.
TEST_CASE("ZWO EAF Focuser Driver - A temperature failure leaves Position answering",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.fail_temperature = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 500);
    try {
        (void)driver->get_temperature();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("temp sensor down") != std::string::npos);
    }
}

TEST_CASE("ZWO EAF Focuser Driver - A temperature error keeps its mapped code", "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.temperature_not_supported = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 500);
    try {
        (void)driver->get_temperature();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotImplemented);
    }
}

// Configured max step (open-astro#1152). The unit clamps moves at its stored max step
// silently, so the driver writes the configured value to the unit at connect.
// Falsified by: zwo_focuser_driver.cpp set_connected() skipping sdk.set_max_step()
// (max_step_calls stays 1 but get_max_step() reads 60000, not 40000).
TEST_CASE("ZWO EAF Focuser Driver - A configured max step that differs is written once and used",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;  // stored 60000, step range 1000... raised below so 40000 is in range
    sdk.step_range = 100000;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk, 40000);
    driver->set_connected(true);

    CHECK(sdk.set_max_step_calls == 1);
    CHECK(sdk.last_set_max_step == 40000);
    CHECK(driver->get_max_step() == 40000);
    driver->move(40000);
    require_alpaca_error([&]() { driver->move(40001); }, alpacacore::AlpacaError::InvalidValue);

    // A reconnect finds the unit already holding the value: no second write.
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(sdk.set_max_step_calls == 1);
    CHECK(driver->get_max_step() == 40000);
}

// Falsified by: zwo_focuser_driver.cpp set_connected() writing the configured
// value without comparing it to the stored one (set_max_step_calls 1, not 0).
TEST_CASE("ZWO EAF Focuser Driver - A configured max step equal to the stored one is not written",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.step_range = 100000;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk, 60000);
    driver->set_connected(true);

    CHECK(sdk.set_max_step_calls == 0);
    CHECK(driver->get_max_step() == 60000);
}

// Falsified by: zwo_focuser_driver.cpp apply_configured_max_step() calling
// sdk.set_max_step(id, 64000) when no max step is configured (set_max_step_calls 1, not 0).
TEST_CASE("ZWO EAF Focuser Driver - Without a configured max step the stored value is left alone",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(sdk.set_max_step_calls == 0);
    CHECK(driver->get_max_step() == 60000);
}

// Falsified by: zwo_focuser_driver.cpp set_connected() throwing on a stored max step
// of 0 again (the connect is refused instead of succeeding with MaxStep 0). Also covers the
// async LastConnectError path of the removed "stored-zero refusal reaches LastConnectError" case.
TEST_CASE("ZWO EAF Focuser Driver - A stored max step of 0 with none configured still connects",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.max_step = 0;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->connect();
    for (int i = 0; i < 400 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(driver->get_connecting());

    CHECK(driver->get_connected());
    CHECK(driver->get_last_connect_error().empty());
    CHECK(driver->get_max_step() == 0);
    CHECK(sdk.set_max_step_calls == 0);
    require_alpaca_error([&] { driver->move(0 + 1); }, alpacacore::AlpacaError::InvalidValue);
}

// Falsified by: zwo_focuser_driver.cpp set_connected() skipping
// apply_configured_max_step() (MaxStep stays 0, set_max_step_calls 0).
TEST_CASE("ZWO EAF Focuser Driver - A configured max step repairs a stored max step of 0",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.max_step = 0;
    sdk.step_range = 100000;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk, 50000);
    driver->set_connected(true);

    CHECK(driver->get_connected());
    CHECK(sdk.set_max_step_calls == 1);
    CHECK(driver->get_max_step() == 50000);
}

// Falsified by: zwo_focuser_driver.cpp apply_configured_max_step() throwing
// (or writing the unclamped value) instead of clamping to step_range_.
TEST_CASE("ZWO EAF Focuser Driver - A configured max step above the unit's step range is clamped to it",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;  // step range 1000
    WarnCapture capture;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk, 1001);
    driver->set_connected(true);
    const auto& warnings = capture.messages;

    CHECK(driver->get_connected());
    CHECK(sdk.set_max_step_calls == 1);
    CHECK(sdk.last_set_max_step == 1000);
    CHECK(driver->get_max_step() == 1000);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("1001") != std::string::npos);
    CHECK(warnings[0].find("1000") != std::string::npos);
}

// Falsified by: zwo_focuser_driver.cpp apply_configured_max_step() clamping
// without the "step_range_ > 0" guard (an unavailable range clamps to 0, writing 0).
TEST_CASE("ZWO EAF Focuser Driver - An unavailable step range does not refuse a configured max step",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.step_range_fails = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk, 40000);
    driver->set_connected(true);

    CHECK(sdk.set_max_step_calls == 1);
    CHECK(driver->get_max_step() == 40000);
}

// Decision 0016: Name and UniqueID need no device access, so a connect that holds the SDK open must not park them.
// Falsified by: zwo_focuser_driver.cpp set_connected(true) holding mutex_ across open_focuser_link() (get_name() then
// waits the whole 300 ms open).
TEST_CASE("ZWO EAF Focuser Driver - Name and UniqueID answer while a connect holds the SDK open",
          "[zwo][focuser][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    FakeEAFSDK sdk;
    sdk.open_delay = kHoldOpen;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);

    driver->connect();
    REQUIRE(wait_for_flag(sdk.open_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(driver->get_connecting());  // the open is still in flight, so the timings above are not vacuous

    REQUIRE(wait_until_connected(*driver));
    CHECK(driver->get_name() == "EAF");
    CHECK(driver->get_unique_id() == "ZWO_EAF_SN_EAF-SN-1");
    driver->set_connected(false);
}

// Falsified by: zwo_focuser_driver.cpp get_max_step() or get_max_increment() taking io_mutex_ (the cached read then
// waits the 300 ms status refill).
TEST_CASE("ZWO EAF Focuser Driver - Identity and limits answer while a status read is in flight",
          "[zwo][focuser][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.read_delay = kHoldOpen;

    std::thread reader([&] {
        try {
            (void)driver->get_position();
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(sdk.read_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(time_call([&] { CHECK(driver->get_max_step() == 60000); }) < kGetterBudget);
    CHECK(time_call([&] { CHECK(driver->get_max_increment() == 1000); }) < kGetterBudget);
    CHECK(sdk.busy.load() > 0);  // the refill is still in flight

    reader.join();
    driver->set_connected(false);
}

// A disconnect must wait for the exchange in flight, never close the handle under it (the I/O lock).
// Falsified by: zwo_focuser_driver.cpp set_connected(false) calling close_focuser_link() without io_mutex_ (the move
// and halt sections; the status read section is also held back by status_cache_.reset()), or move() calling the SDK
// without it (closed_while_busy is set: the fake samples it before the table mutex, which would queue the close).
TEST_CASE("ZWO EAF Focuser Driver - A disconnect waits for the exchange in flight instead of closing under it",
          "[zwo][focuser][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    std::thread worker;
    SECTION("a status read") {
        sdk.read_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                (void)driver->get_position();
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.read_entered));
    }
    SECTION("a move") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->move(100);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }
    SECTION("a halt") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->halt();
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }

    CHECK(time_call([&] { driver->set_connected(false); }) < std::chrono::seconds(5));
    worker.join();
    CHECK_FALSE(sdk.closed_while_busy.load());
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.close_calls == 1);
}

// A disconnect that wins the I/O lock while status reads queue behind a move is not a link failure: every reader
// gets NotConnected, none the "communications compromised" fault that three counted failures would latch.
// Falsified by: zwo_focuser_driver.cpp read_status() throwing NotConnected inside the cache refill instead of
// returning the closed frame (the third queued reader then gets DriverException "communications compromised").
TEST_CASE("ZWO EAF Focuser Driver - A disconnect during queued status reads is not a link failure",
          "[zwo][focuser][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    sdk.move_delay = kHoldOpen;
    std::thread mover([&] {
        try {
            driver->move(100);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(sdk.move_entered));

    constexpr int kReaders = 8;
    std::atomic<int> not_connected{0};
    std::atomic<int> other{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            try {
                (void)driver->get_position();
                ++other;
            } catch (const alpacacore::AlpacaException& e) {
                (e.error_code() == alpacacore::AlpacaError::NotConnected ? not_connected : other)++;
            } catch (const std::exception&) {
                ++other;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // every reader is parked on the move
    driver->set_connected(false);
    mover.join();
    for (auto& reader : readers) {
        reader.join();
    }

    CHECK(other.load() == 0);
    CHECK(not_connected.load() == kReaders);
}

// Issue #1500: EAF_ERROR_REMOVED reaches the driver as NotConnected. Positive evidence the transport is gone
// (decision 0009 point 2a), so Connected reads false at once, not "communications compromised" after three polls.
// Falsified by: the status refill counting a NotConnected SDK error as one more failed poll.
TEST_CASE("ZWO EAF Focuser Driver - A focuser removed from USB reads Connected false",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 500);

    sdk.remove();
    wait_past_status_ttl();
    CHECK_THROWS(driver->get_position());
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
}

// Issue #1500: the replug gives the EAF a new SDK id; one Connect finds it again and the reads work.
// Falsified by: connect opening the id the driver was created with.
TEST_CASE("ZWO EAF Focuser Driver - A replugged focuser is reopened by one Connect",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.remove();
    wait_past_status_ttl();
    CHECK_THROWS(driver->get_position());
    sdk.replug(9);

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(sdk.open_calls == 2);
    CHECK(sdk.last_open_id == 9);
    wait_past_status_ttl();
    CHECK(driver->get_position() == 500);
}

// Issue #1500, the rig step: the client toggles Connected off and on across the replug. The Disconnect must
// not throw for the dead handle, and the Connect must find the focuser under its new id (decision 0009 point 4).
// Falsified by: Disconnect rethrowing the NotConnected close of a removed focuser, or Connect reopening the id
// the driver was created with.
TEST_CASE("ZWO EAF Focuser Driver - A Connected toggle across a replug reopens the focuser",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    sdk.remove();
    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
    sdk.replug(9);

    CHECK_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    CHECK(sdk.last_open_id == 9);
}

// Issue #1500: every Connect binds the serial the focuser had, so a replug while the client is disconnected that
// swaps two focusers' SDK ids still opens the same unit, and its reads go to that unit's new id.
// Falsified by: Connect opening the id the driver was created with, or the first enumerated focuser, or keeping
// the first candidate it opened.
TEST_CASE("ZWO EAF Focuser Driver - Two focusers swapped while disconnected are told apart by serial",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    sdk.second_id = 4;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    driver->set_connected(false);

    sdk.remove();
    sdk.second_id = 3;
    sdk.replug(4);

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(sdk.last_open_id == 4);
    wait_past_status_ttl();
    CHECK(driver->get_position() == 500);
    CHECK(sdk.last_read_id == 4);
}

// Issue #1500: Disconnect from the lost state clears the driver without throwing (invariant 5).
// Falsified by: Disconnect rethrowing the NotConnected close of a removed focuser.
TEST_CASE("ZWO EAF Focuser Driver - Disconnect after a removal does not throw",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.remove();
    wait_past_status_ttl();
    CHECK_THROWS(driver->get_position());

    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

// Issue #1500 (B7): two focusers, and a replug swaps their SDK ids while Y is idle (no call fails on it). X's
// Connect binds its own serial, now on Y's old id. Y must not keep talking to that id: its next read is
// NotConnected, Connected reads false, and none of its calls reach X's unit.
// Falsified by: the wrapper serving a holder whose id now names another unit (no serial check at the shared open).
TEST_CASE("ZWO EAF Focuser Driver - An idle focuser does not drive the unit that took its id",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    sdk.second_id = 4;
    auto y = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, 3, sdk);  // EAF-SN-1
    auto x = alpacacore::vendor::zwo::create_zwo_eaf_focuser(1, 4, sdk);  // EAF-SN-2
    y->set_connected(true);
    x->set_connected(true);

    sdk.remove();
    sdk.second_id = 3;  // X's unit (EAF-SN-2) comes back on id 3, Y's (EAF-SN-1) on id 4
    sdk.replug(4);
    x->set_connected(false);
    x->set_connected(true);
    REQUIRE(x->get_connected());
    wait_past_status_ttl();
    CHECK(x->get_position() == 500);
    CHECK(sdk.last_read_id == 3);

    sdk.last_read_id = -1;
    sdk.last_move_id = -1;
    wait_past_status_ttl();
    // Every check runs, so a red run shows each of Y's calls that reached X's unit.
    const auto error_of = [](const std::function<void()>& fn) -> int {
        try {
            fn();
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code();
        }
        return 0;
    };
    CHECK(error_of([&] { (void)y->get_position(); }) == alpacacore::AlpacaError::NotConnected);
    CHECK(sdk.last_read_id == -1);
    CHECK_FALSE(y->get_connected());
    CHECK(error_of([&] { y->move(100); }) == alpacacore::AlpacaError::NotConnected);
    CHECK(sdk.last_move_id == -1);
    CHECK_NOTHROW(y->set_connected(false));
    CHECK(x->get_connected());
}

// Issue #1500 (B7b): a focuser that reports no serial cannot be proven after a replug. Y holds such a unit on
// id 3 and a replug swaps it with X's unit. X's Connect must not share Y's open of id 3, which nothing proves,
// nor bind the unit on id 4, which is not X's serial: it refuses with NotConnected and names the recovery.
// Falsified by: a candidate scan sharing a held open whose unit reported no serial, or Connect opening the id
// the driver was created with.
TEST_CASE("ZWO EAF Focuser Driver - A held focuser with no serial is not shared after a swap",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    sdk.serial.clear();  // Y's unit reports no serial
    sdk.second_id = 4;
    auto y = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, 3, sdk);
    auto x = alpacacore::vendor::zwo::create_zwo_eaf_focuser(1, 4, sdk);  // EAF-SN-2
    y->set_connected(true);
    x->set_connected(true);

    sdk.remove();
    sdk.second_id = 3;  // X's unit (EAF-SN-2) comes back on id 3, Y's unit on id 4
    sdk.replug(4);
    x->set_connected(false);

    std::string refusal;
    try {
        x->set_connected(true);
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        refusal = e.what();
    }
    CHECK_FALSE(x->get_connected());
    CHECK(refusal.find("EAF-SN-2") != std::string::npos);
    CHECK(refusal.find("re-save") != std::string::npos);
}

// Issue #1500, decision 7: the bridge passes only enumerated ids, so EAF_ERROR_INVALID_ID on an open id means the id is
// gone (the wrapper maps it to NotConnected on a per-device call, the fake models that). One such read is a lost link.
// Falsified by: the status refill counting a NotConnected SDK error as one more failed poll.
TEST_CASE("ZWO EAF Focuser Driver - A read on an id the SDK no longer holds is a lost link",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 500);

    sdk.replug(9);  // between two polls the unit came back on id 9; id 3 answers EAF_ERROR_INVALID_ID
    wait_past_status_ttl();
    require_alpaca_error([&] { (void)driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
}

// Issue #1500: a temperature read that reports the unit removed is positive evidence even though Position answered
// in the same refill; a step range read that does so fails the Connect and closes what it opened.
// Falsified by: the refill keeping a NotConnected temperature failure as an optional reading, or the connect
// accepting a NotConnected step range read as "range unavailable".
TEST_CASE("ZWO EAF Focuser Driver - A removed unit seen by an optional read is a lost link",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 500);

    sdk.temperature_removed = true;
    wait_past_status_ttl();
    require_alpaca_error([&] { (void)driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());

    sdk.temperature_removed = false;
    sdk.step_range_removed = true;
    require_alpaca_error([&] { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_calls == sdk.close_calls);
}

// Issue #1500, decision 4: the loss is logged once, with the serial and the last-known values. The serial is device
// text, so a control character in it never reaches the log raw (CWE-117).
// Falsified by: a WARN per failed read instead of one at the loss, or the serial logged unescaped.
TEST_CASE("ZWO EAF Focuser Driver - A lost link is logged once with its serial escaped",
          "[zwo][focuser][unit][fake-sdk][link]") {
    FakeEAFSDK sdk;
    sdk.serial = "EAF\nSN-1";
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 500);

    WarnCapture capture;
    sdk.remove();
    wait_past_status_ttl();
    for (int i = 0; i < 3; ++i) {
        CHECK_THROWS(driver->get_position());
    }
    REQUIRE(capture.messages.size() == 1);
    CHECK(capture.messages[0].find("EAF?SN-1") != std::string::npos);
    CHECK(capture.messages[0].find('\n') == std::string::npos);
    CHECK(capture.messages[0].find("500") != std::string::npos);    // last position
    CHECK(capture.messages[0].find("60000") != std::string::npos);  // max step
}

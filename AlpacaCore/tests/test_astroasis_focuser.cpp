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
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_hid_transport.h>
#include <alpacacore/version.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// Scripted HID device (open-astro#294). Answers the wire protocol the wrapper
// speaks and counts transactions per command. The state is shared so a test
// keeps a handle after handing the transport to the driver.
struct FakeHidState {
    std::mutex mutex;
    int status_reads = 0;  // cmd 0x32
    int config_reads = 0;  // cmd 0x30 / 0x3a
    int moves = 0;         // cmd 0x36
    int halts = 0;         // cmd 0x37
    int position = 1234;
    bool moving = false;
    bool dead = false;         // reads time out, as a pulled cable does
    bool fail_config = false;  // MaxStep (0x30) goes unanswered
    bool fail_move = false;    // the device moves (0x36) but the reply is lost
    bool fail_halt = false;    // the device stops (0x37) but the reply is lost
    int closes = 0;
    std::vector<std::uint8_t> pending;

    int transactions() {
        std::lock_guard<std::mutex> lock(mutex);
        return status_reads + config_reads + moves + halts;
    }
};

class FakeHidTransport final : public alpacacore::vendor::astroasis::AstroasisHidTransport {
public:
    explicit FakeHidTransport(std::shared_ptr<FakeHidState> state) : state_(std::move(state)) {}

    bool open(const std::string&) override { return true; }
    void close() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->closes;
    }

    int write(const std::uint8_t* data, std::size_t length) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const std::uint8_t cmd = data[1];
        std::vector<std::uint8_t> resp;
        auto be32 = [&resp](std::uint32_t v) {
            for (int shift = 24; shift >= 0; shift -= 8) resp.push_back(static_cast<std::uint8_t>(v >> shift));
        };
        resp.push_back(cmd);
        switch (cmd) {
            case 0x11:
                resp.insert(resp.end(), {1, 0});
                break;
            case 0x10:
                resp.push_back(4);
                be32(0);
                break;
            case 0x32:
                ++state_->status_reads;
                resp.push_back(14);
                be32(2048);        // internal ADC
                be32(0x80000000);  // no external probe
                resp.push_back(0);
                resp.push_back(state_->moving ? 1 : 0);
                be32(static_cast<std::uint32_t>(state_->position));
                break;
            case 0x30:
                ++state_->config_reads;
                if (state_->fail_config) break;
                resp.push_back(18);
                be32(0xFFFFFFFF);
                be32(50000);
                resp.insert(resp.end(), 10, 0);
                break;
            case 0x36:
                ++state_->moves;
                state_->moving = true;
                if (state_->fail_move) break;
                resp.insert(resp.end(), {1, 0});
                break;
            case 0x37:
                ++state_->halts;
                state_->moving = false;
                if (state_->fail_halt) break;
                resp.insert(resp.end(), {1, 0});
                break;
            default:
                break;
        }
        state_->pending = std::move(resp);
        return static_cast<int>(length);
    }

    int read(std::uint8_t* data, std::size_t length, int) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->dead || state_->pending.empty()) {
            state_->pending.clear();
            return 0;
        }
        std::memset(data, 0, length);
        std::memcpy(data, state_->pending.data(), state_->pending.size());
        state_->pending.clear();
        return static_cast<int>(length);
    }

private:
    std::shared_ptr<FakeHidState> state_;
};

std::unique_ptr<alpacacore::FocuserDriver> make_fake_focuser(const std::shared_ptr<FakeHidState>& state) {
    return alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-fake",
                                                                   std::make_unique<FakeHidTransport>(state));
}

}  // namespace

TEST_CASE("Astroasis Focuser Driver - DeviceState reads the device at most once", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const int after_connect = state->transactions();

    const auto device_state = driver->get_device_state();
    CHECK_FALSE(device_state.empty());
    CHECK(state->status_reads <= 1);
    CHECK(state->transactions() - after_connect <= 1);
}

TEST_CASE("Astroasis Focuser Driver - static members never touch the device", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    driver->set_connected(true);
    const int after_connect = state->transactions();

    for (int i = 0; i < 5; ++i) {
        CHECK(driver->get_max_step() == 50000);
        CHECK(driver->get_max_increment() == 50000);
    }
    CHECK(state->transactions() == after_connect);
}

TEST_CASE("Astroasis Focuser Driver - Move and Halt invalidate the status cache", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    driver->set_connected(true);

    CHECK_FALSE(driver->get_is_moving());  // fills the cache
    driver->move(2000);
    CHECK(driver->get_is_moving());  // a stale frame would say false
    driver->halt();
    CHECK_FALSE(driver->get_is_moving());
    driver->move(3000);
    CHECK(driver->get_is_moving());
    driver->halt();
    CHECK_FALSE(driver->get_is_moving());
}

// Falsified by: astroasis_focuser_driver.cpp:268 delete status_cache_.invalidate() in move()'s catch block.
TEST_CASE("Astroasis Focuser Driver - failed move still invalidates the status cache", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    driver->set_connected(true);

    CHECK_FALSE(driver->get_is_moving());  // fills the cache
    state->fail_move = true;
    CHECK_THROWS_AS(driver->move(2000), alpacacore::AlpacaException);
    CHECK(driver->get_is_moving());  // the device moved; a stale frame says false
}

// Falsified by: astroasis_focuser_driver.cpp:249 delete status_cache_.invalidate() in halt()'s catch block.
TEST_CASE("Astroasis Focuser Driver - failed halt still invalidates the status cache", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    driver->set_connected(true);

    driver->move(2000);
    CHECK(driver->get_is_moving());  // fills the cache with moving=true
    state->fail_halt = true;
    CHECK_THROWS_AS(driver->halt(), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_is_moving());  // the device stopped; a stale frame says true
}

// Falsified by: astroasis_focuser_driver.cpp delete protocol_.disconnect() in the MaxStep catch at connect.
TEST_CASE("Astroasis Focuser Driver - failed MaxStep read at connect closes the handle", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    state->fail_config = true;
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->closes >= 1);
        state->fail_config = false;
    }
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_max_step() == 50000);
}

// Falsified by: astroasis_focuser_driver.cpp status_cache_ threshold raised from 3 to 100 (the link never latches).
TEST_CASE("Astroasis Focuser Driver - dead link never serves cached values", "[astroasis][focuser][unit]") {
    auto state = std::make_shared<FakeHidState>();
    auto driver = make_fake_focuser(state);
    driver->set_connected(true);
    REQUIRE(driver->get_position() == 1234);

    state->dead = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // past the TTL
    // The first reads below the threshold report the raw HID failure.
    for (int i = 0; i < 2; ++i) {
        try {
            (void)driver->get_position();
            FAIL("expected a throw");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
        }
    }
    // Third failure latches: DriverException naming the compromised link, Connected untouched.
    try {
        (void)driver->get_position();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(driver->get_connected());

    state->dead = false;
    CHECK(driver->get_position() == 1234);  // recovers with no reconnect
}

TEST_CASE("Astroasis Focuser Driver - Defaults", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Astroasis Oasis Focuser");
}

TEST_CASE("Astroasis Focuser Driver - Metadata", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    CHECK(driver->get_description() == "Astroasis Oasis Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Astroasis Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "ASTROASIS_FOCUSER_0");
}

TEST_CASE("Astroasis Focuser Driver - Disconnected Behavior", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list.
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
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Astroasis Focuser Driver - Connecting State", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("Astroasis Focuser Driver - Device Number Assignment", "[astroasis][focuser][unit]") {
    auto driver0 = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");
    auto driver1 = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw1");
    auto driver5 = alpacacore::vendor::astroasis::create_astroasis_focuser(5, "/dev/hidraw2");

    REQUIRE(driver0->get_device_number() == 0);
    REQUIRE(driver1->get_device_number() == 1);
    REQUIRE(driver5->get_device_number() == 5);
}

TEST_CASE("Astroasis Focuser Driver - Unique IDs", "[astroasis][focuser][unit]") {
    auto driver0 = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");
    auto driver1 = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "ASTROASIS_FOCUSER_0");
    CHECK(driver1->get_unique_id() == "ASTROASIS_FOCUSER_1");
}

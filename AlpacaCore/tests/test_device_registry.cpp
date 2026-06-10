// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/focuser_driver.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catch2_compat.h"

namespace {

class FakeDriver final : public alpacacore::AlpacaDriver {
public:
    FakeDriver(alpacacore::DeviceType type,
               int number,
               std::string name,
               std::string unique_id)
        : type_(type)
        , number_(number)
        , name_(std::move(name))
        , unique_id_(std::move(unique_id)) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return name_; }
    alpacacore::DeviceType get_device_type() const override { return type_; }
    std::string get_unique_id() const override { return unique_id_; }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 1; }

    bool get_connected() const override { return connected_; }
    void set_connected(bool connected) override { connected_ = connected; }

    std::vector<std::string> get_supported_actions() const override {
        return {"ping"};
    }

    std::string action(std::string_view action_name,
                       std::string_view action_parameters) override {
        (void)action_parameters;
        return std::string(action_name);
    }

    bool can_action(std::string_view action_name) const override {
        return action_name == "ping";
    }

    std::string command_blind(std::string_view command, bool raw) override {
        (void)command;
        (void)raw;
        return "ok";
    }

    bool command_bool(std::string_view command, bool raw) override {
        (void)command;
        (void)raw;
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)command;
        (void)raw;
        return "ok";
    }

private:
    alpacacore::DeviceType type_;
    int number_{};
    std::string name_;
    std::string unique_id_;
    bool connected_{false};
};

struct RegistryGuard {
    RegistryGuard() { alpacacore::management::DeviceRegistry::instance().clear(); }
    ~RegistryGuard() { alpacacore::management::DeviceRegistry::instance().clear(); }
};

} // namespace

TEST_CASE("DeviceRegistry register, lookup, and unregister", "[registry]") {
    RegistryGuard guard;
    auto& registry = alpacacore::management::DeviceRegistry::instance();

    auto cam0 = std::make_shared<FakeDriver>(alpacacore::DeviceType::Camera, 0, "Cam0", "cam-0");
    auto cam1 = std::make_shared<FakeDriver>(alpacacore::DeviceType::Camera, 1, "Cam1", "cam-1");

    REQUIRE(registry.register_device(cam0));
    REQUIRE(registry.register_device(cam1));
    REQUIRE(registry.device_count() == 2);

    REQUIRE(registry.get_device(alpacacore::DeviceType::Camera, 0) == cam0);
    REQUIRE(registry.get_device(alpacacore::DeviceType::Camera, 2) == nullptr);

    auto cameras = registry.get_devices_by_type(alpacacore::DeviceType::Camera);
    REQUIRE(cameras.size() == 2);

    auto duplicate = std::make_shared<FakeDriver>(alpacacore::DeviceType::Camera, 0, "Dup", "dup");
    REQUIRE_FALSE(registry.register_device(duplicate));

    REQUIRE(registry.unregister_device(alpacacore::DeviceType::Camera, 0));
    REQUIRE_FALSE(registry.unregister_device(alpacacore::DeviceType::Camera, 0));
    REQUIRE(registry.device_count() == 1);
}

TEST_CASE("DeviceRegistry reports device capabilities", "[registry]") {
    RegistryGuard guard;
    auto& registry = alpacacore::management::DeviceRegistry::instance();

    auto scope0 = std::make_shared<FakeDriver>(alpacacore::DeviceType::Telescope, 0, "Scope0", "scope-0");
    auto scope1 = std::make_shared<FakeDriver>(alpacacore::DeviceType::Telescope, 1, "Scope1", "scope-1");

    REQUIRE(registry.register_device(scope0));
    REQUIRE(registry.register_device(scope1));

    auto caps = registry.get_all_device_capabilities();
    REQUIRE(caps.size() == 2);

    auto find_by_unique_id = [&](const std::string& unique_id) {
        return std::find_if(caps.begin(), caps.end(),
                            [&](const alpacacore::DeviceCapabilities& cap) {
                                return cap.unique_id == unique_id;
                            });
    };

    auto cap0 = find_by_unique_id("scope-0");
    REQUIRE(cap0 != caps.end());
    REQUIRE(cap0->name == "Scope0");
    REQUIRE(cap0->driver_info == "fake driver");
    REQUIRE(cap0->driver_version == "0.0.1");
    REQUIRE(cap0->interface_version == 1);
}

namespace {

// Minimal focuser used to pin the DeviceState omit-on-throw contract: a getter
// that throws -- AlpacaException or any unwrapped vendor exception -- means the
// property is omitted, never that get_device_state() itself throws.
class ThrowingFocuser final : public alpacacore::FocuserDriver {
public:
    int get_device_number() const override { return 0; }
    std::string get_name() const override { return "ThrowingFocuser"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Focuser; }
    std::string get_unique_id() const override { return "throwing-focuser"; }
    std::string get_description() const override { return "fake device"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 4; }
    bool get_connected() const override { return true; }
    void set_connected(bool) override {}
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }

    bool get_absolute() const override { return true; }
    bool get_is_moving() const override { return false; }
    int get_max_step() const override { return 1000; }
    int get_max_increment() const override { return 1000; }
    int get_position() const override { throw std::runtime_error("unwrapped vendor SDK error"); }
    double get_step_size() const override { return 1.0; }
    bool get_temp_comp_available() const override { return false; }
    bool get_temp_comp() const override { return false; }
    void set_temp_comp(bool) override {}
    double get_temperature() const override {
        throw alpacacore::AlpacaException("not implemented", alpacacore::AlpacaError::NotImplemented);
    }
    void halt() override {}
    void move(int) override {}
};

}  // namespace

TEST_CASE("DeviceState omits properties whose getters throw", "[driver]") {
    ThrowingFocuser focuser;

    std::vector<alpacacore::DeviceState> state;
    REQUIRE_NOTHROW(state = focuser.get_device_state());

    auto has = [&state](const std::string& name) {
        return std::any_of(state.begin(), state.end(),
                           [&](const alpacacore::DeviceState& entry) { return entry.name == name; });
    };

    // Healthy getter and the mandatory TimeStamp are reported.
    CHECK(has("IsMoving"));
    CHECK(has("TimeStamp"));
    // Position throws an unwrapped std::runtime_error, Temperature a proper
    // AlpacaException -- both are omitted rather than propagated.
    CHECK_FALSE(has("Position"));
    CHECK_FALSE(has("Temperature"));
}

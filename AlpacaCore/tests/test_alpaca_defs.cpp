// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include <catch2/catch_all.hpp>

#include <alpacacore/alpaca_defs.h>
#include <string>

TEST_CASE("Device type to string mapping", "[alpaca_defs]") {
    using namespace alpacacore;

    REQUIRE(std::string(device_type_to_string(DeviceType::Camera)) == "Camera");
    REQUIRE(std::string(device_type_to_string(DeviceType::Telescope)) == "Telescope");
    REQUIRE(std::string(device_type_to_string(DeviceType::FilterWheel)) == "FilterWheel");
    REQUIRE(std::string(device_type_to_string(DeviceType::Focuser)) == "Focuser");
    REQUIRE(std::string(device_type_to_string(DeviceType::Rotator)) == "Rotator");
    REQUIRE(std::string(device_type_to_string(DeviceType::Dome)) == "Dome");
    REQUIRE(std::string(device_type_to_string(DeviceType::Shutter)) == "Shutter");
    REQUIRE(std::string(device_type_to_string(DeviceType::Switch)) == "Switch");
    REQUIRE(std::string(device_type_to_string(DeviceType::CoverCalibrator)) == "CoverCalibrator");
    REQUIRE(std::string(device_type_to_string(DeviceType::ObservingConditions)) == "ObservingConditions");
    REQUIRE(std::string(device_type_to_string(DeviceType::SafetyMonitor)) == "SafetyMonitor");

    REQUIRE(std::string(device_type_to_string(static_cast<DeviceType>(999))) == "Unknown");
}

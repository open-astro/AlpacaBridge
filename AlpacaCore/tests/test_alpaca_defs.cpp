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

#include "catch2_compat.h"

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
    REQUIRE(std::string(device_type_to_string(DeviceType::Switch)) == "Switch");
    REQUIRE(std::string(device_type_to_string(DeviceType::CoverCalibrator)) == "CoverCalibrator");
    REQUIRE(std::string(device_type_to_string(DeviceType::ObservingConditions)) == "ObservingConditions");
    REQUIRE(std::string(device_type_to_string(DeviceType::SafetyMonitor)) == "SafetyMonitor");

    REQUIRE(std::string(device_type_to_string(static_cast<DeviceType>(999))) == "Unknown");
}

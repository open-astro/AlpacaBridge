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

#pragma once

#include <alpacacore/alpaca_defs.h>
#include <vector>
#include <string>

namespace alpacacore {

/**
 * @brief Device capability information.
 */
struct DeviceCapabilities {
    DeviceType type;
    int device_number{};
    std::string name;
    std::string unique_id;
    std::string description;
    std::string driver_info;
    std::string driver_version;
    int interface_version{};
};

/**
 * @brief Get capabilities for all registered devices.
 */
std::vector<DeviceCapabilities> get_all_device_capabilities();

} // namespace alpacacore


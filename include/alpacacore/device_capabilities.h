// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaCore/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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


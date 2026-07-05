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


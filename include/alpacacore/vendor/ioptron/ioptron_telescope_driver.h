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

#pragma once

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <memory>

namespace alpacacore::vendor::ioptron {

/**
 * @brief Factory function to create iOptron telescope driver.
 *
 * @param device_number Alpaca device number
 * @param connection_info Connection information (serial port or network)
 * @return Unique pointer to telescope driver
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope(
    int device_number,
    const ConnectionInfo& connection_info);

} // namespace alpacacore::vendor::ioptron


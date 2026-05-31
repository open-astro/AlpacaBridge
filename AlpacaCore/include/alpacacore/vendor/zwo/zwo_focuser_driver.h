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

#include <alpacacore/focuser_driver.h>
#include <memory>

namespace alpacacore::vendor::zwo {

/**
 * @brief Create a ZWO EAF focuser driver by focuser ID.
 *
 * @param device_number Alpaca device number
 * @param focuser_id ZWO EAF SDK focuser ID
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser(int device_number, int focuser_id);

/**
 * @brief Create a ZWO EAF focuser driver by focuser index (enumeration order).
 *
 * @param device_number Alpaca device number
 * @param focuser_index ZWO EAF SDK focuser index (0-based)
 * @return Unique pointer to focuser driver
 */
std::unique_ptr<FocuserDriver> create_zwo_eaf_focuser_by_index(int device_number, int focuser_index);

} // namespace alpacacore::vendor::zwo

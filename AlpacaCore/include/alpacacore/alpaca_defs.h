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

#include <string>
#include <cstdint>
#include <vector>
#include <variant>

namespace alpacacore {

/**
 * @brief Alpaca device types.
 */
enum class DeviceType {
    Camera,
    Telescope,
    FilterWheel,
    Focuser,
    Rotator,
    Dome,
    Switch,
    CoverCalibrator,
    ObservingConditions,
    SafetyMonitor
};

/**
 * @brief Alpaca response header structure.
 *
 * Higher layers (e.g., AlpacaHTTP) use this to construct JSON responses.
 */
struct AlpacaResponseHeader {
    int client_transaction_id{};
    int server_transaction_id{};
    int error_number{};
    std::string error_message;
};

/**
 * @brief Device connection state.
 */
enum class ConnectionState {
    NotConnected,
    Connected,
    Connecting,
    Disconnecting
};

/**
 * @brief Device state value for DeviceState responses.
 */
using DeviceStateValue = std::variant<bool, std::int32_t, double, std::string>;

/**
 * @brief Device state entry for DeviceState responses.
 */
struct DeviceState {
    std::string name;
    DeviceStateValue value;
};

/**
 * @brief Get device type name as string.
 */
const char* device_type_to_string(DeviceType type);

} // namespace alpacacore

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

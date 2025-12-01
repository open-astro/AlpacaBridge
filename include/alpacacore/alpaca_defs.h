// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <string>
#include <cstdint>

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
    Shutter,
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
 * @brief Get device type name as string.
 */
const char* device_type_to_string(DeviceType type);

} // namespace alpacacore


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
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::playerone {

struct PlayerOnePWInfo {
    int handle{-1};             // SDK handle (PWProperties::Handle)
    std::string name;           // PWProperties::Name (e.g. "PhoenixWheel")
    int position_count{};       // filter capacity (e.g. 5-position)
    std::string serial_number;  // PWProperties::SN
};

/**
 * Thin wrapper around the Player One Phoenix Filter Wheel SDK (libPlayerOnePW)
 * v1.2.3. Separate from PlayerOneSDKWrapper because the wheel ships in its own
 * SDK library with an unrelated C API.
 *
 * - Singleton: enumeration state (POAGetPWCount) is process-wide.
 * - A std::mutex serializes calls into the SDK.
 * - open_wheel/close_wheel are reference counted per handle so multiple
 *   logical opens map onto one SDK open.
 *
 * PWErrors != PW_OK is translated to AlpacaException.
 */
class PlayerOnePWSDKWrapper {
public:
    static PlayerOnePWSDKWrapper& instance();

    std::vector<PlayerOnePWInfo> enumerate_wheels();
    bool get_wheel_info_by_index(int wheel_index, PlayerOnePWInfo& info);
    bool get_wheel_info_by_handle(int handle, PlayerOnePWInfo& info);

    void open_wheel(int handle);
    void close_wheel(int handle);

    // Returns -1 while the wheel is moving (matches the ASCOM Position
    // contract; the SDK reports PW_ERROR_IS_MOVING in that window).
    int get_position(int handle);
    void goto_position(int handle, int position);
    bool is_moving(int handle);

    bool get_one_way(int handle);
    void set_one_way(int handle, bool one_way);

    // Filter alias / focus offset are stored on the wheel itself.
    std::string get_filter_alias(int handle, int position);
    int get_focus_offset(int handle, int position);

    // Recovery for PW_ERROR_FIRMWARE_ERROR (filter/hole misalignment).
    void reset_wheel(int handle);

    std::string get_sdk_version();
    int get_api_version();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    PlayerOnePWSDKWrapper();
    ~PlayerOnePWSDKWrapper();
};

}  // namespace alpacacore::vendor::playerone

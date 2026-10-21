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
#include <vector>

namespace alpacacore::vendor::playerone {

struct PlayerOnePWInfo {
    int handle{-1};             // SDK handle (PWProperties::Handle)
    std::string name;           // PWProperties::Name (e.g. "PhoenixWheel")
    int position_count{};       // filter capacity (e.g. 5-position)
    std::string serial_number;  // PWProperties::SN
};

// Phoenix filter wheel SDK seam (issue #1256). Deliberately non-owning: the
// implementation must outlive its driver. The destructor is protected and
// non-virtual so nothing can be deleted through the interface.
//
// Position contract: get_position() returns -1 while the wheel is moving
// (matches the ASCOM Position contract; the SDK reports PW_ERROR_IS_MOVING in
// that window).
class PlayerOnePWSDK {
public:
    virtual std::vector<PlayerOnePWInfo> enumerate_wheels() = 0;
    virtual bool get_wheel_info_by_index(int wheel_index, PlayerOnePWInfo& info) = 0;
    virtual bool get_wheel_info_by_handle(int handle, PlayerOnePWInfo& info) = 0;

    virtual void open_wheel(int handle) = 0;
    virtual void close_wheel(int handle) = 0;

    virtual int get_position(int handle) = 0;
    virtual void goto_position(int handle, int position) = 0;
    virtual bool is_moving(int handle) = 0;

    virtual bool get_one_way(int handle) = 0;
    virtual void set_one_way(int handle, bool one_way) = 0;

    // Filter alias / focus offset are stored on the wheel itself.
    virtual std::string get_filter_alias(int handle, int position) = 0;
    virtual int get_focus_offset(int handle, int position) = 0;

    // Recovery for PW_ERROR_FIRMWARE_ERROR (filter/hole misalignment).
    virtual void reset_wheel(int handle) = 0;

    virtual std::string get_sdk_version() = 0;

protected:
    ~PlayerOnePWSDK() = default;
};

}  // namespace alpacacore::vendor::playerone

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

namespace alpacacore::vendor::zwo {

struct ZWOEFWInfo {
    int wheel_id{};
    std::string name;
    int slot_count{};
    // SDK enumeration slot, not the device ID or a filter position; unknown for ID-only reads.
    int slot_index{-1};
};

/**
 * @brief Abstract ZWO EFW SDK surface used by the filter wheel driver.
 *
 * Production passes the ZWOEFWSDKWrapper singleton; tests inject a scripted
 * fake through the factory overloads in zwo_filterwheel_driver.h, so the driver
 * runs without the vendor SDK. Same shape as ZWOEAFSDK and ZWOCAASDK: the
 * destructor is protected and non-virtual, the driver holds a reference.
 */
class ZWOEFWSDK {
public:
    virtual std::vector<ZWOEFWInfo> enumerate_wheels() = 0;
    virtual bool get_wheel_info_by_id(int wheel_id, ZWOEFWInfo& info) = 0;
    virtual bool get_wheel_info_by_index(int wheel_index, ZWOEFWInfo& info) = 0;

    virtual void open_wheel(int wheel_id) = 0;
    virtual void close_wheel(int wheel_id) = 0;

    virtual int get_position(int wheel_id) = 0;
    virtual void set_position(int wheel_id, int position) = 0;

    virtual void set_unidirectional(int wheel_id, bool enabled) = 0;
    virtual bool get_unidirectional(int wheel_id) = 0;
    virtual void calibrate(int wheel_id) = 0;

    virtual int get_hardware_error_code(int wheel_id) = 0;
    virtual std::string get_serial_number(int wheel_id) = 0;
    virtual std::string get_firmware_version(int wheel_id) = 0;
    virtual std::string get_sdk_version() = 0;

protected:
    ~ZWOEFWSDK() = default;
};

}  // namespace alpacacore::vendor::zwo

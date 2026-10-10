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

struct ZWOEAFFocuserInfo {
    int focuser_id{};
    std::string name;
    int max_step{};
};

/**
 * @brief Abstract ZWO EAF SDK surface used by the focuser driver.
 *
 * Production passes the ZWOEAFSDKWrapper singleton; tests inject a scripted
 * fake through the factory overloads in zwo_focuser_driver.h, so the driver
 * runs without the vendor SDK. Same shape as ZWOCAASDK: the destructor is
 * protected and non-virtual, the driver holds a reference.
 */
class ZWOEAFSDK {
public:
    virtual std::vector<ZWOEAFFocuserInfo> enumerate_focusers() = 0;
    virtual bool get_focuser_info_by_id(int focuser_id, ZWOEAFFocuserInfo& info) = 0;
    virtual bool get_focuser_info_by_index(int focuser_index, ZWOEAFFocuserInfo& info) = 0;

    virtual void open_focuser(int focuser_id) = 0;
    virtual void close_focuser(int focuser_id) = 0;

    virtual bool is_moving(int focuser_id) = 0;
    virtual int get_position(int focuser_id) = 0;
    virtual void move(int focuser_id, int position) = 0;
    virtual void stop(int focuser_id) = 0;

    virtual int get_max_step(int focuser_id) = 0;
    virtual int get_step_range(int focuser_id) = 0;

    virtual double get_temperature(int focuser_id) = 0;
    virtual std::string get_serial_number(int focuser_id) = 0;
    virtual std::string get_firmware_version(int focuser_id) = 0;
    virtual std::string get_sdk_version() = 0;

protected:
    ~ZWOEAFSDK() = default;
};

}  // namespace alpacacore::vendor::zwo

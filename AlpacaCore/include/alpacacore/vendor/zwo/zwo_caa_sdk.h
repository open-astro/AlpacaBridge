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

struct ZWOCAARotatorInfo {
    int rotator_id{};
    std::string name;
    double max_degree{};
};

struct ZWOCAAMotionStatus {
    bool is_moving{};
    bool hand_control{};
};

/**
 * @brief Abstract ZWO CAA SDK surface used by the rotator driver.
 *
 * Production passes the ZWOCAASDKWrapper singleton; tests inject a scripted
 * fake through the factory overloads in zwo_rotator_driver.h, so the driver
 * runs without the vendor SDK. The destructor is protected and non-virtual:
 * nothing owns a ZWOCAASDK*, the driver holds a reference.
 */
class ZWOCAASDK {
public:
    virtual std::vector<ZWOCAARotatorInfo> enumerate_rotators() = 0;
    virtual bool get_rotator_info_by_id(int rotator_id, ZWOCAARotatorInfo& info) = 0;
    virtual bool get_rotator_info_by_index(int rotator_index, ZWOCAARotatorInfo& info) = 0;

    virtual void open_rotator(int rotator_id) = 0;
    virtual void close_rotator(int rotator_id) = 0;

    virtual ZWOCAAMotionStatus get_motion_status(int rotator_id) = 0;
    virtual double get_degree(int rotator_id) = 0;
    virtual void move_relative(int rotator_id, double angle) = 0;
    virtual void move_absolute(int rotator_id, double angle) = 0;
    virtual void move_mechanical(int rotator_id, double angle) = 0;
    virtual void stop(int rotator_id) = 0;
    virtual void sync_degree(int rotator_id, double angle) = 0;

    virtual double get_max_degree(int rotator_id) = 0;
    virtual double get_temperature(int rotator_id) = 0;

    virtual bool get_reverse(int rotator_id) = 0;
    virtual void set_reverse(int rotator_id, bool reverse) = 0;

    virtual std::string get_serial_number(int rotator_id) = 0;
    virtual std::string get_firmware_version(int rotator_id) = 0;
    virtual std::string get_rotator_type(int rotator_id) = 0;
    virtual std::string get_sdk_version() = 0;

protected:
    ~ZWOCAASDK() = default;
};

}  // namespace alpacacore::vendor::zwo

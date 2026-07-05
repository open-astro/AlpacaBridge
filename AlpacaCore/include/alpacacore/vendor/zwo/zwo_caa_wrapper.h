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

#include <memory>
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

class ZWOCAASDKWrapper {
public:
    static ZWOCAASDKWrapper& instance();

    std::vector<ZWOCAARotatorInfo> enumerate_rotators();
    bool get_rotator_info_by_id(int rotator_id, ZWOCAARotatorInfo& info);
    bool get_rotator_info_by_index(int rotator_index, ZWOCAARotatorInfo& info);

    void open_rotator(int rotator_id);
    void close_rotator(int rotator_id);

    ZWOCAAMotionStatus get_motion_status(int rotator_id);
    double get_degree(int rotator_id);
    void move_relative(int rotator_id, double angle);
    void move_absolute(int rotator_id, double angle);
    void move_mechanical(int rotator_id, double angle);
    void stop(int rotator_id);
    void sync_degree(int rotator_id, double angle);

    double get_max_degree(int rotator_id);
    double get_temperature(int rotator_id);

    bool get_reverse(int rotator_id);
    void set_reverse(int rotator_id, bool reverse);

    std::string get_serial_number(int rotator_id);
    std::string get_firmware_version(int rotator_id);
    std::string get_rotator_type(int rotator_id);
    std::string get_sdk_version();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOCAASDKWrapper();
    ~ZWOCAASDKWrapper();
};

} // namespace alpacacore::vendor::zwo

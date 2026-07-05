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

struct ZWOEFWInfo {
    int wheel_id{};
    std::string name;
    int slot_count{};
};

class ZWOEFWSDKWrapper {
public:
    static ZWOEFWSDKWrapper& instance();

    std::vector<ZWOEFWInfo> enumerate_wheels();
    bool get_wheel_info_by_id(int wheel_id, ZWOEFWInfo& info);
    bool get_wheel_info_by_index(int wheel_index, ZWOEFWInfo& info);

    void open_wheel(int wheel_id);
    void close_wheel(int wheel_id);

    int get_position(int wheel_id);
    void set_position(int wheel_id, int position);

    void set_unidirectional(int wheel_id, bool enabled);
    bool get_unidirectional(int wheel_id);
    void calibrate(int wheel_id);

    int get_hardware_error_code(int wheel_id);
    std::string get_serial_number(int wheel_id);
    std::string get_firmware_version(int wheel_id);
    std::string get_sdk_version();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOEFWSDKWrapper();
    ~ZWOEFWSDKWrapper();
};

} // namespace alpacacore::vendor::zwo

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

#include <alpacacore/vendor/zwo/zwo_efw_sdk.h>

#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

class ZWOEFWSDKWrapper final : public ZWOEFWSDK {
public:
    static ZWOEFWSDKWrapper& instance();

    std::vector<ZWOEFWInfo> enumerate_wheels() override;
    bool get_wheel_info_by_id(int wheel_id, ZWOEFWInfo& info) override;
    bool get_wheel_info_by_index(int wheel_index, ZWOEFWInfo& info) override;

    void open_wheel(int wheel_id) override;
    void close_wheel(int wheel_id) override;

    int get_position(int wheel_id) override;
    void set_position(int wheel_id, int position) override;

    void set_unidirectional(int wheel_id, bool enabled) override;
    bool get_unidirectional(int wheel_id) override;
    void calibrate(int wheel_id) override;

    int get_hardware_error_code(int wheel_id) override;
    std::string get_serial_number(int wheel_id) override;
    std::string get_firmware_version(int wheel_id) override;
    std::string get_sdk_version() override;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOEFWSDKWrapper();
    ~ZWOEFWSDKWrapper();
};

} // namespace alpacacore::vendor::zwo

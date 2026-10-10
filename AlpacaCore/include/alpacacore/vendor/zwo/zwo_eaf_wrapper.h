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

#include <alpacacore/vendor/zwo/zwo_eaf_sdk.h>

#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

class ZWOEAFSDKWrapper final : public ZWOEAFSDK {
public:
    static ZWOEAFSDKWrapper& instance();

    std::vector<ZWOEAFFocuserInfo> enumerate_focusers() override;
    bool get_focuser_info_by_id(int focuser_id, ZWOEAFFocuserInfo& info) override;
    bool get_focuser_info_by_index(int focuser_index, ZWOEAFFocuserInfo& info) override;

    void open_focuser(int focuser_id) override;
    void close_focuser(int focuser_id) override;

    bool is_moving(int focuser_id) override;
    int get_position(int focuser_id) override;
    void move(int focuser_id, int position) override;
    void stop(int focuser_id) override;

    int get_max_step(int focuser_id) override;
    int get_step_range(int focuser_id) override;

    double get_temperature(int focuser_id) override;
    std::string get_serial_number(int focuser_id) override;
    std::string get_firmware_version(int focuser_id) override;
    std::string get_sdk_version() override;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOEAFSDKWrapper();
    ~ZWOEAFSDKWrapper();
};

} // namespace alpacacore::vendor::zwo

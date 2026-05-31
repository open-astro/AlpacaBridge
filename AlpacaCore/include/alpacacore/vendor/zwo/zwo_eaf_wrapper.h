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

namespace alpacacore::vendor::zwo {

struct ZWOEAFFocuserInfo {
    int focuser_id{};
    std::string name;
    int max_step{};
};

class ZWOEAFSDKWrapper {
public:
    static ZWOEAFSDKWrapper& instance();

    std::vector<ZWOEAFFocuserInfo> enumerate_focusers();
    bool get_focuser_info_by_id(int focuser_id, ZWOEAFFocuserInfo& info);
    bool get_focuser_info_by_index(int focuser_index, ZWOEAFFocuserInfo& info);

    void open_focuser(int focuser_id);
    void close_focuser(int focuser_id);

    bool is_moving(int focuser_id);
    int get_position(int focuser_id);
    void move(int focuser_id, int position);
    void stop(int focuser_id);

    int get_max_step(int focuser_id);
    int get_step_range(int focuser_id);

    double get_temperature(int focuser_id);
    std::string get_serial_number(int focuser_id);
    std::string get_firmware_version(int focuser_id);
    std::string get_sdk_version();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    ZWOEAFSDKWrapper();
    ~ZWOEAFSDKWrapper();
};

} // namespace alpacacore::vendor::zwo

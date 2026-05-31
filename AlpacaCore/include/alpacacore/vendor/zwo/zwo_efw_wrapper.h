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

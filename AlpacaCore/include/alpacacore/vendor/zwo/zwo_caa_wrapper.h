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
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

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

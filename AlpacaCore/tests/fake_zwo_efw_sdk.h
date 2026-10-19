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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_efw_sdk.h>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted ZWO EFW SDK: one wheel (id 5, 7 slots), shared by the filter wheel unit
// tests, the connected stress case and the contract sweep.
// Every override runs under one internal mutex, so the fake is race-free under a
// concurrent storm (the ZWO driver tests run it through lifecycle stress and TSan);
// the public knobs and counters are plain fields for single-threaded tests, set before
// the driver starts and read after it stops. No call blocks except the explicit
// open_delay, which holds a connect open (contract sweep tier 2).

class FakeZWOEFWSDK final : public vendor::zwo::ZWOEFWSDK {
public:
    using Info = vendor::zwo::ZWOEFWInfo;

    int id{5};
    int slot_count{7};
    int position{0};
    bool unidirectional{false};
    bool fail_reads{false};
    // No wheel attached: enumeration is empty and the id lookups fail.
    bool no_wheels{false};
    // The next open_wheel() throws (a connect that must fail).
    bool fail_open{false};
    // open_wheel() sleeps this long before returning (a connect held open).
    std::chrono::milliseconds open_delay{0};
    int open_calls{0};
    int close_calls{0};
    int position_calls{0};
    int set_position_calls{0};
    int last_set_position{-1};
    int calibrate_calls{0};

    std::vector<Info> enumerate_wheels() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (no_wheels) return {};
        return {Info{id, "EFW", slot_count}};
    }
    bool get_wheel_info_by_id(int wheel_id, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (no_wheels || wheel_id != id) {
            return false;
        }
        info = Info{id, "EFW", slot_count};
        return true;
    }
    bool get_wheel_info_by_index(int wheel_index, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (no_wheels || wheel_index != 0) {
            return false;
        }
        info = Info{id, "EFW", slot_count};
        return true;
    }
    void open_wheel(int) override {
        if (open_delay.count() > 0) std::this_thread::sleep_for(open_delay);
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
    }
    void close_wheel(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls;
    }
    int get_position(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++position_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return position;
    }
    void set_position(int, int slot) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++set_position_calls;
        last_set_position = slot;
        if (slot < 0 || slot >= slot_count) {
            throw AlpacaException("EFWSetPosition: EFW_ERROR_INVALID_VALUE", AlpacaError::InvalidValue);
        }
        position = slot;
    }
    void set_unidirectional(int, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        unidirectional = enabled;
    }
    bool get_unidirectional(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return unidirectional;
    }
    void calibrate(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calibrate_calls;
    }
    int get_hardware_error_code(int) override { return 0; }
    std::string get_serial_number(int) override { return "EFW-SN-1"; }
    std::string get_firmware_version(int) override { return "1.0.0"; }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    mutable std::mutex mutex_;
};

}  // namespace alpacacore::test

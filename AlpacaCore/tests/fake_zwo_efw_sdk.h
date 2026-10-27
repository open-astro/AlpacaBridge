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

#include <atomic>
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
//
// KNOWN PARITY GAPS (the real EFW SDK is harsher here; prefer closing a gap):
// - With moving_reads at 0 (the default) a move settles at once. On hardware
//   EFWGetPosition reports -1 while the wheel turns and EFWSetPosition during a move
//   fails with EFW_ERROR_MOVING; set moving_reads to script that window.
// - The moving window ends after a number of reads, not after a time, and calibrate()
//   opens none, though the real wheel turns while it calibrates.
// - Serial number, firmware and SDK version are fixed strings.
// - fail_reads throws std::runtime_error; the real wrapper throws only AlpacaException.

class FakeZWOEFWSDK final : public vendor::zwo::ZWOEFWSDK {
public:
    using Info = vendor::zwo::ZWOEFWInfo;

    int id{5};
    int slot_index{0};
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
    // After a set_position(), get_position() reads -1 (moving) this many times, and a
    // set_position() inside that window throws InvalidOperation (EFW_ERROR_MOVING).
    int moving_reads{0};
    // What get_hardware_error_code() answers.
    int hardware_error_code{0};
    int open_calls{0};
    int last_open_id{-1};
    int close_calls{0};
    int position_calls{0};
    int set_position_calls{0};
    int last_set_position{-1};
    int calibrate_calls{0};
    int moving_refusals{0};
    // get_position() sleeps this long, outside the fake's mutex, before answering (a slow Position refill).
    // position_entered counts the calls that have started, so a test knows the refill is in flight.
    std::chrono::milliseconds position_delay{0};
    std::atomic<int> position_entered{0};
    // open_wheel() calls that have started (counted before open_delay), so a test knows a connect is in flight.
    std::atomic<int> open_entered{0};
    // set_position() throws a DriverException after it is counted (a refused or failed move command).
    bool fail_set_position{false};

    std::vector<Info> enumerate_wheels() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (no_wheels) return {};
        return {Info{id, "EFW", slot_count, slot_index}};
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
        if (no_wheels || wheel_index != slot_index) {
            return false;
        }
        info = Info{id, "EFW", slot_count, slot_index};
        return true;
    }
    void open_wheel(int wheel_id) override {
        ++open_entered;
        if (open_delay.count() > 0) std::this_thread::sleep_for(open_delay);
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
        last_open_id = wheel_id;
    }
    void close_wheel(int wheel_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        ++close_calls;
    }
    int get_position(int wheel_id) override {
        ++position_entered;
        if (position_delay.count() > 0) std::this_thread::sleep_for(position_delay);
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        ++position_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        if (moving_left_ > 0) {
            --moving_left_;
            return -1;
        }
        return position;
    }
    void set_position(int wheel_id, int slot) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        ++set_position_calls;
        if (fail_set_position) throw AlpacaException("scripted set_position failure", AlpacaError::DriverException);
        last_set_position = slot;
        if (slot < 0 || slot >= slot_count) {
            throw AlpacaException("EFWSetPosition: EFW_ERROR_INVALID_VALUE", AlpacaError::InvalidValue);
        }
        if (moving_left_ > 0) {
            ++moving_refusals;
            throw AlpacaException("EFWSetPosition: EFW_ERROR_MOVING", AlpacaError::InvalidOperation);
        }
        position = slot;
        moving_left_ = moving_reads;
    }
    void set_unidirectional(int wheel_id, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        unidirectional = enabled;
    }
    bool get_unidirectional(int wheel_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        return unidirectional;
    }
    void calibrate(int wheel_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        ++calibrate_calls;
    }
    int get_hardware_error_code(int wheel_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_id(wheel_id);
        return hardware_error_code;
    }
    std::string get_serial_number(int wheel_id) override {
        check_id(wheel_id);
        return "EFW-SN-1";
    }
    std::string get_firmware_version(int wheel_id) override {
        check_id(wheel_id);
        return "1.0.0";
    }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    // A wrong id is EFW_ERROR_INVALID_ID on hardware, which the wrapper maps to InvalidValue.
    void check_id(int wheel_id) const {
        if (wheel_id != id) throw AlpacaException("EFW: EFW_ERROR_INVALID_ID", AlpacaError::InvalidValue);
    }

    mutable std::mutex mutex_;
    int moving_left_{0};
};

}  // namespace alpacacore::test

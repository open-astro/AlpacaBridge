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
#include <alpacacore/vendor/playerone/playerone_pw_sdk.h>

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted Player One Phoenix Wheel SDK. One wheel, no background thread: a
// goto_position() puts the wheel in motion for a counted number of
// get_position()/is_moving() reads (set_moving_reads), which keeps the fake
// deterministic and race-free under TSan.
//
// KNOWN PARITY GAPS: one fake wheel only (no hot-plug enumeration, no shared
// handle across wheels); open/close are counted but the fake does not model
// the real wrapper's per-handle refcount (it counts every open and close and
// the open count is the balance). Motion ends after a fixed read count rather
// than elapsed time. A goto_position() while the wheel is moving throws
// InvalidOperation, the code the real wrapper maps PW_ERROR_IS_MOVING to; that
// the SDK refuses a goto during motion is an assumption (the harsher of the
// two possible answers). set_one_way/get_one_way and reset_wheel only store
// state. The Phoenix lineup is 5, 7 or 8 slots (set_slot_count refuses other
// counts); set_raw_slot_count bypasses that to drive the driver's
// "no filter positions" refusal.
class FakePlayerOnePWSDK final : public vendor::playerone::PlayerOnePWSDK {
public:
    using Info = vendor::playerone::PlayerOnePWInfo;

    FakePlayerOnePWSDK() {
        info_.handle = 41;
        info_.name = "FakePhoenixWheel";
        info_.position_count = 5;
        info_.serial_number = "fake-pw-41";
        rebuild_slots_locked();
    }

    std::vector<Info> enumerate_wheels() override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("enumerate_wheels");
        return no_wheels_ ? std::vector<Info>{} : std::vector<Info>{info_};
    }
    bool get_wheel_info_by_index(int wheel_index, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_wheel_info_by_index");
        if (no_wheels_ || wheel_index != 0) return false;
        info = info_;
        return true;
    }
    bool get_wheel_info_by_handle(int handle, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_wheel_info_by_handle");
        if (no_wheels_ || handle != info_.handle) return false;
        info = info_;
        return true;
    }

    void open_wheel(int handle) override {
        std::chrono::milliseconds delay;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            record("open_wheel");
            delay = open_delay_;
        }
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        std::lock_guard<std::mutex> lock(mutex_);
        check_handle_locked(handle);
        maybe_fail_locked("open_wheel");
        ++open_count_;
    }
    void close_wheel(int handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("close_wheel");
        check_handle_locked(handle);
        if (open_count_ > 0) --open_count_;
        maybe_fail_locked("close_wheel");
    }

    // -1 while moving, as the real wrapper reports PW_ERROR_IS_MOVING.
    int get_position(int handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_position");
        check_open_locked(handle);
        maybe_fail_locked("get_position");
        if (moving_reads_left_ > 0) {
            if (--moving_reads_left_ == 0) position_ = target_;
            return -1;
        }
        return position_;
    }
    void goto_position(int handle, int position) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("goto_position");
        check_open_locked(handle);
        maybe_fail_locked("goto_position");
        if (moving_reads_left_ > 0) {
            throw AlpacaException("POAGotoPosition: wheel is moving", AlpacaError::InvalidOperation);
        }
        if (position < 0 || position >= info_.position_count) {
            throw AlpacaException("POAGotoPosition: invalid argument", AlpacaError::InvalidValue);
        }
        target_ = position;
        if (moving_reads_ > 0 && position != position_) {
            moving_reads_left_ = moving_reads_;
        } else {
            position_ = position;
        }
    }
    bool is_moving(int handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("is_moving");
        check_open_locked(handle);
        return moving_reads_left_ > 0;
    }

    bool get_one_way(int handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_one_way");
        check_open_locked(handle);
        return one_way_;
    }
    void set_one_way(int handle, bool one_way) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("set_one_way");
        check_open_locked(handle);
        one_way_ = one_way;
    }

    std::string get_filter_alias(int handle, int position) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_filter_alias");
        check_open_locked(handle);
        maybe_fail_locked("get_filter_alias");
        check_slot_locked(position);
        return aliases_[static_cast<std::size_t>(position)];
    }
    int get_focus_offset(int handle, int position) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_focus_offset");
        check_open_locked(handle);
        maybe_fail_locked("get_focus_offset");
        check_slot_locked(position);
        return offsets_[static_cast<std::size_t>(position)];
    }

    void reset_wheel(int handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("reset_wheel");
        check_open_locked(handle);
        position_ = 0;
        target_ = 0;
        moving_reads_left_ = 0;
    }

    std::string get_sdk_version() override {
        std::lock_guard<std::mutex> lock(mutex_);
        record("get_sdk_version");
        return "fake-playerone-pw-sdk";
    }

    // --- scripting -----------------------------------------------------------
    // Phoenix lineup only: 5, 7 or 8 slots. Returns false (and changes nothing)
    // for any other count.
    bool set_slot_count(int slots) {
        if (slots != 5 && slots != 7 && slots != 8) return false;
        set_raw_slot_count(slots);
        return true;
    }
    // Any count, including 0, to model a wheel that reports no positions.
    void set_raw_slot_count(int slots) {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.position_count = slots;
        rebuild_slots_locked();
    }
    void set_alias(int position, const std::string& alias) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_slot_locked(position);
        aliases_[static_cast<std::size_t>(position)] = alias;
    }
    void set_offset(int position, int offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_slot_locked(position);
        offsets_[static_cast<std::size_t>(position)] = offset;
    }
    void set_no_wheels(bool no_wheels) {
        std::lock_guard<std::mutex> lock(mutex_);
        no_wheels_ = no_wheels;
    }
    void set_open_delay(std::chrono::milliseconds delay) {
        std::lock_guard<std::mutex> lock(mutex_);
        open_delay_ = delay;
    }
    // get_position() reads that report -1 after a goto to a different slot.
    void set_moving_reads(int reads) {
        std::lock_guard<std::mutex> lock(mutex_);
        moving_reads_ = reads;
    }
    void fail_next(const std::string& call) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_ = call;
    }

    int open_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_count_;
    }
    int call_count(const std::string& call) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = calls_.find(call);
        return it == calls_.end() ? 0 : it->second;
    }
    int handle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_.handle;
    }

private:
    void record(const std::string& call) { ++calls_[call]; }
    void rebuild_slots_locked() {
        const auto n = static_cast<std::size_t>(info_.position_count > 0 ? info_.position_count : 0);
        aliases_.assign(n, std::string());
        offsets_.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) aliases_[i] = "Slot" + std::to_string(i + 1);
        position_ = 0;
        target_ = 0;
        moving_reads_left_ = 0;
    }
    void check_handle_locked(int handle) const {
        if (handle != info_.handle) {
            throw AlpacaException("PlayerOnePW: invalid handle", AlpacaError::InvalidValue);
        }
    }
    void check_open_locked(int handle) const {
        check_handle_locked(handle);
        if (open_count_ <= 0) {
            throw AlpacaException("PlayerOnePW: wheel not opened", AlpacaError::NotConnected);
        }
    }
    void check_slot_locked(int position) const {
        if (position < 0 || position >= static_cast<int>(aliases_.size())) {
            throw AlpacaException("PlayerOnePW: invalid position", AlpacaError::InvalidValue);
        }
    }
    void maybe_fail_locked(const std::string& call) {
        if (fail_next_ == call) {
            fail_next_.clear();
            throw AlpacaException("FakePlayerOnePWSDK: scripted failure in " + call, AlpacaError::DriverException);
        }
    }

    mutable std::mutex mutex_;
    Info info_;
    std::vector<std::string> aliases_;
    std::vector<int> offsets_;
    std::map<std::string, int> calls_;
    std::string fail_next_;
    std::chrono::milliseconds open_delay_{0};
    int open_count_{0};
    int position_{0};
    int target_{0};
    int moving_reads_{0};
    int moving_reads_left_{0};
    bool one_way_{false};
    bool no_wheels_{false};
};

}  // namespace alpacacore::test

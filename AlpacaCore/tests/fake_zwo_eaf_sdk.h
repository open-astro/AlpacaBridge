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
#include <alpacacore/vendor/zwo/zwo_eaf_sdk.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted ZWO EAF SDK: one focuser (id 3), shared by the focuser unit tests, the
// connected stress case and the contract sweep. second_id >= 0 adds a second focuser
// (serial second_serial) to enumeration, open and the serial read.
// Every override runs under one internal mutex, so the fake is race-free under a
// concurrent storm (the ZWO driver tests run it through lifecycle stress and TSan);
// the public knobs and counters are plain fields for single-threaded tests, set before
// the driver starts and read after it stops. No call blocks except the explicit
// delays: open_delay holds a connect open (contract sweep tier 2), read_delay holds an
// is_moving() read and move_delay a move() or stop() in flight (getters-never-wait tests).
// The *_entered flags and `busy` are atomics a test polls from another thread; close_focuser()
// sets closed_while_busy when it runs while a delayed call is still in flight.

// KNOWN PARITY GAPS: move() settles a move at once (position/degree jump to the target and
// the moving flag stays whatever a test set), where the real EAFIsMoving reports motion until the unit
// arrives. A test that needs a moving window sets `moving` itself.
class FakeZWOEAFSDK final : public vendor::zwo::ZWOEAFSDK {
public:
    using Info = vendor::zwo::ZWOEAFFocuserInfo;

    int id{3};
    std::string serial{"EAF-SN-1"};
    // A second focuser on the same SDK (issue #1500 id swap); -1 = none.
    int second_id{-1};
    std::string second_serial{"EAF-SN-2"};
    int max_step{60000};
    int step_range{1000};
    int position{500};
    bool moving{false};
    double temperature{12.5};
    bool fail_reads{false};
    bool fail_temperature{false};
    bool temperature_not_supported{false};
    int moving_calls{0};
    int position_calls{0};
    int temperature_calls{0};
    int max_step_calls{0};
    int step_range_calls{0};
    int set_max_step_calls{0};
    int last_set_max_step{-1};
    int open_calls{0};
    int last_open_id{-1};
    // The SDK id of the last position read and of the last move (issue #1500: which unit a driver talks to).
    int last_read_id{-1};
    int last_move_id{-1};
    int close_calls{0};
    bool step_range_fails{false};
    bool removed{false};
    // The next open_focuser() throws (a connect that must fail).
    bool fail_open{false};
    // open_focuser() sleeps this long before returning (a connect held open).
    std::chrono::milliseconds open_delay{0};
    // is_moving() sleeps this long (a status refill held open); move()/stop() sleep move_delay.
    std::chrono::milliseconds read_delay{0};
    std::chrono::milliseconds move_delay{0};
    std::atomic<bool> open_entered{false};
    std::atomic<bool> read_entered{false};
    std::atomic<bool> move_entered{false};
    std::atomic<int> busy{0};
    std::atomic<bool> closed_while_busy{false};

    // Issue #1500: a USB drop. remove() makes open/close/read/serial throw what the wrapper throws for
    // EAF_ERROR_REMOVED (NotConnected) and the focusers vanish from enumeration; replug(id) brings this one
    // back under a new SDK id.
    void remove() {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = true;
    }
    void replug(int new_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = false;
        id = new_id;
    }

    std::vector<Info> enumerate_focusers() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (removed) return {};
        std::vector<Info> focusers{Info{id, "EAF", max_step}};
        if (second_id >= 0)
            focusers.insert(second_id < id ? focusers.begin() : focusers.end(), Info{second_id, "EAF", max_step});
        return focusers;
    }
    bool get_focuser_info_by_id(int focuser_id, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (removed || (focuser_id != id && focuser_id != second_id)) {
            return false;
        }
        info = Info{focuser_id, "EAF", max_step};
        return true;
    }
    bool get_focuser_info_by_index(int, Info& info) override { return get_focuser_info_by_id(id, info); }
    void open_focuser(int focuser_id) override {
        const InFlight in_flight(*this, open_delay, open_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_removed();
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
        last_open_id = focuser_id;
    }
    void close_focuser(int) override {
        if (busy.load() > 0) closed_while_busy = true;
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls;
        // The wrapper erases its bookkeeping, then EAFClose fails on the removed unit.
        throw_if_removed();
    }
    bool is_moving(int) override {
        const InFlight in_flight(*this, read_delay, read_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        ++moving_calls;
        throw_if_removed();
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return moving;
    }
    int get_position(int focuser_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++position_calls;
        last_read_id = focuser_id;
        throw_if_removed();
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return position;
    }
    void move(int focuser_id, int target) override {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        last_move_id = focuser_id;
        position = target;
    }
    void stop(int) override {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        moving = false;
    }
    int get_max_step(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++max_step_calls;
        return max_step;
    }
    void set_max_step(int, int value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++set_max_step_calls;
        last_set_max_step = value;
        max_step = value;  // the unit stores what it is given
    }
    int get_step_range(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++step_range_calls;
        if (step_range_fails) {
            throw std::runtime_error("step range unavailable");
        }
        return step_range;
    }
    double get_temperature(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++temperature_calls;
        if (temperature_not_supported) {
            throw AlpacaException("EAF temperature not supported", AlpacaError::NotImplemented);
        }
        if (fail_temperature) {
            throw std::runtime_error("temp sensor down");
        }
        return temperature;
    }
    std::string get_serial_number(int focuser_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_removed();
        return focuser_id == second_id ? second_serial : serial;
    }
    std::string get_firmware_version(int) override { return "1.0"; }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    void throw_if_removed() const {
        if (removed) throw AlpacaException("EAF_ERROR_REMOVED", AlpacaError::NotConnected);
    }

    // Marks a call in flight for its whole duration, sleeping `delay` first, outside mutex_.
    struct InFlight {
        InFlight(FakeZWOEAFSDK& fake, std::chrono::milliseconds delay, std::atomic<bool>& entered) : fake_(fake) {
            ++fake_.busy;
            entered = true;
            if (delay.count() > 0) std::this_thread::sleep_for(delay);
        }
        ~InFlight() { --fake_.busy; }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        FakeZWOEAFSDK& fake_;
    };

    mutable std::mutex mutex_;
};

}  // namespace alpacacore::test

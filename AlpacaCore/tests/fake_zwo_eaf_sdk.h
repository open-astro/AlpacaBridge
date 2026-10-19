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

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted ZWO EAF SDK: one focuser (id 3), shared by the focuser unit tests, the
// connected stress case and the contract sweep.
// Every override runs under one internal mutex, so the fake is race-free under a
// concurrent storm (the ZWO driver tests run it through lifecycle stress and TSan);
// the public knobs and counters are plain fields for single-threaded tests, set before
// the driver starts and read after it stops. No call blocks except the explicit
// open_delay, which holds a connect open (contract sweep tier 2).

// KNOWN PARITY GAPS: move() settles a move at once (position/degree jump to the target and
// the moving flag stays whatever a test set), where the real EAFIsMoving reports motion until the unit
// arrives. A test that needs a moving window sets `moving` itself.
class FakeZWOEAFSDK final : public vendor::zwo::ZWOEAFSDK {
public:
    using Info = vendor::zwo::ZWOEAFFocuserInfo;

    int id{3};
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
    int close_calls{0};
    bool step_range_fails{false};
    // The next open_focuser() throws (a connect that must fail).
    bool fail_open{false};
    // open_focuser() sleeps this long before returning (a connect held open).
    std::chrono::milliseconds open_delay{0};

    std::vector<Info> enumerate_focusers() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return {Info{id, "EAF", max_step}};
    }
    bool get_focuser_info_by_id(int focuser_id, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (focuser_id != id) {
            return false;
        }
        info = Info{id, "EAF", max_step};
        return true;
    }
    bool get_focuser_info_by_index(int, Info& info) override { return get_focuser_info_by_id(id, info); }
    void open_focuser(int) override {
        if (open_delay.count() > 0) std::this_thread::sleep_for(open_delay);
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
    }
    void close_focuser(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls;
    }
    bool is_moving(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++moving_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return moving;
    }
    int get_position(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++position_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return position;
    }
    void move(int, int target) override {
        std::lock_guard<std::mutex> lock(mutex_);
        position = target;
    }
    void stop(int) override {
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
    std::string get_serial_number(int) override { return "EAF-SN-1"; }
    std::string get_firmware_version(int) override { return "1.0"; }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    mutable std::mutex mutex_;
};

}  // namespace alpacacore::test

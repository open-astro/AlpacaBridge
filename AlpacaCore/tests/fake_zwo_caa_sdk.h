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
#include <alpacacore/vendor/zwo/zwo_caa_sdk.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted ZWO CAA SDK: one rotator (id 7), shared by the rotator unit tests, the
// connected stress case and the contract sweep.
// Every override runs under one internal mutex, so the fake is race-free under a
// concurrent storm (the ZWO driver tests run it through lifecycle stress and TSan);
// the public knobs and counters are plain fields for single-threaded tests, set before
// the driver starts and read after it stops. No call blocks except the explicit
// delays: open_delay holds a connect open (contract sweep tier 2), read_delay holds a
// get_motion_status() read and move_delay a move, stop or set_reverse in flight (getters-never-wait tests).
// The *_entered flags and `busy` are atomics a test polls from another thread; close_rotator()
// sets closed_while_busy when it runs while a delayed call is still in flight.

// KNOWN PARITY GAPS: move_absolute() settles a move at once (position/degree jump to the target and
// the moving flag stays whatever a test set), where the real CAAIsMoving reports motion until the unit
// arrives. A test that needs a moving window sets `moving` itself.
class FakeZWOCAASDK final : public vendor::zwo::ZWOCAASDK {
public:
    using Info = vendor::zwo::ZWOCAARotatorInfo;

    int id{7};
    std::string serial{"CAA-SN-1"};
    // The logical (reverse-applied) angle CAAGetDegree() reports, as in the driver.
    double degree{0.0};
    double max_degree{360.0};
    bool reverse{false};
    bool fail_info{false};
    int open_calls{0};
    int close_calls{0};
    double last_move_absolute{-1.0};
    double last_move_mechanical{-1.0};
    int motion_calls{0};
    int degree_calls{0};
    int reverse_calls{0};
    bool fail_reads{false};
    bool moving{false};
    // The next open_rotator() throws (a connect that must fail).
    bool fail_open{false};
    // open_rotator() sleeps this long before returning (a connect held open).
    std::chrono::milliseconds open_delay{0};
    // get_motion_status() sleeps this long (a status refill held open); moves, stop() and set_reverse() sleep
    // move_delay.
    std::chrono::milliseconds read_delay{0};
    std::chrono::milliseconds move_delay{0};
    std::atomic<bool> open_entered{false};
    std::atomic<bool> read_entered{false};
    std::atomic<bool> move_entered{false};
    std::atomic<int> busy{0};
    std::atomic<bool> closed_while_busy{false};

    std::vector<Info> enumerate_rotators() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return {Info{id, "CAA", max_degree}};
    }
    bool get_rotator_info_by_id(int rotator_id, Info& info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_info || rotator_id != id) {
            return false;
        }
        info = Info{id, "CAA", max_degree};
        return true;
    }
    bool get_rotator_info_by_index(int, Info& info) override { return get_rotator_info_by_id(id, info); }
    void open_rotator(int) override {
        const InFlight in_flight(*this, open_delay, open_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
    }
    void close_rotator(int) override {
        if (busy.load() > 0) closed_while_busy = true;
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls;
    }
    vendor::zwo::ZWOCAAMotionStatus get_motion_status(int) override {
        const InFlight in_flight(*this, read_delay, read_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        ++motion_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return {moving, false};
    }
    double get_degree(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++degree_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return degree;
    }
    void move_relative(int, double angle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        degree += angle;
    }
    void move_absolute(int, double angle) override {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        last_move_absolute = angle;
        degree = angle;
    }
    void move_mechanical(int, double angle) override {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        last_move_mechanical = angle;
        degree = reverse ? 360.0 - angle : angle;
    }
    void stop(int) override {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        moving = false;
    }
    void sync_degree(int, double angle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        degree = angle;
    }
    double get_max_degree(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_degree;
    }
    double get_temperature(int) override { return 20.0; }
    bool get_reverse(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++reverse_calls;
        return reverse;
    }
    void set_reverse(int, bool value) override {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        reverse = value;
    }
    std::string get_serial_number(int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return serial;
    }
    std::string get_firmware_version(int) override { return "1.0"; }
    std::string get_rotator_type(int) override { return "CAA"; }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    // Marks a call in flight for its whole duration, sleeping `delay` first, outside mutex_.
    struct InFlight {
        InFlight(FakeZWOCAASDK& fake, std::chrono::milliseconds delay, std::atomic<bool>& entered) : fake_(fake) {
            ++fake_.busy;
            entered = true;
            if (delay.count() > 0) std::this_thread::sleep_for(delay);
        }
        ~InFlight() { --fake_.busy; }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        FakeZWOCAASDK& fake_;
    };

    mutable std::mutex mutex_;
};

}  // namespace alpacacore::test

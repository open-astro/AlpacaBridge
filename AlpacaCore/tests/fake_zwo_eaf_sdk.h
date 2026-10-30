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
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted ZWO EAF SDK: one focuser (id 3), shared by the focuser unit tests, the
// connected stress case and the contract sweep. second_id >= 0 adds a second focuser
// (serial second_serial) to enumeration, open and the serial read.
// An empty serial makes the serial read fail as a unit with no serial does.
// The fake models only the library: raw per-id units behind the product's SharedOpenTable
// (issue #1500), composed as the real wrapper composes it, so opens are shared and checked by
// serial, and a per-device call outside with_link() is refused. A per-device call on an id the
// units do not hold fails as EAF_ERROR_INVALID_ID does through the wrapper (NotConnected).
// Every raw call runs under one internal mutex, so the fake is race-free under a
// concurrent storm (the ZWO driver tests run it through lifecycle stress and TSan);
// the public knobs and counters are plain fields for single-threaded tests, set before
// the driver starts and read after it stops. No call blocks except the explicit
// delays: open_delay holds a connect open (contract sweep tier 2), read_delay holds an
// is_moving() read and move_delay a move() or stop() in flight (getters-never-wait tests).
// The *_entered flags and `busy` are atomics a test polls from another thread; close_focuser_link()
// sets closed_while_busy when a driver asks to close while a delayed call is still in flight.

// KNOWN PARITY GAPS: move() settles a move at once (position/degree jump to the target and
// the moving flag stays whatever a test set), where the real EAFIsMoving reports motion until the unit
// arrives. A test that needs a moving window sets `moving` itself.
class FakeZWOEAFSDK final : public vendor::zwo::ZWOEAFSDK {
public:
    using Info = vendor::zwo::ZWOEAFFocuserInfo;

    int id{3};
    int slot_index{0};
    std::string serial{"EAF-SN-1"};
    // A second focuser on the same SDK (issue #1500 id swap); -1 = none.
    int second_id{-1};
    std::string second_serial{"EAF-SN-2"};
    int max_step{60000};
    int step_range{1000};
    int position{500};
    // Atomic with fail_reads: the contract sweep sets and clears them while the driver polls (tier 3).
    std::atomic<bool> moving{false};
    double temperature{12.5};
    std::atomic<bool> fail_reads{false};
    bool fail_temperature{false};
    // Issue #1500: the temperature or step range read reports the unit removed (EAF_ERROR_REMOVED).
    bool temperature_removed{false};
    bool step_range_removed{false};
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
    // Every real open throws while set (a connect that must fail).
    bool fail_open{false};
    // A real open sleeps this long before returning (a connect held open).
    std::chrono::milliseconds open_delay{0};
    // is_moving() sleeps this long (a status refill held open); move()/stop() sleep move_delay.
    std::chrono::milliseconds read_delay{0};
    std::chrono::milliseconds move_delay{0};
    std::atomic<bool> open_entered{false};
    std::atomic<bool> read_entered{false};
    std::atomic<bool> move_entered{false};
    std::atomic<int> busy{0};
    std::atomic<bool> closed_while_busy{false};

    // Issue #1500: a USB drop. remove() makes the open and every per-device call throw what the wrapper throws
    // for EAF_ERROR_REMOVED (NotConnected) and the focusers vanish from enumeration; replug(id) brings this one
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
        const auto unchecked = table_.lock_unchecked();
        std::lock_guard<std::mutex> lock(mutex_);
        if (removed) return {};
        std::vector<Info> focusers{Info{id, "EAF", max_step, slot_index}};
        if (second_id >= 0)
            focusers.insert(second_id < id ? focusers.begin() : focusers.end(), Info{second_id, "EAF", max_step});
        if (second_id >= 0) {
            for (std::size_t i = 0; i < focusers.size(); ++i) focusers[i].slot_index = static_cast<int>(i);
        }
        return focusers;
    }
    bool get_focuser_info_by_id(int focuser_id, Info& info) override {
        const auto unchecked = table_.lock_unchecked();
        std::lock_guard<std::mutex> lock(mutex_);
        if (removed || (focuser_id != id && focuser_id != second_id)) {
            return false;
        }
        info = Info{focuser_id, "EAF", max_step};
        return true;
    }
    bool get_focuser_info_by_index(int index, Info& info) override {
        const auto focusers = enumerate_focusers();
        for (const auto& entry : focusers) {
            if (entry.slot_index == index) {
                info = entry;
                return true;
            }
        }
        return false;
    }
    vendor::zwo::SharedOpenTable::Opened open_focuser_link(int focuser_id) override { return table_.open(focuser_id); }
    std::optional<vendor::zwo::SharedOpenTable::Opened> open_focuser_candidate(int focuser_id) override {
        return table_.open_candidate(focuser_id);
    }
    void close_focuser_link(int focuser_id, std::uint64_t link) override {
        // Sampled before the table: the table mutex would hold the close back until the call in flight ends.
        if (busy.load() > 0) closed_while_busy = true;
        table_.close(focuser_id, link);
    }
    void with_link(int focuser_id, std::uint64_t link, const std::function<void()>& fn) override {
        table_.with_link(focuser_id, link, fn);
    }
    // Per-device calls: the table checks the caller's link, then the raw unit answers in the same hold.
    bool is_moving(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_is_moving(focuser_id); });
    }
    int get_position(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_position(focuser_id); });
    }
    void move(int focuser_id, int target) override {
        table_.call(focuser_id, [&] { raw_move(focuser_id, target); });
    }
    void stop(int focuser_id) override {
        table_.call(focuser_id, [&] { raw_stop(focuser_id); });
    }
    int get_max_step(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_max_step(focuser_id); });
    }
    void set_max_step(int focuser_id, int value) override {
        table_.call(focuser_id, [&] { raw_set_max_step(focuser_id, value); });
    }
    int get_step_range(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_step_range(focuser_id); });
    }
    double get_temperature(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_temperature(focuser_id); });
    }
    std::string get_serial_number(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_serial(focuser_id); });
    }
    std::string get_firmware_version(int focuser_id) override {
        return table_.call(focuser_id, [&] { return raw_firmware(focuser_id); });
    }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    // The library under the table: EAFOpen, EAFClose and the per-device calls on the raw units.
    void raw_open(int focuser_id) {
        const InFlight in_flight(*this, open_delay, open_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_removed();
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
        last_open_id = focuser_id;
    }
    void raw_close(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls;
        // The table erases its bookkeeping, then EAFClose fails on a removed unit or a vanished id.
        check_unit(focuser_id);
    }
    bool raw_is_moving(int focuser_id) {
        const InFlight in_flight(*this, read_delay, read_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        ++moving_calls;
        check_unit(focuser_id);
        if (fail_reads) {
            throw AlpacaException("link down", AlpacaError::DriverException);
        }
        return moving;
    }
    int raw_position(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++position_calls;
        last_read_id = focuser_id;
        check_unit(focuser_id);
        if (fail_reads) {
            throw AlpacaException("link down", AlpacaError::DriverException);
        }
        return position;
    }
    void raw_move(int focuser_id, int target) {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        check_unit(focuser_id);
        last_move_id = focuser_id;
        position = target;
    }
    void raw_stop(int focuser_id) {
        const InFlight in_flight(*this, move_delay, move_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        check_unit(focuser_id);
        moving = false;
    }
    int raw_max_step(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++max_step_calls;
        check_unit(focuser_id);
        return max_step;
    }
    void raw_set_max_step(int focuser_id, int value) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++set_max_step_calls;
        check_unit(focuser_id);
        last_set_max_step = value;
        max_step = value;  // the unit stores what it is given
    }
    int raw_step_range(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++step_range_calls;
        check_unit(focuser_id);
        if (step_range_removed) throw AlpacaException("EAFStepRange: EAF_ERROR_REMOVED", AlpacaError::NotConnected);
        if (step_range_fails) {
            throw std::runtime_error("step range unavailable");
        }
        return step_range;
    }
    double raw_temperature(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++temperature_calls;
        check_unit(focuser_id);
        if (temperature_removed) throw AlpacaException("EAFGetTemp: EAF_ERROR_REMOVED", AlpacaError::NotConnected);
        if (temperature_not_supported) {
            throw AlpacaException("EAF temperature not supported", AlpacaError::NotImplemented);
        }
        if (fail_temperature) {
            throw std::runtime_error("temp sensor down");
        }
        return temperature;
    }
    std::string raw_serial(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_unit(focuser_id);
        const std::string& unit_serial = focuser_id == second_id ? second_serial : serial;
        // An empty serial is a unit that reports none (TODO(#1500): confirm on the rig which code an EAF
        // without a serial returns; the wrapper maps EAF_ERROR_NOT_SUPPORTED to NotImplemented).
        if (unit_serial.empty()) {
            throw AlpacaException("EAFGetSerialNumber: EAF_ERROR_NOT_SUPPORTED", AlpacaError::NotImplemented);
        }
        return unit_serial;
    }
    std::string raw_firmware(int focuser_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_unit(focuser_id);
        return "1.0";
    }

    void throw_if_removed() const {
        if (removed) throw AlpacaException("EAF_ERROR_REMOVED", AlpacaError::NotConnected);
    }
    // A per-device call on an id the library does not hold: EAF_ERROR_INVALID_ID, which the wrapper maps to
    // NotConnected on a per-device call (issue #1500 decision 7: the bridge passes only enumerated ids).
    void check_unit(int focuser_id) const {
        throw_if_removed();
        if (focuser_id != id && focuser_id != second_id) {
            throw AlpacaException("EAF_ERROR_INVALID_ID", AlpacaError::NotConnected);
        }
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
    // The product's open bookkeeping over the raw units, as the real wrapper composes it over the SDK.
    vendor::zwo::SharedOpenTable table_{vendor::zwo::SharedOpenTable::Ops{
        [this](int focuser_id) { raw_open(focuser_id); },
        [this](int focuser_id) { raw_close(focuser_id); },
        [this](int focuser_id) { return raw_serial(focuser_id); },
    }};
};

}  // namespace alpacacore::test

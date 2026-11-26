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
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "connect_hold_gate.h"

namespace alpacacore::test {

// Scripted ZWO CAA SDK: one rotator (id 7), shared by the rotator unit tests, the
// connected stress case and the contract sweep. second_id >= 0 adds a second rotator
// (serial second_serial) to enumeration, open and the serial read.
// The fake models only the library: raw per-id units behind the product's SharedOpenTable
// (issue #1668), composed as the real wrapper composes it, so opens are shared and checked by
// serial, and a per-device call outside with_link() is refused. A per-device call on an id the
// units do not hold fails as CAA_ERROR_INVALID_ID does through the wrapper (NotConnected).
// Every raw call runs under one internal mutex, so the fake is race-free under a
// concurrent storm (the ZWO driver tests run it through lifecycle stress and TSan);
// the public knobs and counters are plain fields for single-threaded tests, set before
// the driver starts and read after it stops. No call blocks except the explicit
// delays: open_delay holds a connect open (contract sweep tier 2), read_delay holds a
// get_motion_status() read and move_delay a move, stop or set_reverse in flight (getters-never-wait tests).
// The *_entered flags and `busy` are atomics a test polls from another thread; close_rotator_link()
// sets closed_while_busy when it runs while a delayed call is still in flight.

// KNOWN PARITY GAPS: a move puts the position/degree at the target at once, where the real CAAGetDegree
// follows the unit as it turns. The moving flag stays whatever a test set unless `moving_reads` scripts the window:
// the real CAAIsMoving reports motion until the unit arrives, which moving_reads counts in reads of
// get_motion_status() (as the EFW fake does for its wheel). At 0 (the default) a move is over at once.
class FakeZWOCAASDK final : public vendor::zwo::ZWOCAASDK {
public:
    using Info = vendor::zwo::ZWOCAARotatorInfo;

    int id{7};
    // The unit's EAF_INFO / EFW_INFO / CAA_INFO name; the SDK reports "CAA".
    std::string name{"CAA"};
    int slot_index{0};
    // What get_firmware_version() answers; an empty string is an unreadable one. fail_firmware makes it throw.
    std::string firmware{"1.0"};
    bool fail_firmware{false};
    std::atomic<int> firmware_calls{0};
    std::string serial{"CAA-SN-1"};
    // A second rotator on the same SDK (issue #1668 id swap); -1 = none.
    int second_id{-1};
    std::string second_serial{"CAA-SN-2"};
    // The logical (reverse-applied) angle CAAGetDegree() reports, as in the driver.
    double degree{0.0};
    double max_degree{360.0};
    bool reverse{false};
    bool fail_info{false};
    int open_calls{0};
    int last_open_id{-1};
    int close_calls{0};
    double last_move_absolute{-1.0};
    double last_move_mechanical{-1.0};
    // The SDK id of the last degree read and of the last move (issue #1668: which rotator a driver talks to).
    int last_read_id{-1};
    int last_move_id{-1};
    int motion_calls{0};
    int degree_calls{0};
    int reverse_calls{0};
    // Moves of any kind the rotator accepted (absolute, relative, mechanical); a Connect must send none.
    int move_calls{0};
    // Atomic: the contract sweep sets and clears them while the driver polls (tier 3).
    std::atomic<bool> fail_reads{false};
    std::atomic<bool> moving{false};
    // A move (absolute, relative or mechanical) reports moving for this many get_motion_status() reads, then the
    // unit has arrived; stop() ends the window.
    int moving_reads{0};
    // Every real open throws while set (a connect that must fail).
    bool fail_open{false};
    // A real open sleeps this long before returning (a connect held open).
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
    // Issue #1668: a USB drop. remove() makes the open and every per-device call throw what the wrapper throws
    // for CAA_ERROR_REMOVED (NotConnected) and the rotators vanish from enumeration; replug(id) brings this one
    // back under a new SDK id.
    bool removed{false};

    void remove() {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = true;
    }
    void replug(int new_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = false;
        id = new_id;
    }
    /// Holders of the table's open for @p rotator_id.
    std::size_t holders(int rotator_id) const { return table_.open_count(rotator_id); }

    std::vector<Info> enumerate_rotators() override {
        const auto unchecked = table_.lock_unchecked();
        std::lock_guard<std::mutex> lock(mutex_);
        if (removed) return {};
        std::vector<Info> rotators{Info{id, name, max_degree, slot_index}};
        if (second_id >= 0) {
            rotators.insert(second_id < id ? rotators.begin() : rotators.end(), Info{second_id, name, max_degree});
            for (std::size_t i = 0; i < rotators.size(); ++i) rotators[i].slot_index = static_cast<int>(i);
        }
        return rotators;
    }
    bool get_rotator_info_by_id(int rotator_id, Info& info) override {
        const auto unchecked = table_.lock_unchecked();
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_info || removed || (rotator_id != id && rotator_id != second_id)) {
            return false;
        }
        info = Info{rotator_id, name, max_degree};
        return true;
    }
    bool get_rotator_info_by_index(int index, Info& info) override {
        for (const auto& entry : enumerate_rotators()) {
            if (entry.slot_index == index) {
                if (!get_rotator_info_by_id(entry.rotator_id, info)) return false;
                info.slot_index = index;
                return true;
            }
        }
        return false;
    }
    vendor::zwo::SharedOpenTable::Opened open_rotator_link(int rotator_id) override { return table_.open(rotator_id); }
    std::optional<vendor::zwo::SharedOpenTable::Opened> open_rotator_candidate(int rotator_id) override {
        return table_.open_candidate(rotator_id);
    }
    void close_rotator_link(int rotator_id, std::uint64_t link) override {
        // Sampled before the table: the table mutex would hold the close back until the call in flight ends.
        if (busy.load() > 0) closed_while_busy = true;
        table_.close(rotator_id, link);
    }
    void with_link(int rotator_id, std::uint64_t link, const std::function<void()>& fn) override {
        table_.with_link(rotator_id, link, fn);
    }
    // Per-device calls: the table checks the caller's link, then the raw unit answers in the same hold.
    vendor::zwo::ZWOCAAMotionStatus get_motion_status(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            const InFlight in_flight(*this, read_delay, read_entered);
            std::lock_guard<std::mutex> lock(mutex_);
            ++motion_calls;
            check_unit(rotator_id);
            if (fail_reads) {
                throw AlpacaException("link down", AlpacaError::DriverException);
            }
            const bool in_move = moving_left_ > 0;
            if (in_move) --moving_left_;
            return vendor::zwo::ZWOCAAMotionStatus{moving || in_move, false};
        });
    }
    double get_degree(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            ++degree_calls;
            last_read_id = rotator_id;
            check_unit(rotator_id);
            if (fail_reads) {
                throw AlpacaException("link down", AlpacaError::DriverException);
            }
            return degree;
        });
    }
    void move_relative(int rotator_id, double angle) override {
        table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            ++move_calls;
            last_move_id = rotator_id;
            degree += angle;
            moving_left_ = moving_reads;
        });
    }
    void move_absolute(int rotator_id, double angle) override {
        table_.call(rotator_id, [&] {
            const InFlight in_flight(*this, move_delay, move_entered);
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            ++move_calls;
            last_move_id = rotator_id;
            last_move_absolute = angle;
            degree = angle;
            moving_left_ = moving_reads;
        });
    }
    void move_mechanical(int rotator_id, double angle) override {
        table_.call(rotator_id, [&] {
            const InFlight in_flight(*this, move_delay, move_entered);
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            ++move_calls;
            last_move_id = rotator_id;
            last_move_mechanical = angle;
            degree = reverse ? 360.0 - angle : angle;
            moving_left_ = moving_reads;
        });
    }
    void stop(int rotator_id) override {
        table_.call(rotator_id, [&] {
            const InFlight in_flight(*this, move_delay, move_entered);
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            moving = false;
            moving_left_ = 0;
        });
    }
    void sync_degree(int rotator_id, double angle) override {
        table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            degree = angle;
        });
    }
    double get_max_degree(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            return max_degree;
        });
    }
    double get_temperature(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            return 20.0;
        });
    }
    bool get_reverse(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            ++reverse_calls;
            check_unit(rotator_id);
            return reverse;
        });
    }
    void set_reverse(int rotator_id, bool value) override {
        table_.call(rotator_id, [&] {
            const InFlight in_flight(*this, move_delay, move_entered);
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            reverse = value;
        });
    }
    std::string get_serial_number(int rotator_id) override {
        return table_.call(rotator_id, [&] { return raw_serial(rotator_id); });
    }
    std::string get_firmware_version(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            ++firmware_calls;
            if (fail_firmware) throw AlpacaException("firmware read failed", AlpacaError::DriverException);
            return firmware;
        });
    }
    std::string get_rotator_type(int rotator_id) override {
        return table_.call(rotator_id, [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            check_unit(rotator_id);
            return std::string("CAA");
        });
    }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }

private:
    // The library under the table: CAAOpen, CAAClose and the serial read on the raw units.
    void raw_open(int rotator_id) {
        const InFlight in_flight(*this, open_delay, open_entered);
        std::lock_guard<std::mutex> lock(mutex_);
        if (removed) throw AlpacaException("CAAOpen: CAA_ERROR_REMOVED", AlpacaError::NotConnected);
        if (rotator_id != id && rotator_id != second_id) {
            // CAAOpen of an id it does not know: CAA_ERROR_INVALID_ID, InvalidValue through the wrapper's open.
            throw AlpacaException("CAAOpen: CAA_ERROR_INVALID_ID", AlpacaError::InvalidValue);
        }
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        ++open_calls;
        last_open_id = rotator_id;
    }
    void raw_close(int rotator_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls;
        // The table erases its bookkeeping, then CAAClose fails on a removed rotator or a vanished id.
        check_unit(rotator_id);
    }
    std::string raw_serial(int rotator_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_unit(rotator_id);
        const std::string& unit_serial = rotator_id == second_id ? second_serial : serial;
        // An empty serial is a rotator that reports none (the wrapper maps CAA_ERROR_NOT_SUPPORTED to NotImplemented).
        if (unit_serial.empty()) {
            throw AlpacaException("CAAGetSerialNumber: CAA_ERROR_NOT_SUPPORTED", AlpacaError::NotImplemented);
        }
        return unit_serial;
    }

    // A per-device call on an id the library does not hold: CAA_ERROR_INVALID_ID, which the wrapper maps to
    // NotConnected on a per-device call (issue #1668, as the EAF: the bridge passes only enumerated ids).
    void check_unit(int rotator_id) const {
        if (removed) throw AlpacaException("CAA_ERROR_REMOVED", AlpacaError::NotConnected);
        if (rotator_id != id && rotator_id != second_id) {
            throw AlpacaException("CAA_ERROR_INVALID_ID", AlpacaError::NotConnected);
        }
    }

    int moving_left_{0};  // reads of get_motion_status() still to report moving; under mutex_

    // Marks a call in flight for its whole duration, sleeping `delay` first, outside mutex_.
    struct InFlight {
        InFlight(FakeZWOCAASDK& fake, std::chrono::milliseconds delay, std::atomic<bool>& entered) : fake_(fake) {
            ++fake_.busy;
            entered = true;
            alpacacore::test::connect_hold(delay);  // connect_hold_gate.h
        }
        ~InFlight() { --fake_.busy; }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        FakeZWOCAASDK& fake_;
    };

    mutable std::mutex mutex_;
    // The product's open bookkeeping over the raw units, as the real wrapper composes it over the SDK.
    vendor::zwo::SharedOpenTable table_{vendor::zwo::SharedOpenTable::Ops{
        [this](int rotator_id) { raw_open(rotator_id); },
        [this](int rotator_id) { raw_close(rotator_id); },
        [this](int rotator_id) { return raw_serial(rotator_id); },
    }};
};

}  // namespace alpacacore::test

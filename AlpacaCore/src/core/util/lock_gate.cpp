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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/lock_gate.h>
#include <alpacacore/util/logging.h>

#include <array>
#include <utility>

namespace alpacacore::util {

LockGate::LockGate(std::mutex& io_mutex, std::mutex& state_mutex, std::string log_tag)
    : io_mutex_(io_mutex), state_mutex_(state_mutex), log_tag_(std::move(log_tag)) {}

LockGate::HeldLocks* LockGate::lookup(const LockGate* gate, bool claim) {
    // One table per thread, shared by every gate: a slot records which of one gate's locks this thread
    // holds, and is free again once it records none.
    thread_local std::array<HeldLocks, kLockGateTableSize> table{};
    HeldLocks* free_slot = nullptr;
    for (auto& slot : table) {
        if (slot.gate == gate) {
            return &slot;
        }
        if (slot.gate == nullptr && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    if (claim && free_slot != nullptr) {
        free_slot->gate = gate;
        return free_slot;
    }
    return nullptr;
}

LockGate::HeldLocks* LockGate::find_held() const { return lookup(this, false); }

LockGate::HeldLocks* LockGate::claim_held() const {
    HeldLocks* slot = lookup(this, true);
    if (slot == nullptr) {
        // More gates locked at once on this thread than the table holds. Never expected. Without a slot an
        // exchange cannot see the locks this thread holds: it would keep the state lock held across the
        // exchange, or take the operation lock again and deadlock this thread. Refuse before taking any lock
        // (decision 0016 Rule 6).
        ALPACA_LOG_ERROR(log_tag_, "Lock table full on this thread; refusing the call");
        throw AlpacaException(log_tag_ + " driver lock table full on this thread", AlpacaError::DriverException);
    }
    return slot;
}

void LockGate::release_held(HeldLocks* slot) {
    if (slot != nullptr && !slot->state && !slot->io) {
        slot->gate = nullptr;
    }
}

bool LockGate::holds_io() const {
    const HeldLocks* held = find_held();
    return held != nullptr && held->io;
}

bool LockGate::holds_state() const {
    const HeldLocks* held = find_held();
    return held != nullptr && held->state;
}

StateLock::StateLock(const LockGate& gate, Mode mode) : gate_(&gate), mode_(mode) { lock(); }

StateLock::StateLock(StateLock&& other) noexcept : gate_(other.gate_), mode_(other.mode_), owned_(other.owned_) {
    other.owned_ = false;
}

StateLock::~StateLock() {
    if (owned_) {
        unlock();
    }
}

void StateLock::lock() {
    LockGate::HeldLocks* held = gate_->claim_held();
    if (mode_ == Mode::Io) {
        gate_->io_mutex_.lock();
        held->io = true;
    }
    gate_->state_mutex_.lock();
    held->state = true;
    owned_ = true;
}

void StateLock::unlock() {
    LockGate::HeldLocks* held = gate_->find_held();
    if (held != nullptr) {
        held->state = false;
    }
    gate_->state_mutex_.unlock();
    if (mode_ == Mode::Io) {
        if (held != nullptr) {
            held->io = false;
        }
        gate_->io_mutex_.unlock();
    }
    LockGate::release_held(held);
    owned_ = false;
}

}  // namespace alpacacore::util

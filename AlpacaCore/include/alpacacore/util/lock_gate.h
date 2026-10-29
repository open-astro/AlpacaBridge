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

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace alpacacore::util {

/// Slots in each thread's held-lock table: how many gates one thread may hold
/// locks of at once.
inline constexpr std::size_t kLockGateTableSize = 8;

/**
 * @brief The decision 0016 lock gate of a converted driver: its operation lock
 *        (`io_mutex_`) and its state lock (`mutex_`), and which of them this
 *        thread holds (#1729).
 *
 * Lock order: the operation lock, then the state lock (a leaf). A command
 * holds both through a StateLock (Mode::Io) for the whole command; a getter
 * holds the state lock alone (Mode::State) and makes no device exchange.
 * Every device exchange runs inside exchange(), under the operation lock,
 * which drops the state lock for the exchange, so the state lock is never held
 * across device I/O.
 *
 * Which locks a thread holds is kept per gate in a thread-local table of
 * kLockGateTableSize slots, so an exchange called from a helper that was
 * handed no lock object still knows what to drop and take back. When the
 * table is full a StateLock refuses with DriverException
 * before it takes any lock (decision 0016 item 8, Rule 6): without a slot the gate cannot see what the
 * thread holds, so an exchange would either keep the state lock held or take
 * the operation lock a second time.
 *
 * The gate owns no mutex: it refers to the driver's two, which must outlive
 * it. It is a member the driver composes, not a base class (decision 0013,
 * point 5). What stays in the driver: the transport an exchange reaches and
 * any counter it compares across an exchange (see the driver lifecycle
 * instructions).
 */
class LockGate {
public:
    LockGate(std::mutex& io_mutex, std::mutex& state_mutex, std::string log_tag);
    LockGate(const LockGate&) = delete;
    LockGate& operator=(const LockGate&) = delete;
    LockGate(LockGate&&) = delete;
    LockGate& operator=(LockGate&&) = delete;
    ~LockGate() = default;

    /// True when this thread holds the operation lock through this gate (a
    /// Mode::Io StateLock).
    bool holds_io() const;

    /// True when this thread holds the state lock through this gate.
    bool holds_state() const;

    /// Runs `f()` as one exchange. A thread that holds the state lock gives it
    /// up for the call and takes it back after, also when `f` throws; the
    /// operation lock it must then hold stays held. A thread that holds no
    /// state lock calls straight through.
    template <typename F>
    decltype(auto) exchange(F&& f) const {
        HeldLocks* held = find_held();
        if (held == nullptr || !held->state) {
            return std::forward<F>(f)();
        }
        // Dropping the state lock is only safe under the operation lock: without it a second command
        // could interleave with this exchange.
        assert(held->io && "LockGate::exchange dropped the state lock on a thread without the operation lock");
        held->state = false;
        state_mutex_.unlock();
        const RestoreState restore{*this, held};
        return std::forward<F>(f)();
    }

private:
    friend class StateLock;

    struct HeldLocks {
        const LockGate* gate = nullptr;
        bool state = false;
        bool io = false;
    };

    // This thread's slot for `gate`; with `claim`, a free slot is claimed when it has none. nullptr when it
    // has none (and, with `claim`, the table is full).
    static HeldLocks* lookup(const LockGate* gate, bool claim);
    // This thread's slot for this gate, or nullptr when it holds none of its locks.
    HeldLocks* find_held() const;
    // This thread's slot for this gate, claimed when it has none; throws DriverException when the
    // table is full.
    HeldLocks* claim_held() const;
    // Frees the slot when it records no lock.
    static void release_held(HeldLocks* slot);

    // Takes the state lock back when an exchange() ends, also by a throw.
    class RestoreState {
    public:
        RestoreState(const LockGate& gate, HeldLocks* held) : gate_(gate), held_(held) {}
        RestoreState(const RestoreState&) = delete;
        RestoreState& operator=(const RestoreState&) = delete;
        ~RestoreState() {
            gate_.state_mutex_.lock();
            held_->state = true;
        }

    private:
        const LockGate& gate_;
        HeldLocks* held_;
    };

    std::mutex& io_mutex_;
    std::mutex& state_mutex_;
    std::string log_tag_;
};

/**
 * @brief A driver's state lock, with the operation lock above it in Mode::Io.
 *
 * unique_lock-shaped: lock(), unlock() (releases both) and owns_lock() mean
 * what they mean on a unique_lock of the state lock, so a release point (a
 * wait, a join, a slot start) lets every other command in. Mode::State is for
 * members that take no operation lock (the getters); they never reach the
 * device.
 */
class StateLock {
public:
    enum class Mode : std::uint8_t { State, Io };

    /// Locks at once; throws DriverException, holding nothing, when this
    /// thread's table is full.
    explicit StateLock(const LockGate& gate, Mode mode = Mode::Io);
    StateLock(StateLock&& other) noexcept;
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
    StateLock& operator=(StateLock&&) = delete;
    ~StateLock();

    /// Takes the operation lock (Mode::Io), then the state lock. Throws
    /// DriverException before it takes either when this thread's table is full.
    void lock();
    void unlock();
    bool owns_lock() const { return owned_; }

private:
    const LockGate* gate_;
    Mode mode_;
    bool owned_ = false;
};

}  // namespace alpacacore::util

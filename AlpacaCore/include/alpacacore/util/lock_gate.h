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
 * holds the state lock alone (Mode::State). Every device exchange runs inside
 * exchange() or single_read(), which drop the state lock for the exchange, so
 * the state lock is never held across device I/O.
 *
 * Which locks a thread holds is kept per gate in a thread-local table of
 * kLockGateTableSize slots, so an exchange called from a helper that was
 * handed no lock object still knows what to drop and take back. When the
 * table is full a StateLock or single_read() refuses with DriverException
 * before it takes any lock (decision 0016 item 8, Rule 6): without a slot the gate cannot see what the
 * thread holds, so an exchange would either keep the state lock held or take
 * the operation lock a second time.
 *
 * The gate owns no mutex: it refers to the driver's two, which must outlive
 * it. It is a member the driver composes, not a base class (decision 0013,
 * point 5). What stays in the driver: the transport an exchange reaches, a
 * session re-check under the operation lock, and any counter it compares
 * across an exchange (see the driver lifecycle instructions).
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
    /// Mode::Io StateLock, or inside a single_read()).
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

    /// Runs `f()` as one exchange from a thread that does not hold the
    /// operation lock (a getter whose cache is stale): drops the state lock if
    /// held, calls `before_io()` with no lock of this gate held, takes the
    /// operation lock and marks it held for the call, so a nested exchange()
    /// or holds_io() inside `f` sees it. After `f`, also when it or
    /// `before_io` throws, the state lock is taken back before the operation
    /// lock is released, so the reply is stored before another command's
    /// exchange can land. Throws DriverException, before it drops or takes
    /// any lock, when this thread's table is full.
    template <typename BeforeIo, typename F>
    decltype(auto) single_read(BeforeIo&& before_io, F&& f) const {
        HeldLocks* held = find_held();
        assert((held == nullptr || !held->io) && "LockGate::single_read on a thread that holds the operation lock");
        HeldLocks* slot = held != nullptr ? held : claim_held();
        SingleRead read{*this, slot, held != nullptr && held->state};
        std::forward<BeforeIo>(before_io)();
        read.take_io();
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

    // The single read's locks, unwound in the order single_read() documents.
    class SingleRead {
    public:
        SingleRead(const LockGate& gate, HeldLocks* slot, bool had_state)
            : gate_(gate), slot_(slot), had_state_(had_state) {
            if (had_state_) {
                slot_->state = false;
                gate_.state_mutex_.unlock();
            }
        }
        SingleRead(const SingleRead&) = delete;
        SingleRead& operator=(const SingleRead&) = delete;
        ~SingleRead() {
            if (had_state_) {
                gate_.state_mutex_.lock();
                slot_->state = true;
            }
            if (io_taken_) {
                slot_->io = false;
            }
            release_held(slot_);
            if (io_taken_) {
                gate_.io_mutex_.unlock();
            }
        }
        void take_io() {
            gate_.io_mutex_.lock();
            slot_->io = true;
            io_taken_ = true;
        }

    private:
        const LockGate& gate_;
        HeldLocks* slot_;
        bool had_state_;
        bool io_taken_ = false;
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
 * members that take no operation lock (the getters); they reach the device
 * only through LockGate::single_read().
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

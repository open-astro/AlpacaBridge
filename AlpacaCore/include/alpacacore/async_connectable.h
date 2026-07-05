// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <alpacacore/util/logging.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace alpacacore {

/**
 * Async connection-thread lifecycle, extracted once (issue #100).
 *
 * This is the battle-tested protocol from the ToupTek camera driver (the
 * PR #99 review marathon): every rule below exists because its absence was a
 * shipped race. Drivers inherit this as a mixin next to their device base:
 *
 *     class FooDriver : public CameraDriver, protected AsyncConnectable { ... };
 *
 * The driver's existing `set_connected(bool)` / `get_connected()` overrides
 * satisfy this class's pure virtuals — no extra methods to write.
 *
 * What the base guarantees:
 * - `connect()`/`disconnect()` forward to start_connection_task(), which NEVER
 *   spawns a thread after shutdown began (a connect racing the destructor
 *   would otherwise leave an unjoined thread -> std::terminate).
 * - A disconnect racing an in-flight connect is NEVER dropped: it is recorded
 *   (pending_disconnect_) and either consumed by the connect at entry (newer
 *   request wins, camera stays disconnected) or run by the task's tail.
 *   DELIBERATELY ASYMMETRIC: a connect racing an in-flight DISCONNECT is
 *   dropped (early return, no recording). A dropped disconnect leaves
 *   hardware energized unattended — a safety problem the user cannot see;
 *   a dropped connect leaves the device visibly disconnected, which the
 *   client observes immediately (Connected stays false) and simply retries.
 *   Mirroring the flag for connects would double the tail/entry protocol
 *   surface for a case with a trivial user-side recovery.
 * - conn_task_ only transitions to Idle inside the task's tail UNDER
 *   connection_mutex_ — the same lock every start_connection_task holds — so
 *   a recorder can never misjudge the in-flight state (the double-read race).
 *   Idle is published BEFORE the tail's deferred disconnect runs, so the
 *   deferred call can't be mistaken for another racing disconnect
 *   (self-block: device stays Connected, stale flag no-ops the next connect).
 *   COST: the deferred disconnect runs while connection_mutex_ is held, so a
 *   concurrent connect()/disconnect() blocks for the full hardware teardown.
 *   Deliberate — correctness over latency; it only bites on the rare
 *   disconnect-races-connect path, but drivers with slow disconnects
 *   (long serial timeouts, USB re-enumeration) should know it's there.
 * - stop_connection_thread() joins OUTSIDE connection_mutex_ (the tail takes
 *   it; joining under it deadlocks with a finishing task).
 *
 * Driver obligations (each is one line, but they are contractual):
 * 1. DESTRUCTOR, first thing:  `shutdown_connection();`
 *    (sets shutting_down_ under the lock, then join — before any member the
 *    connection thread touches is destroyed).
 * 2. `connect()` / `disconnect()`:  `start_connection_task(true/false);`
 * 3. `get_connecting()`:  `return connection_task_active();`
 * 4. The SYNC set_connected(false) body, after taking the driver state mutex,
 *    where it reads `connected_`:
 *        if (record_disconnect_if_connect_in_flight(connected_.load())) return;
 * 5. The set_connected(true) body, same position:
 *        if (consume_pending_disconnect(connected_.load())) return;
 *    (newer disconnect wins; consumed only when this is a REAL connect —
 *    a no-op connect on an already-connected device must leave the flag
 *    for the task tail, or the recorded disconnect is silently lost)
 * Rules 4/5 keep the driver's own `connected_` reads under the driver's own
 * mutex; the base's pending flag has a dedicated leaf mutex (lock order:
 * driver mutex -> pending flag mutex; never the reverse).
 */
class AsyncConnectable {
public:
    /// True while a connect or disconnect task is in flight (get_connecting()).
    bool connection_task_active() const { return conn_task_.load() != kConnIdle; }

protected:
    explicit AsyncConnectable(std::string log_tag) : log_tag_(std::move(log_tag)) {}

    // Destructor intentionally does NOT stop the thread: by the time this
    // base subobject is destroyed the derived driver (whose set_connected the
    // thread calls) is already gone. The DERIVED destructor must call
    // shutdown_connection() first — see the class comment.
    virtual ~AsyncConnectable() = default;

    // Satisfied by the driver's existing AlpacaDriver overrides.
    virtual void set_connected(bool connected) = 0;
    virtual bool get_connected() const = 0;

    /// Spawn (or record intent against) an async connect/disconnect.
    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (shutting_down_) {
            return;  // Destruction in progress; never spawn a new thread.
        }
        const auto inflight = conn_task_.load();
        if (inflight != kConnIdle) {
            // An async disconnect racing an in-flight CONNECT must not be
            // dropped (the device would come up Connected despite the explicit
            // request). Record the intent; the connect consumes it at entry or
            // the task tail runs the disconnect.
            if (!connect && inflight == kConnConnect) {
                std::lock_guard<std::mutex> pending_lock(pending_mutex_);
                pending_disconnect_ = true;
            }
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();  // finished task; reap before reuse
        }
        conn_task_.store(connect ? kConnConnect : kConnDisconnect);
        try {
            connection_thread_ = std::thread(&AsyncConnectable::run_connection_task, this, connect);
        } catch (...) {
            // std::thread ctor can throw (e.g. OS thread limit). Roll back the
            // in-flight publish or conn_task_ stays non-Idle forever and every
            // future connect/disconnect becomes a no-op.
            conn_task_.store(kConnIdle);
            throw;
        }
    }

    /// Join a finished/running task. Never call while holding a lock the
    /// task body takes.
    void stop_connection_thread() {
        std::thread thread_to_join;
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            thread_to_join = std::move(connection_thread_);
        }
        if (thread_to_join.joinable()) {
            thread_to_join.join();
        }
    }

    /// Destructor helper — MUST be the derived destructor's first act:
    /// blocks new task spawns, then joins the in-flight one.
    void shutdown_connection() {
        {
            std::lock_guard<std::mutex> lock(connection_mutex_);
            shutting_down_ = true;
        }
        stop_connection_thread();
    }

    /// Sync-disconnect gate (driver obligation 4). Call with the driver state
    /// mutex held, passing the current connected_ value read under it.
    /// Returns true if the disconnect was recorded against an in-flight
    /// connect and the caller should return without touching hardware.
    ///
    /// conn_task_ is re-read INSIDE pending_mutex_, which pairs with the task
    /// tail publishing Idle BEFORE its pending-consume (also under
    /// pending_mutex_): either this recorder wins the pending lock first and
    /// the tail is guaranteed to see the flag, or the tail already consumed —
    /// in which case Idle was published before it released the lock, this
    /// re-read observes Idle, and we return false so the caller performs the
    /// disconnect synchronously itself. No stale-flag window (the round-4
    /// TOCTOU: check-then-record without the lock let a finishing tail slip
    /// between, leaving a flag no task would ever honor).
    bool record_disconnect_if_connect_in_flight(bool connected_now) {
        if (connected_now) {
            return false;
        }
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        if (conn_task_.load() != kConnConnect) {
            return false;
        }
        pending_disconnect_ = true;
        return true;
    }

    /// Unconditionally record a pending disconnect. For drivers with an extra
    /// sync-connect window the conn_task_ gate cannot see — e.g. a
    /// mutex-released homing poll inside a synchronous set_connected(true)
    /// (ToupTek AFW) — which must record the disconnect themselves and consume
    /// it when the window closes.
    void record_pending_disconnect() {
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        pending_disconnect_ = true;
    }

    /// Connect-entry gate (driver obligation 5). Call with the driver state
    /// mutex held, passing the current connected_ value read under it.
    /// Returns true if a newer disconnect was pending AND this is a real
    /// connect (device currently disconnected) — the caller must return
    /// without connecting.
    ///
    /// A NO-OP connect (device already connected — possible whenever a client
    /// calls Connect/Connected=true on a connected device) must NOT consume
    /// the flag: it performs no hardware transition, so eating the flag here
    /// silently loses the recorded disconnect (round-4 finding — the old
    /// iOptron spawn-skip masked this; the sync setter path never had a
    /// guard anywhere). Left un-consumed, the flag is honored by the task
    /// tail's deferred disconnect (async path) or by the next real
    /// transition, and the idempotency early-return keeps the no-op a no-op.
    bool consume_pending_disconnect(bool connected_now) {
        if (connected_now) {
            return false;
        }
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        if (pending_disconnect_) {
            pending_disconnect_ = false;
            return true;
        }
        return false;
    }

private:
    void run_connection_task(bool connect) {
        try {
            set_connected(connect);
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR(log_tag_, std::string("Connection task failed: ") + e.what());
        } catch (...) {
            // A non-std exception escaping a std::thread entry point calls
            // std::terminate. Every driver throws std:: exceptions today, but
            // this base is now the single chokepoint for 26 drivers' connect
            // paths — swallow-and-log rather than bet on that forever.
            ALPACA_LOG_ERROR(log_tag_, "Connection task failed: non-std exception");
        }
        // Tail under connection_mutex_: see the class comment for why the
        // Idle publish and the deferred-disconnect handoff must both
        // happen under this lock, in this order.
        std::lock_guard<std::mutex> conn_lock(connection_mutex_);
        // Read connected-now BEFORE taking pending_mutex_: get_connected() may
        // take the driver state mutex (Bisque/Celestron/SynScan/iOptron
        // telescopes), and the sync set_connected path takes driver mutex ->
        // pending_mutex_ (obligations 4/5) — calling it under pending_mutex_
        // is an ABBA deadlock. The read is a momentary snapshot either way
        // (the sync setter never held both locks at once), and a stale value
        // is benign: the deferred disconnect below is idempotent.
        const bool connected_now = connect && get_connected();
        // Publish Idle BEFORE the pending-consume. Paired with the recorder
        // re-reading conn_task_ under pending_mutex_, this closes the
        // stale-flag TOCTOU: a recorder that wins pending_mutex_ first is
        // guaranteed this consume sees its flag; a recorder that loses
        // observes Idle (published before this lock was released) and falls
        // back to disconnecting synchronously itself. Idle-before-deferred
        // also keeps the deferred set_connected(false) below from being
        // mistaken for another racing disconnect by the sync gate.
        conn_task_.store(kConnIdle);
        bool need_disconnect = false;
        {
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            if (pending_disconnect_) {
                pending_disconnect_ = false;
                need_disconnect = connected_now;
            }
        }
        if (need_disconnect) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR(log_tag_, std::string("Deferred disconnect failed: ") + e.what());
            } catch (...) {
                ALPACA_LOG_ERROR(log_tag_, "Deferred disconnect failed: non-std exception");
            }
        }
    }

    enum ConnTaskState : std::uint8_t { kConnIdle = 0, kConnConnect = 1, kConnDisconnect = 2 };

    std::string log_tag_;
    std::atomic<ConnTaskState> conn_task_{kConnIdle};
    std::mutex connection_mutex_;  // guards shutting_down_, thread spawn, Idle publish
    std::mutex pending_mutex_;     // leaf lock for pending_disconnect_ only
    bool pending_disconnect_ = false;
    bool shutting_down_ = false;  // under connection_mutex_
    std::thread connection_thread_;
};

}  // namespace alpacacore

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
 * - conn_task_ only transitions to Idle inside the task's tail UNDER
 *   connection_mutex_ — the same lock every start_connection_task holds — so
 *   a recorder can never misjudge the in-flight state (the double-read race).
 *   Idle is published BEFORE the tail's deferred disconnect runs, so the
 *   deferred call can't be mistaken for another racing disconnect
 *   (self-block: device stays Connected, stale flag no-ops the next connect).
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
 *        if (consume_pending_disconnect()) return;  // newer disconnect wins
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
    bool record_disconnect_if_connect_in_flight(bool connected_now) {
        if (!connected_now && conn_task_.load() == kConnConnect) {
            record_pending_disconnect();
            return true;
        }
        return false;
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
    /// mutex held. Returns true if a newer disconnect was pending — the
    /// caller must return without connecting.
    bool consume_pending_disconnect() {
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
            ALPACA_LOG_ERROR(log_tag_.c_str(), std::string("Connection task failed: ") + e.what());
        }
        // Tail under connection_mutex_: see the class comment for why the
        // Idle publish and the deferred-disconnect handoff must both
        // happen under this lock, in this order.
        std::lock_guard<std::mutex> conn_lock(connection_mutex_);
        bool need_disconnect = false;
        {
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            if (pending_disconnect_) {
                pending_disconnect_ = false;
                need_disconnect = connect && get_connected();
            }
        }
        conn_task_.store(kConnIdle);
        if (need_disconnect) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR(log_tag_.c_str(), std::string("Deferred disconnect failed: ") + e.what());
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

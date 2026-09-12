// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <alpacacore/managementdriver.h>
#include <alpacahttp/util/socket_utils.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

#include "config.h"
#include "router.h"

namespace alpacahttp {

class Server {
public:
    explicit Server(const Config& config);
    ~Server();

    // Set management driver (from AlpacaCore)
    void set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver);

    // Set shutdown callback (called when shutdown endpoint is requested)
    void set_shutdown_callback(std::function<void()> callback);
    // Set restart callback (called when restart endpoint is requested)
    void set_restart_callback(std::function<void()> callback);

    // Start the server (blocking)
    void start();

    // Start the server in background thread
    void start_async();

    // Stop the server
    void stop();

    // Check if server is running
    bool is_running() const { return running_; }

    // Wait for server to stop
    void wait();

    // Test-only seam (open-astro#314): the router this server dispatches to,
    // so a test can install HostClock hooks before start() and then observe
    // what the RTC probe thread does with them. Call it before start(); the
    // router's own seam replaces the clock object and is not safe against a
    // request in flight.
    Router& router_for_test() { return router_; }

private:
    Config config_;
    Router router_;
    std::atomic<bool> running_{false};
    std::atomic<util::SocketHandle> server_fd_{util::kInvalidSocket};
    std::thread server_thread_;
    // Guards ownership of server_thread_ only (not the lifecycle phases).
    // stop() is re-entrant from another thread -- the shutdown endpoint's
    // detached thread runs the shutdown callback, which can make the embedder's
    // own loop call stop() too -- so without this both callers could reach
    // join_server_thread() and join() the same std::thread. Concurrent join()
    // is UB, and in practice the second pthread_join throws std::system_error
    // that nothing catches, i.e. std::terminate(): the very failure #402 set
    // out to remove, moved onto the normal shutdown path. The lock is NEVER
    // held across the join itself -- the thread is moved out first -- so a
    // worker that calls stop() cannot deadlock against it.
    std::mutex server_thread_mutex_;
    // Set while one caller is inside owned.join(). Every other caller waits on
    // server_thread_cv_ until it clears, so join_server_thread() returns only
    // once the server thread is really gone -- for the loser as well as the
    // winner. Without that wait the loser returns while run_server() is still
    // unwinding, and ~Server() then tears the object down underneath it.
    bool server_thread_joining_{false};
    std::condition_variable server_thread_cv_;
    // Bumped every time start_async() installs a new server thread. A waiter
    // captures it before waiting and gives up if it moved: without that it
    // would re-read server_thread_ after waking and could adopt the NEXT
    // generation's thread -- the restart path does exactly that, and joining a
    // freshly started server hangs stop() forever.
    std::uint64_t server_thread_generation_{0};

    // One client connection. Owned by exactly one party at a time: the accept
    // loop (briefly, until it is parked), the reactor (while idle, waiting for
    // the peer's next request), the ready queue, or a worker (while a request
    // is being served). Ownership moves with the unique_ptr, so a connection
    // can never be polled and served at the same time.
    struct Connection {
        util::SocketHandle fd{util::kInvalidSocket};
        std::string remote_address;
        // Bytes read past the end of the last request (a pipelining client).
        // A connection with buffered bytes is never parked: the worker keeps
        // serving until it is empty, since the reactor polls the socket and
        // would not see them.
        std::string carried;
        std::uint64_t requests_served{0};
        std::chrono::steady_clock::time_point opened_at;
        // When the reactor gives up waiting on this connection: the first
        // request's slowloris bound, then the keep-alive idle gap, and never
        // past the connection lifetime cap.
        std::chrono::steady_clock::time_point deadline;
        // Set by the reactor when the deadline passed: the worker that picks
        // it up closes it (gracefully, off the reactor thread) instead of
        // reading from it.
        bool close_only{false};
    };
    using ConnectionPtr = std::unique_ptr<Connection>;

    // Workers: fixed pool, each serves one request at a time. thread_pool_size
    // therefore bounds concurrent REQUESTS; idle connections cost no worker.
    std::vector<std::thread> worker_threads_;
    std::deque<ConnectionPtr> ready_queue_;
    std::mutex queue_mutex_;
    // One permit per queued connection, plus one per worker when they are
    // told to stop or a new generation starts. A worker blocks on the
    // semaphore, not on a condition variable, so the critical section is
    // confined to the pop and never spans the socket I/O that follows: the
    // static analyzer (unix.BlockInCriticalSection) cannot see a condition
    // variable's wait release its lock and would report every recv() the
    // worker does afterwards as blocking under queue_mutex_. Spurious
    // permits (a wake that finds the queue empty) are harmless; the worker
    // loops and acquires again.
    std::counting_semaphore<> ready_signal_{0};
    bool shutdown_workers_{false};
    // Worker threads currently alive, including any that detached themselves
    // across a restart. Counted at SPAWN (before the thread has executed an
    // instruction), decremented by the thread on exit. stop() and each
    // generation bump release this many permits, so every live worker, not
    // just the current pool, gets its wake-and-exit; counting inside the
    // thread body instead would let a stop() that lands before a new thread
    // reaches its first instruction undercount, leave that thread with no
    // permit, and hang the join.
    std::atomic<std::size_t> worker_count_{0};
    // Held by run_server() while it spawns the reactor and the workers, and
    // by stop() for its whole teardown, so a stop() (a second restart on the
    // heels of a first, or start_async() followed at once by stop()) cannot
    // walk worker_threads_ while it is still being filled.
    std::mutex lifecycle_mutex_;
    // Bumped by every run_server(). A worker exits when the generation it
    // was spawned in is no longer current, so a worker that detached itself
    // (stop() called from inside a request handler on that worker) cannot
    // survive into the next generation as an extra thread once start()
    // clears shutdown_workers_ again. Guarded by queue_mutex_.
    std::uint64_t worker_generation_{0};

    // Reactor: one thread parks idle connections on a poll set and hands them
    // to the ready queue when their next request arrives. Woken through a
    // self-pipe when a worker parks a connection or stop() begins.
    std::thread reactor_thread_;
    // open-astro#314: the hardware-RTC probe refresh. Its own thread rather
    // than the reactor's, because the probe reads
    // /sys/class/rtc/rtcN/since_epoch and can block on a wedged I2C bus for
    // about a second, while the reactor must never block in anything but
    // poll() (AGENTS.md) -- a stalled reactor delays every parked keep-alive
    // connection's next request, which is the cost this change exists to
    // avoid. Wakes every 31 s, or immediately when stop() sets the flag.
    std::thread rtc_probe_thread_;
    std::mutex rtc_probe_mutex_;
    std::condition_variable rtc_probe_cv_;
    bool rtc_probe_stop_{false};
    // Threads stop() could not join because it was running ON them (a
    // handler that called stop() from its own worker; no current handler
    // does). Never detached: the next stop() from another thread, or the
    // destructor, joins them, so no server thread outlives the Server and
    // nothing can touch its members, the wake pipe included, after
    // destruction. Guarded by lifecycle_mutex_.
    std::vector<std::thread> orphaned_threads_;
    // Created once in the constructor and closed in the destructor after
    // every thread, orphaned ones included, has been joined; never replaced
    // while the Server is alive, since wake_reactor() reads the write end
    // from any thread with no lock. Leftover wake bytes from a previous run
    // cost one spurious poll() return.
    int reactor_wake_fds_[2]{-1, -1};
    std::mutex reactor_mutex_;
    std::vector<ConnectionPtr> reactor_incoming_;
    bool reactor_accepting_{false};

    // Connections alive in any owner. Bounded by Config::max_connections so
    // idle keep-alive clients cannot exhaust the process's descriptors; at
    // the bound the accept loop pauses and new clients wait in the listen
    // backlog.
    std::atomic<std::size_t> live_connections_{0};

    void run_server();
    void reactor_loop();
    void rtc_probe_loop();
    void worker_thread(std::uint64_t generation);
    enum class ServeResult : std::uint8_t { KeepOpen, Close };
    ServeResult serve_one_request(Connection& conn);
    void park_connection(ConnectionPtr conn);
    void enqueue_ready(ConnectionPtr conn);
    void close_connection(ConnectionPtr conn, bool graceful);
    void wake_reactor();
    void close_wake_pipe();
    void join_orphaned_threads(std::thread::id current_id);
    // Joins server_thread_ if it is joinable, keeping it as an orphan when it
    // IS the calling thread. Called from the end of stop(), and from stop()'s
    // and start_async()'s !running_ paths: a run_server() that returned early
    // (bad port, bind() failure, no wake pipe) leaves a joinable thread behind
    // with running_ already false, and destroying a joinable std::thread calls
    // std::terminate() (issue #402).
    // `only_if_stopped` is how stop()'s !running_ path asks for "reap a thread
    // that already returned, but never adopt a live one". The decision is made
    // under server_thread_mutex_, the same lock start_async() takes to install
    // a thread, so it cannot be raced by a restart.
    void join_server_thread(std::thread::id current_id, bool only_if_stopped = false);
    void reset_queues_for_start();
    void handle_shutdown_request();
    void handle_restart_request();

    std::function<void()> shutdown_callback_;
    std::mutex shutdown_mutex_;
    std::atomic<bool> shutdown_requested_{false};
    std::function<void()> restart_callback_;
    std::mutex restart_mutex_;
    std::atomic<bool> restart_requested_{false};
};

} // namespace alpacahttp

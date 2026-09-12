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

#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/util/socket_utils.h>
#include <alpacahttp/version.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace alpacahttp {

Server::Server(const Config& config)
    : config_(config)
{
    router_.set_shutdown_callback([this]() { handle_shutdown_request(); });
    router_.set_restart_callback([this]() { handle_restart_request(); });
    router_.set_server_info(config_.server_name(), config_.manufacturer(), alpacahttp::kVersion, config_.location(),
                            config_.profile_name());
    router_.set_config_path(config_.config_path());
    router_.set_sync_system_clock_from_clients(config_.sync_system_clock_from_clients());

    // The reactor's wake pipe lives as long as the Server. Non-blocking on
    // both ends: a wake is one byte, and a full pipe already means a wake is
    // pending.
    if (::pipe(reactor_wake_fds_) == 0) {
        for (int pipe_fd : reactor_wake_fds_) {
            ::fcntl(pipe_fd, F_SETFL, ::fcntl(pipe_fd, F_GETFL) | O_NONBLOCK);
        }
    } else {
        util::log_error("Failed to create reactor wake pipe: " + util::socket_error_message(errno));
        reactor_wake_fds_[0] = -1;
        reactor_wake_fds_[1] = -1;
    }
}

void Server::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    router_.set_management_driver(mgmt_driver);
}

void Server::set_shutdown_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    shutdown_callback_ = std::move(callback);
}

void Server::set_restart_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(restart_mutex_);
    restart_callback_ = std::move(callback);
}

Server::~Server() {
    stop();
    {
        // Anything stop() could not join because it ran on that thread.
        // Joined here, on whatever thread destroys the Server, so no server
        // thread survives the object. (Destroying the Server from inside one
        // of its own request handlers is not supported.)
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        join_orphaned_threads(std::this_thread::get_id());
    }
    close_wake_pipe();
}

// Join every orphaned thread except the calling one. Caller holds
// lifecycle_mutex_.
void Server::join_orphaned_threads(std::thread::id current_id) {
    std::vector<std::thread> still_orphaned;
    for (auto& thread : orphaned_threads_) {
        if (!thread.joinable()) {
            continue;
        }
        if (thread.get_id() == current_id) {
            still_orphaned.push_back(std::move(thread));
            continue;
        }
        thread.join();
    }
    orphaned_threads_.swap(still_orphaned);
}

// Prepare the queues for a (re)start. Both are expected to be empty: stop()
// joins the reactor, which closes what it held, and the workers, which drain
// ready_queue_. Anything still here is a connection nobody owns, so it is
// closed through close_connection rather than dropped, which is also what
// keeps live_connections_ exact. The counter itself is never reset: every
// connection is counted once at accept and once at close, whichever server
// generation each happens in, so a connection that outlives a restart (a
// worker detached because stop() was called on it) still balances.
void Server::reset_queues_for_start() {
    std::deque<ConnectionPtr> leftover_ready;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = false;
        leftover_ready.swap(ready_queue_);
    }
    std::vector<ConnectionPtr> leftover_incoming;
    {
        std::lock_guard<std::mutex> lock(reactor_mutex_);
        leftover_incoming.swap(reactor_incoming_);
    }
    for (auto& conn : leftover_ready) {
        close_connection(std::move(conn), true);
    }
    for (auto& conn : leftover_incoming) {
        close_connection(std::move(conn), true);
    }
}

void Server::start() {
    if (running_) {
        return;
    }

    shutdown_requested_ = false;
    reset_queues_for_start();
    running_ = true;
    run_server();
}

void Server::start_async() {
    if (running_) {
        return;
    }

    // Same hazard as stop()'s !running_ path (issue #402): a previous
    // start_async() whose run_server() failed early left a joinable thread in
    // server_thread_, and assigning over a joinable std::thread also calls
    // std::terminate(). An embedder retrying on another port does exactly
    // this. The thread has already finished; the join just reaps it.
    join_server_thread(std::this_thread::get_id());

    shutdown_requested_ = false;
    reset_queues_for_start();
    running_ = true;
    {
        // Same guard as join_server_thread(): the assignment is the other half
        // of server_thread_'s ownership, and assigning over a joinable thread
        // is std::terminate() too.
        std::unique_lock<std::mutex> guard(server_thread_mutex_);
        // Wait out any join that started in the gap since join_server_thread()
        // returned, so this assignment cannot land on a thread another caller
        // is mid-join on. NOTE this does not make concurrent start_async()
        // calls safe: two threads that both pass the `if (running_)` check
        // above will both arrive here and the second assigns over a joinable
        // thread, which is std::terminate(). No caller does that today --
        // handle_restart_request() is CAS-guarded -- and serialising
        // start_async() itself is out of this change's scope.
        server_thread_cv_.wait(guard, [this] { return !server_thread_joining_; });
        server_thread_ = std::thread(&Server::run_server, this);
        // New generation: any waiter still parked on the previous one must give
        // up rather than adopt this thread.
        ++server_thread_generation_;
        server_thread_cv_.notify_all();
    }
}

void Server::stop() {
    if (!running_) {
        // Not "nothing to do". run_server() can return early with running_
        // already false -- an out-of-range port, a bind() that failed because
        // the port is in use or a previous instance has not released it, or a
        // missing reactor wake pipe -- and start_async() has by then created
        // the thread and stored it. Returning here without joining left a
        // joinable std::thread for ~Server() to destroy, which calls
        // std::terminate(): a port conflict became an abort at destruction
        // instead of a clean failure the caller could report, and the caller's
        // own is_running() check did not help, because it correctly returned
        // false and the crash came later (issue #402).
        //
        // only_if_stopped: reap a thread that already returned, never adopt a
        // live one. Between this `!running_` read and the lock inside, a
        // restart (handle_restart_request() stops then starts on a detached
        // thread) can install a running server -- adopting it hangs this
        // caller forever, which for the example embedder is the process never
        // exiting.
        join_server_thread(std::this_thread::get_id(), /*only_if_stopped=*/true);
        return;
    }

    util::log_info("Stopping HTTP server...");
    running_ = false;
    const auto current_id = std::this_thread::get_id();
    // Phases 1 and 2 run under lifecycle_mutex_, serialized against
    // run_server()'s spawn phase: either that phase ran first and every
    // thread it made is in worker_threads_ and counted in worker_count_, or
    // it runs after this and sees running_ false and spawns nothing. The
    // mutex is released before phase 3 joins the server thread, which may be
    // about to take it for exactly that check.
    {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

        // 1. Reactor first. Once it stops accepting handoffs, a worker that
        //    finishes a request closes the connection instead of parking it, and
        //    the reactor closes every connection it was holding on its way out.
        //    Nothing is ever parked past this point, so stop() no longer has to
        //    wait out an idle gap: an idle client costs nothing here.
        {
            std::lock_guard<std::mutex> lock(reactor_mutex_);
            reactor_accepting_ = false;
        }
        wake_reactor();
        // The RTC probe timer, woken out of its wait the same way. It holds
        // no connections and serves no request, so it can go first; a pass
        // already inside the probe finishes its read before the flag is
        // observed, which is bounded by the bus timeout (#314).
        {
            std::lock_guard<std::mutex> lock(rtc_probe_mutex_);
            rtc_probe_stop_ = true;
        }
        rtc_probe_cv_.notify_all();
        // Threads a previous stop() could not join because it ran on them.
        join_orphaned_threads(current_id);
        if (rtc_probe_thread_.joinable()) {
            if (rtc_probe_thread_.get_id() == current_id) {
                orphaned_threads_.push_back(std::move(rtc_probe_thread_));
            } else {
                rtc_probe_thread_.join();
            }
        }
        if (reactor_thread_.joinable()) {
            // The reactor runs no handler code, so it cannot be the caller;
            // the orphan branch is kept only so a future bug cannot deadlock.
            if (reactor_thread_.get_id() == current_id) {
                orphaned_threads_.push_back(std::move(reactor_thread_));
            } else {
                reactor_thread_.join();
            }
        }

        // 2. Workers. Each finishes what is queued (every response from now on
        //    carries Connection: close, since running_ is false) and exits.
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            shutdown_workers_ = true;
        }
        // One permit per live worker, including any detached one that is no
        // longer in worker_threads_, so none stays blocked on the semaphore.
        ready_signal_.release(static_cast<std::ptrdiff_t>(worker_count_.load(std::memory_order_relaxed)));
        for (auto& thread : worker_threads_) {
            if (!thread.joinable()) {
                continue;
            }
            if (thread.get_id() == current_id) {
                // stop() was called from inside a request handler on this
                // worker (not a path any current handler takes: the management
                // restart/shutdown endpoints run it on a detached thread). It
                // cannot join itself. It finishes its request and exits on
                // the shutdown flag or the generation check; its handle is
                // kept, not detached, so the next stop() from another thread
                // or the destructor joins it.
                orphaned_threads_.push_back(std::move(thread));
                continue;
            }
            thread.join();
        }
        worker_threads_.clear();
    }

    // 3. Listener: shutdown before close so a blocked accept() wakes up.
    auto fd = server_fd_.exchange(util::kInvalidSocket);
    if (fd != util::kInvalidSocket) {
        util::socket_shutdown(fd);
        util::socket_close(fd);
    }

    join_server_thread(current_id);
    util::log_info("HTTP server stopped");
}

void Server::join_server_thread(std::thread::id current_id, bool only_if_stopped) {
    // Take sole ownership of the thread under server_thread_mutex_, then act
    // on it with the lock released. Whoever wins the move joins; every other
    // caller finds server_thread_ empty and returns, so exactly one join()
    // ever runs on it. stop() is re-entrant from another thread (the shutdown
    // endpoint's detached thread runs the shutdown callback, which can make
    // the embedder's loop call stop() as well), and concurrent join() on one
    // std::thread is UB -- in practice the second pthread_join throws
    // std::system_error that nothing catches, i.e. std::terminate().
    //
    // The join happens OUTSIDE the lock deliberately: it blocks until the
    // accept loop unwinds, and holding a lock that any other stop() caller
    // needs for that long is how this turns into a deadlock instead.
    // Take sole ownership of the thread, join it with the lock released, and
    // make every OTHER caller wait until that join has finished. Both halves
    // matter:
    //
    //  - Exactly one caller may join. stop() is re-entrant from another thread
    //    (the shutdown endpoint's detached thread runs the shutdown callback,
    //    which can make the embedder's own loop call stop() too), and
    //    concurrent join() on one std::thread is UB -- in practice the second
    //    pthread_join throws std::system_error that nothing catches.
    //
    //  - Every caller must still return only once the thread is GONE, because
    //    ~Server() runs straight after stop() and tears down the wake pipe,
    //    the config and the connection maps that run_server() is still using.
    //    Simply returning when another caller won the move loses that: the
    //    loser's stop() returns while the accept loop is still unwinding.
    //
    // The join is outside the lock: it blocks until the accept loop unwinds,
    // and the waiters need the mutex free to sit on the condition variable.
    // No deadlock with the winner -- the caller that runs stop()'s phases
    // closes the listener before it ever reaches this point, so the join it
    // waits on can always complete.
    std::thread owned;
    {
        std::unique_lock<std::mutex> guard(server_thread_mutex_);
        // Wait on THIS generation, not just "no join in flight". The waiter
        // releases the mutex, so by the time it wakes the winner may already
        // have finished its join, returned from stop() and called
        // start_async() again -- the restart path (handle_restart_request()
        // stops and restarts while the embedder's loop, seeing is_running()
        // false, calls stop() too) does exactly that. Re-reading
        // server_thread_ blind would then adopt the NEW server's thread and
        // join it, hanging stop() forever while the restarted server runs on.
        const std::uint64_t generation = server_thread_generation_;
        server_thread_cv_.wait(
            guard, [this, generation] { return !server_thread_joining_ || server_thread_generation_ != generation; });
        if (only_if_stopped && running_) {
            // stop()'s !running_ path asked to reap a thread that had already
            // returned early (a failed bind, #402). By the time it won this
            // lock a restart may have installed a LIVE thread and set running_
            // -- start_async() sets running_ before it takes this lock, so
            // seeing it true here means server_thread_ is the new server's.
            // Joining that blocks until the restarted server stops, which
            // nothing is left to do: the embedder's stop() never returns.
            // Reading running_ under THIS lock is what makes the answer
            // independent of when the lock was won.
            return;
        }
        if (server_thread_generation_ != generation) {
            // A newer server thread exists, which means the one this call was
            // about has already been joined -- start_async() only installs a
            // new one after join_server_thread() has reaped the old. Not ours.
            return;
        }
        if (!server_thread_.joinable()) {
            // Either never started, or a join that has already COMPLETED
            // reaped it -- the wait above is what makes that distinction safe.
            return;
        }
        owned = std::move(server_thread_);
        server_thread_joining_ = true;
    }

    if (owned.get_id() == current_id) {
        // Unreachable from stop() (run_server() never calls stop()); kept as
        // an orphan rather than a detach for the same reason as above.
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        orphaned_threads_.push_back(std::move(owned));
    } else {
        owned.join();
    }

    {
        std::lock_guard<std::mutex> guard(server_thread_mutex_);
        server_thread_joining_ = false;
        // notify_all() INSIDE the lock, deliberately. A waiter only needs the
        // mutex to re-check the predicate, so notifying after the unlock would
        // let it return from stop() -- and the embedder run ~Server() -- while
        // this thread is still about to touch server_thread_cv_. This is the
        // last `this` access after the protocol's own "the thread is gone, you
        // may destroy me now" signal, so it is the one that has to be inside.
        server_thread_cv_.notify_all();
    }
}

// Destructor only, after every thread has been joined. The pipe is never
// closed or replaced while the Server is alive: stop() may return with the
// accept loop still unwinding (blocking start() stopped from a handler), and
// an orphaned worker may still call wake_reactor() until it is joined, and
// either would otherwise write into a recycled descriptor.
void Server::close_wake_pipe() {
    for (int& pipe_fd : reactor_wake_fds_) {
        if (pipe_fd >= 0) {
            ::close(pipe_fd);
            pipe_fd = -1;
        }
    }
}

void Server::wait() {
    // Routed through the same ownership handshake as join_server_thread(), so
    // a concurrent stop() and wait() cannot both join the one thread.
    join_server_thread(std::this_thread::get_id());
}

namespace {

// Pending-connection backlog for the listener. Kept comfortably above the
// worker pool: with keep-alive a burst of new clients can arrive while every
// worker is mid-request, and a backlog shorter than that burst turns into
// refused connections rather than a short wait.
constexpr int kListenBacklog = 64;

// Per-connection socket timeout: a peer that stalls mid-request (slowloris)
// or mid-response is disconnected after this many seconds of inactivity.
constexpr int kSocketTimeoutSeconds = 30;

// Resolve the peer address once per connection so the router can
// discriminate clients that send no ClientID in the per-client Connected
// registry (issue #163).
std::string peer_address_string(const struct sockaddr_storage& peer) {
    char addr_buf[INET6_ADDRSTRLEN] = {0};
    if (peer.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in*>(&peer)->sin_addr, addr_buf, sizeof(addr_buf));
    } else if (peer.ss_family == AF_INET6) {
        // On the dual-stack listener an IPv4 client arrives as a v4-mapped
        // IPv6 peer (::ffff:a.b.c.d); report the plain dotted form so the
        // registry key and logs keep the format the IPv4-only listener
        // produced.
        const struct in6_addr* a6 = &reinterpret_cast<const struct sockaddr_in6*>(&peer)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a6)) {
            struct in_addr a4 {};
            std::memcpy(&a4, &a6->s6_addr[12], sizeof(a4));
            inet_ntop(AF_INET, &a4, addr_buf, sizeof(addr_buf));
        } else {
            inet_ntop(AF_INET6, a6, addr_buf, sizeof(addr_buf));
        }
    }
    return addr_buf;
}

// Create a bound, listening HTTP socket on `port`. Prefers a dual-stack IPv6
// socket (IPV6_V6ONLY off) so both ::1 and 127.0.0.1 connect directly; falls
// back to IPv4-only where IPv6 is unavailable.
//
// Why dual-stack: .NET clients (ConformU, NINA on Linux/macOS) connecting to
// "127.0.0.1" or "localhost" try ::1 first and only then 127.0.0.1. With an
// IPv4-only listener every request pays a refused IPv6 SYN before the real
// connect, and on a Raspberry Pi that fallback showed up in ConformU as a
// consistent ~100 ms on a handful of otherwise 1 ms members (captured with
// tcpdump on the HAE16 EQ validation, 2026-08-25).
util::SocketHandle create_listener(int port) {
    util::SocketHandle fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd != util::kInvalidSocket) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        int v6only = 0;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) != 0) {
            // Without a confirmed dual-stack socket, IPv4 clients could be
            // locked out on a host whose default is v6-only; fall back to the
            // IPv4 listener rather than guess.
            util::log_warning("IPV6_V6ONLY could not be cleared (" +
                              util::socket_error_message(util::socket_get_last_error()) +
                              "); falling back to an IPv4-only HTTP listener");
            util::socket_close(fd);
            fd = util::kInvalidSocket;
        }
        struct sockaddr_in6 address6 {};
        address6.sin6_family = AF_INET6;
        address6.sin6_addr = in6addr_any;
        address6.sin6_port = htons(static_cast<u_short>(port));
        if (fd != util::kInvalidSocket) {
            if (bind(fd, reinterpret_cast<struct sockaddr*>(&address6), sizeof(address6)) == 0 &&
                listen(fd, kListenBacklog) == 0) {
                return fd;
            }
            util::socket_close(fd);
            util::log_warning("Dual-stack HTTP listener unavailable, falling back to IPv4 only");
        }
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == util::kInvalidSocket) {
        return util::kInvalidSocket;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<u_short>(port));
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 || listen(fd, kListenBacklog) < 0) {
        util::socket_close(fd);
        return util::kInvalidSocket;
    }
    return fd;
}

}  // namespace

void Server::run_server() {
    util::ensure_winsock();
    const int port = config_.http_port();
    if (port < 0 || port > static_cast<int>(std::numeric_limits<u_short>::max())) {
        util::log_error("Invalid HTTP port: " + std::to_string(port));
        running_ = false;
        return;
    }

    util::SocketHandle server_fd = create_listener(port);
    if (server_fd == util::kInvalidSocket) {
        util::log_error("Failed to bind HTTP listener to port " + std::to_string(port));
        running_ = false;
        return;
    }

    // Store server_fd so we can close it from stop()
    server_fd_.store(server_fd);

    util::log_info("Server listening on port " + std::to_string(port));

    // The listener fd can go bad underneath us without stop() being called (a
    // stray double-close elsewhere in the process can free and then re-close
    // our fd number). Observed once in the field: the accept loop broke
    // silently and the whole server shut down "successfully" mid ConformU
    // run. Recreate the listener instead of dying.
    auto last_rebind = std::chrono::steady_clock::time_point::min();
    auto rebind_listener = [&]() -> bool {
        // Backoff ACROSS rebind cycles too: if the fd-loss condition recurs
        // immediately after a successful rebind, sleep instead of spinning
        // select-fail -> rebind -> select-fail with continuous error logging.
        auto now = std::chrono::steady_clock::now();
        if (now - last_rebind < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        last_rebind = now;
        auto old = server_fd_.exchange(util::kInvalidSocket);
        if (old != util::kInvalidSocket) {
            // The fd number may have been recycled to an UNRELATED live socket
            // by whatever double-closed the listener. Close it only if it is
            // still a listening socket; otherwise leak the number rather than
            // sever an innocent connection.
            int acc = 0;
            socklen_t len = sizeof(acc);
            if (getsockopt(old, SOL_SOCKET, SO_ACCEPTCONN, &acc, &len) == 0 && acc != 0) {
                util::socket_close(old);
            }
        }
        for (int attempt = 0; attempt < 10 && running_; ++attempt) {
            util::SocketHandle fd = create_listener(port);
            if (fd != util::kInvalidSocket) {
                server_fd_.store(fd);
                server_fd = fd;
                util::log_error("HTTP listener recreated after descriptor loss");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return false;
    };

    // Self-pipe the reactor sleeps on alongside its idle connections, so a
    // worker parking a connection (or stop()) can wake it without a timeout
    // spin. Created by the constructor; refuse to run without it.
    if (reactor_wake_fds_[0] < 0 || reactor_wake_fds_[1] < 0) {
        util::log_error("Reactor wake pipe unavailable; cannot start HTTP server");
        running_ = false;
        auto listener = server_fd_.exchange(util::kInvalidSocket);
        if (listener != util::kInvalidSocket) {
            util::socket_close(listener);
        }
        return;
    }
    std::size_t pool_size = config_.thread_pool_size();
    {
        // Spawn phase, serialized against stop(): either stop() has not run
        // yet and will find every thread made here in worker_threads_ and
        // counted, or it already ran (start_async() followed at once by
        // stop()) and there is nothing to spawn for.
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (!running_) {
            auto listener = server_fd_.exchange(util::kInvalidSocket);
            if (listener != util::kInvalidSocket) {
                util::socket_close(listener);
            }
            util::log_info("Server stopped");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(reactor_mutex_);
            reactor_accepting_ = true;
        }
        reactor_thread_ = std::thread(&Server::reactor_loop, this);
        {
            std::lock_guard<std::mutex> lock(rtc_probe_mutex_);
            rtc_probe_stop_ = false;
        }
        rtc_probe_thread_ = std::thread(&Server::rtc_probe_loop, this);

        // Start worker thread pool for handling concurrent requests. A new
        // generation: any worker left over from the previous one (detached
        // because stop() was called on it) wakes, sees its generation is
        // stale, and exits instead of serving alongside these.
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            generation = ++worker_generation_;
        }
        // One permit per worker still alive from a previous generation
        // (detached across a restart), so each wakes, sees its generation is
        // stale, and exits, however many there are. The new pool is counted
        // below, after this, so none of these permits are consumed by it.
        ready_signal_.release(static_cast<std::ptrdiff_t>(worker_count_.load(std::memory_order_relaxed)));
        worker_threads_.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            // Counted before the thread exists, so a stop() that follows
            // this phase releases a permit for it even if it has not yet
            // executed an instruction.
            worker_count_.fetch_add(1, std::memory_order_relaxed);
            worker_threads_.emplace_back(&Server::worker_thread, this, generation);
        }
    }
    util::log_info("Started " + std::to_string(pool_size) + " worker threads for concurrent request handling");

    const std::size_t max_connections = config_.max_connections();

    // Accept connections using select() to allow checking running_ flag periodically
    while (running_) {
        // At the connection bound, stop accepting rather than accept-and-
        // refuse: the kernel keeps completing handshakes into the listen
        // backlog, so a new client waits (an idle connection expires within
        // kKeepAliveIdleSeconds) instead of getting a reset. Polled rather
        // than signalled; the 20 ms adds nothing a client can notice.
        if (live_connections_.load(std::memory_order_relaxed) >= max_connections) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // Use select() to wait for connections with a timeout, so we can check running_ periodically
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;  // 500ms timeout
        
        int select_result = util::socket_select(server_fd, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result < 0) {
            // Error in select
            int err = util::socket_get_last_error();
            if (util::socket_interrupted(err)) {
                // Interrupted by signal - continue
                continue;
            } else if (util::socket_bad_descriptor(err) || util::socket_not_socket(err)) {
                if (!running_) {
                    break;  // stop() closed the listener deliberately
                }
                util::log_error("HTTP listener descriptor went bad in select(); rebinding");
                if (!rebind_listener()) {
                    break;
                }
                continue;
            } else {
                if (running_) {
                    util::log_error("Server select error: " + util::socket_error_message(err));
                }
                break;
            }
        } else if (select_result == 0) {
            // Timeout - check running_ flag and continue
            continue;
        }
        
        // Connection available - accept it
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_storage client_address {};
            util::SocketLen client_len = sizeof(client_address);

            util::SocketHandle client_fd =
                accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_address), &client_len);
            if (client_fd == util::kInvalidSocket) {
                int err = util::socket_get_last_error();
                if (util::socket_interrupted(err) || util::socket_would_block(err)) {
                    // Interrupted or would block - continue
                    continue;
                } else if (util::socket_bad_descriptor(err) || util::socket_not_socket(err)) {
                    if (!running_) {
                        break;  // stop() closed the listener deliberately
                    }
                    util::log_error("HTTP listener descriptor went bad in accept(); rebinding");
                    if (!rebind_listener()) {
                        break;
                    }
                    continue;
                } else {
                    if (running_) {
                        util::log_error("Failed to accept connection: " + util::socket_error_message(err));
                    }
                    continue;
                }
            }

            // Bound how long a slow or stalled peer can hold a worker mid-
            // request (slowloris). Fail closed: without recv/send timeouts a
            // stalled request could pin a worker forever.
            if (!util::socket_set_timeouts(client_fd, kSocketTimeoutSeconds)) {
                util::log_warning("Dropping connection, failed to set socket timeouts: " +
                                  util::socket_error_message(util::socket_get_last_error()));
                util::socket_close(client_fd);
                continue;
            }

            auto conn = std::make_unique<Connection>();
            conn->fd = client_fd;
            conn->remote_address = peer_address_string(client_address);
            conn->opened_at = std::chrono::steady_clock::now();
            // The first request gets the same slowloris bound a worker's recv
            // would have given it, but waited out on the reactor: a client
            // that connects and never sends costs no worker at all.
            conn->deadline = conn->opened_at + std::chrono::seconds(kSocketTimeoutSeconds);
            live_connections_.fetch_add(1, std::memory_order_relaxed);
            park_connection(std::move(conn));
        }
    }

    // Clean up socket if not already closed
    auto fd = server_fd_.exchange(util::kInvalidSocket);
    if (fd != util::kInvalidSocket) {
        util::socket_close(fd);
    }
    util::log_info("Server stopped");
}

namespace {

// Upper bound on how long a whole request may take to arrive (headers +
// body). Complements SO_RCVTIMEO, which only bounds the gap between bytes —
// large ImageArray responses are unaffected (this bounds the read side only).
constexpr int kRequestDeadlineSeconds = 120;

// How long a keep-alive connection may sit idle between requests before the
// worker gives it up. Persistent connections matter for timing: ConformU's
// .NET client stalled ~175 ms before opening each new TCP connection on a
// Raspberry Pi 3B, and with every response marked "Connection: close" that
// stall landed inside its FAST-target measurements (CameraState, CameraXSize,
// SensorType on a ZWO camera; DeviceState, AlignmentMode, EquatorialSystem on
// a mount) while the server itself answered in 2-8 ms. Kept well under the
// per-request slowloris bound so idle clients cannot pin the pool.
constexpr int kKeepAliveIdleSeconds = 15;

// Upper bound on requests served over one keep-alive connection. Without
// this, a small number of clients that simply send a request at least every
// kKeepAliveIdleSeconds (accidentally -- several long-lived Alpaca clients --
// or adversarially) can each pin one worker thread indefinitely, since the
// thread pool is fixed-size and the accept queue has no backpressure of its
// own (PR #2 review). Closing after N requests bounds how long any single
// connection can hold a worker, forcing well-behaved clients to reconnect
// (cheap: this is what keep-alive was added to avoid *per-request*, not
// forbid outright) and adversarial ones to give up a worker periodically.
constexpr std::uint64_t kMaxRequestsPerConnection = 1000;

// The connection lifetime cap (Config::keep_alive_lifetime_seconds, 300 s by
// default) is the second, independent bound on the same failure mode: the
// count cap alone still lets a connection that sends one request every
// kKeepAliveIdleSeconds stay persistent for ~4 hours (1000 * 15 s) and simply
// reconnect afterward (PR #2 review). Once a connection has been open that
// long, the NEXT response forces a reconnect no matter how few requests it
// has served (never a reactor-side close, which would race the client's next
// request; an idle connection past the cap just runs out its idle gap). A
// well-behaved
// long-lived client (autoguiding, ConformU) pays one extra handshake every
// few minutes, negligible next to the per-request handshake keep-alive
// exists to avoid. Lives in Config so the cap can be tested.

// Upper bound on the request line + headers; larger header blocks are
// rejected before any body is read.
constexpr std::size_t kMaxHeaderBytes = std::size_t{64} * 1024;

void send_error(util::SocketHandle socket_fd, int status, const char* reason, const char* body) {
    Response error_response;
    error_response.set_status(status, reason);
    error_response.set_body(body);
    std::string response_str = error_response.to_string();
    util::socket_send_all(socket_fd, response_str.c_str(), response_str.size());
}

// True when the client wants the connection kept open after this request
// (RFC 7230 §6.3): HTTP/1.1 persists unless it says "Connection: close";
// anything else (HTTP/1.0, or no version at all) closes unless it says
// "Connection: keep-alive". The header is a comma-separated token list, so
// match whole tokens rather than substrings.
bool wants_keep_alive(const Request& request) {
    std::string connection = request.get_header("connection");
    std::transform(connection.begin(), connection.end(), connection.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool says_close = false;
    bool says_keep_alive = false;
    std::size_t start = 0;
    while (start <= connection.size()) {
        std::size_t comma = connection.find(',', start);
        std::string token = connection.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token == "close") {
            says_close = true;
        } else if (token == "keep-alive") {
            says_keep_alive = true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (says_close) {
        return false;
    }
    if (request.http_version() != "HTTP/1.1") {
        return says_keep_alive;
    }
    return true;
}

// Whether this exchange is one we understand well enough to leave the
// connection open afterwards. Persistence is opt-in: wants_keep_alive says
// what the client asked for, this says whether honouring it is safe.
//
// The distinction matters because the cost of being wrong is asymmetric. A
// needless close costs one TCP handshake; persisting through an exchange we
// framed incorrectly desynchronizes the stream, and the client reads our
// leftover bytes as the head of its next response. Every framing gap this
// server has (or grows later) should therefore degrade into a reconnect, not
// a desync.
//
// Deliberately not re-checked here: the HTTP version and the Connection
// tokens (wants_keep_alive owns those, and this is ANDed with it), and
// Transfer-Encoding (read_request answers 501 and drops the connection
// before routing, since an undecodable body means we cannot know where the
// next request starts).
bool may_persist(const Request& request, const Response& response) {
    // An unknown method reaches the router as HttpMethod::UNKNOWN and is
    // answered with a normal bodied error. That is fine for the response
    // itself but wrong to persist through: HEAD is the common case, and a
    // HEAD client discards headers, expects no body per RFC 7231 §4.3.2, and
    // would read ours as its next response. Browsers, uptime monitors and
    // reverse-proxy health checks all send HEAD at the web UI.
    if (request.method() == HttpMethod::UNKNOWN) {
        return false;
    }
    // Without Content-Length the response is framed by connection close, so
    // it cannot share a connection with anything after it. Every router path
    // sets a body (and therefore a length) today, and Response::to_string()
    // now defaults the header when a handler does not, but this stays as the
    // structural guard: a future bodyless response must close, not desync.
    if (response.get_header("Content-Length").empty()) {
        return false;
    }
    return true;
}

// Read one full HTTP request: loop until the end-of-headers marker, then read
// exactly Content-Length body bytes (bounded by Request::kMaxBodyBytes).
// `raw_request` may arrive holding bytes left over from the previous request
// on a keep-alive connection (a pipelining client); any bytes past the end of
// this request are handed back in `surplus` for the next call.
// Returns false after sending an error response where possible (on a dead or
// timed-out socket nothing can be sent); the caller closes the connection.
//
// Every recv here runs under the per-request kSocketTimeoutSeconds bound set
// at accept time. The wait BETWEEN requests never happens in this function:
// a worker only reads a connection the reactor has already seen become
// readable (or one with carried bytes), so by the time we get here the
// request has begun arriving and deserves the same budget as request 1.
bool read_request(util::SocketHandle socket_fd, std::string& raw_request, std::string& surplus) {
    char buffer[8192];

    // Total-request wall-clock deadline. SO_RCVTIMEO bounds each individual
    // recv, but a peer trickling one byte per just-under-timeout interval
    // would pass every per-recv check and pin this worker indefinitely.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRequestDeadlineSeconds);

    // Skip any empty line(s) before the request line. RFC 7230 §3.5 says a
    // server SHOULD do this, and on a persistent connection it matters:
    // several client stacks leave a stray CRLF on the wire after a body, and
    // Request::parse rejects an empty request line. Before keep-alive those
    // bytes died with the connection; now they would 400 a session the client
    // believes is still good. Re-applied after every recv, since a chunk can
    // be nothing but padding.
    auto strip_leading_crlf = [&raw_request]() {
        std::size_t skip = 0;
        while (raw_request.compare(skip, 2, "\r\n") == 0) {
            skip += 2;
        }
        if (skip > 0) {
            raw_request.erase(0, skip);
        }
    };

    // Read until \r\n\r\n (end of headers); SO_RCVTIMEO bounds each recv.
    // Carried-over bytes may already hold the terminator, so look before the
    // first recv.
    strip_leading_crlf();
    std::size_t header_end = raw_request.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        // Enforce the header-size cap before reading more: without a
        // terminator the whole buffer is header so far.
        if (raw_request.size() > kMaxHeaderBytes) {
            send_error(socket_fd, 431, "Request Header Fields Too Large", "Request headers too large");
            return false;
        }
        int bytes_read = util::socket_recv(socket_fd, buffer, static_cast<int>(sizeof(buffer)));
        if (bytes_read <= 0) {
            // Peer closed, error, or receive timeout: drop the connection.
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            send_error(socket_fd, 408, "Request Timeout", "Request took too long to arrive");
            return false;
        }
        raw_request.append(buffer, static_cast<std::size_t>(bytes_read));
        strip_leading_crlf();
        header_end = raw_request.find("\r\n\r\n");
    }
    // The chunk that finds the terminator is size-checked too — otherwise it
    // could push the header block up to one recv buffer past the cap
    // unchecked. Only the bytes up to the terminator count as headers.
    if (header_end > kMaxHeaderBytes) {
        send_error(socket_fd, 431, "Request Header Fields Too Large", "Request headers too large");
        return false;
    }

    // Parse Content-Length (case-insensitive) out of the header block
    std::size_t content_length = 0;
    {
        std::string headers = raw_request.substr(0, header_end + 2);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // Reject Transfer-Encoding outright: this server frames request
        // bodies from Content-Length only, so a chunked body would be read
        // as a zero-length body and its chunk framing left on the wire. That
        // was harmless while every connection closed after one request; on a
        // persistent connection those bytes are parsed as the NEXT request,
        // which desynchronizes the stream (the client gets its response plus
        // a spurious 400) and is request-smuggling-shaped behind any
        // intermediary that does understand chunked. 501 is the honest
        // answer for a transfer coding we do not implement (RFC 7230 §3.3.1);
        // the connection closes because we cannot know where the body ended.
        if (headers.find("\r\ntransfer-encoding:") != std::string::npos) {
            send_error(socket_fd, 501, "Not Implemented", "Transfer-Encoding is not supported");
            return false;
        }
        auto pos = headers.find("\r\ncontent-length:");
        if (pos != std::string::npos) {
            // Reject duplicate Content-Length headers outright (RFC 7230 §3.3.2)
            // instead of one layer using the first and another the last.
            if (headers.find("\r\ncontent-length:", pos + 1) != std::string::npos) {
                send_error(socket_fd, 400, "Bad Request", "Duplicate Content-Length");
                return false;
            }
            pos += std::strlen("\r\ncontent-length:");
            auto eol = headers.find("\r\n", pos);
            std::string value = headers.substr(pos, eol - pos);
            // Trim surrounding whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            try {
                std::size_t consumed = 0;
                content_length = std::stoul(value, &consumed);
                if (consumed != value.size()) {
                    throw std::invalid_argument("trailing garbage");
                }
            } catch (...) {
                send_error(socket_fd, 400, "Bad Request", "Invalid Content-Length");
                return false;
            }
            if (content_length > Request::kMaxBodyBytes) {
                send_error(socket_fd, 413, "Payload Too Large", "Request body too large");
                return false;
            }
        }
    }

    // Read exactly Content-Length body bytes
    const std::size_t expected_total = header_end + 4 + content_length;
    while (raw_request.size() < expected_total) {
        std::size_t remaining = expected_total - raw_request.size();
        int chunk = static_cast<int>(std::min(remaining, sizeof(buffer)));
        int bytes_read = util::socket_recv(socket_fd, buffer, chunk);
        if (bytes_read <= 0) {
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            send_error(socket_fd, 408, "Request Timeout", "Request took too long to arrive");
            return false;
        }
        raw_request.append(buffer, static_cast<std::size_t>(bytes_read));
    }

    // The body loop never over-reads, but the header phase can pull in the
    // start of a pipelined next request; hand those bytes back to the caller.
    if (raw_request.size() > expected_total) {
        surplus.assign(raw_request, expected_total, std::string::npos);
        raw_request.resize(expected_total);
    }

    return true;
}

}  // namespace

// Serve exactly one request on `conn`. Returns KeepOpen when the connection
// may persist (the caller decides whether to keep serving carried bytes or
// hand it back to the reactor) and Close when it must end: the client asked,
// the request was malformed, a cap fired, the server is stopping, the
// exchange was not framed with certainty, or a send failed.
Server::ServeResult Server::serve_one_request(Connection& conn) {
    // Read request (headers, then exactly Content-Length body bytes). Bytes
    // read past the end of one request (a pipelining client) seed the next.
    std::string raw_request = std::move(conn.carried);
    conn.carried.clear();
    if (!read_request(conn.fd, raw_request, conn.carried)) {
        return ServeResult::Close;
    }

    // Parse request
    Request request;
    if (!request.parse(raw_request)) {
        send_error(conn.fd, 400, "Bad Request", "Invalid request");
        return ServeResult::Close;
    }
    request.set_remote_address(conn.remote_address);

    bool keep_alive = wants_keep_alive(request);
    ++conn.requests_served;
    const auto connection_age = std::chrono::steady_clock::now() - conn.opened_at;
    if (conn.requests_served >= kMaxRequestsPerConnection ||
        connection_age >= std::chrono::seconds(config_.keep_alive_lifetime_seconds())) {
        // Force a reconnect so one connection cannot stay persistent forever
        // -- by request count or by wall clock, whichever comes first. A
        // fresh TCP handshake at either bound is negligible next to the
        // per-request handshake this feature exists to avoid.
        keep_alive = false;
    }
    if (!running_) {
        // stop() has begun. The reactor is already refusing handoffs, so a
        // KeepOpen here would be closed anyway; saying so in the response
        // lets a polling client (NINA/PHD2) reconnect cleanly instead of
        // reading EOF on its next request.
        keep_alive = false;
    }

    // Generate transaction ID (thread-safe)
    static std::atomic<std::uint32_t> transaction_counter{0};
    std::uint32_t server_tx_id = ++transaction_counter;

    // Route request
    Response response = router_.route(request, server_tx_id);

    // Persistence is opt-in: whatever the client asked for, only keep the
    // connection open if this exchange is one we framed correctly.
    keep_alive = keep_alive && may_persist(request, response);

    // A handler that set its own Connection header can only narrow
    // keep_alive to false, never widen it back to true past the count/
    // lifetime caps above. Matched case-insensitively for consistency
    // with how the request-side Connection header is parsed in
    // wants_keep_alive -- no handler sets this today, but a
    // differently-cased "Keep-Alive" would otherwise be silently treated
    // as a close.
    const std::string& connection_header = response.get_header("Connection");
    if (!connection_header.empty()) {
        std::string lower_connection_header = connection_header;
        std::transform(lower_connection_header.begin(), lower_connection_header.end(), lower_connection_header.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        keep_alive = keep_alive && lower_connection_header == "keep-alive";
    }
    // Always rewrite the header to match the final decision (rather than
    // only setting it when absent) -- otherwise a handler that had set
    // "Connection: keep-alive" before the count/lifetime caps forced
    // keep_alive to false would leave that stale header on the wire: the
    // client would read "keep-alive" while the server closes the socket
    // right after sending, a protocol-violating response (review round
    // 3). Explicitly writing "close" here is identical to leaving the
    // header unset, since Response::to_string() defaults to "close".
    response.set_header("Connection", keep_alive ? "keep-alive" : "close");

    // Send response (loop until fully sent; MSG_NOSIGNAL prevents SIGPIPE)
    std::string response_str = response.to_string();
    if (!util::socket_send_all(conn.fd, response_str.c_str(), response_str.size())) {
        util::log_warning("Failed to send full response: " + util::socket_error_message(util::socket_get_last_error()));
        return ServeResult::Close;
    }

    return keep_alive ? ServeResult::KeepOpen : ServeResult::Close;
}

void Server::worker_thread(std::uint64_t generation) {
    // Counted by run_server() at spawn; this thread only ever decrements.
    while (true) {
        ConnectionPtr conn;

        // Wait for a permit: a queued connection, a stop, or a generation
        // change. The critical section below is only the pop.
        ready_signal_.acquire();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (worker_generation_ != generation) {
                // A newer generation of workers owns the queue now. This
                // thread was detached by a stop() it called itself (a
                // request handler that restarted the server synchronously);
                // whatever is queued belongs to the new pool.
                break;
            }
            if (shutdown_workers_ && ready_queue_.empty()) {
                // Shutdown requested and no more work
                break;
            }

            if (!ready_queue_.empty()) {
                conn = std::move(ready_queue_.front());
                ready_queue_.pop_front();
            }
        }
        if (!conn) {
            continue;
        }

        if (conn->close_only) {
            // The reactor gave up waiting on it (idle gap or lifetime cap).
            // Closed here, gracefully, so the reactor never blocks in a drain.
            close_connection(std::move(conn), true);
            continue;
        }

        // Serve the request the reactor saw arrive. Then keep going only
        // while there are buffered bytes from a pipelining client: the
        // reactor polls the socket and would never see them. A lone stray
        // CRLF after a body is not a request, so it is stripped before that
        // decision -- otherwise the worker would sit in recv waiting for a
        // request that may be seconds away, which is exactly the idle wait
        // the reactor exists to take off the pool.
        ServeResult result;
        do {
            result = serve_one_request(*conn);
            std::size_t skip = 0;
            while (conn->carried.compare(skip, 2, "\r\n") == 0) {
                skip += 2;
            }
            conn->carried.erase(0, skip);
        } while (result == ServeResult::KeepOpen && !conn->carried.empty());

        if (result != ServeResult::KeepOpen) {
            // Every closing exit of serve_one_request lands here: this and
            // the close_only branch above are the client-close sites for
            // served connections. Graceful, because with keep-alive the
            // peer often has its next request already in our receive queue
            // when we decide to stop, and a plain close() on a socket with
            // unread bytes sends RST.
            close_connection(std::move(conn), true);
            continue;
        }

        // Between requests the peer may legitimately go quiet; bound the
        // wait. Deliberately NOT clamped to the lifetime cap: closing an
        // idle connection the instant the cap passes races a polling
        // client's next request (it would meet EOF instead of a response,
        // and .NET HttpClient does not retry a PUT on a dead pooled
        // connection). The cap is enforced on the next RESPONSE instead,
        // with Connection: close, so the client reconnects cleanly; an idle
        // connection past the cap costs at most one more idle gap.
        conn->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kKeepAliveIdleSeconds);
        park_connection(std::move(conn));
    }
    worker_count_.fetch_sub(1, std::memory_order_relaxed);
}

// Hand an idle connection to the reactor. After stop() has begun the reactor
// no longer accepts, and the connection is closed here instead; nothing is
// ever left parked with no one to poll it.
void Server::park_connection(ConnectionPtr conn) {
    bool parked = false;
    {
        std::lock_guard<std::mutex> lock(reactor_mutex_);
        if (reactor_accepting_) {
            reactor_incoming_.push_back(std::move(conn));
            parked = true;
        }
    }
    if (!parked) {
        // Not parked: the reactor is gone. Graceful, since the peer may
        // already have sent its next request.
        close_connection(std::move(conn), true);
        return;
    }
    wake_reactor();
}

void Server::enqueue_ready(ConnectionPtr conn) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ready_queue_.push_back(std::move(conn));
    }
    ready_signal_.release();
}

// The single place a client socket is closed, so live_connections_ can never
// drift: every owner ends a connection through here.
void Server::close_connection(ConnectionPtr conn, bool graceful) {
    if (!conn) {
        return;
    }
    if (graceful) {
        util::socket_close_graceful(conn->fd);
    } else {
        util::socket_close(conn->fd);
    }
    conn->fd = util::kInvalidSocket;
    live_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void Server::wake_reactor() {
    const int fd = reactor_wake_fds_[1];
    if (fd < 0) {
        return;
    }
    const char byte = 0;
    // EAGAIN (pipe full) means a wake is already pending; any other failure
    // is a closed pipe during stop(), where the reactor is on its way out.
    (void)::write(fd, &byte, 1);
}

// open-astro#314: refresh the hardware-RTC probe on a timer instead of on
// whichever request arrives next. The paths that read its answer -- the
// ITelescopeV4 connect initiator and the description endpoint -- have a 1 s
// budget, and the probe reads /sys/class/rtc/rtcN/since_epoch, which is an
// I2C transaction on a bus-attached RTC and can block for about that long
// when the bus is wedged.
//
// Its own thread, not the reactor's: the reactor must not block in anything
// but poll() (AGENTS.md), and a second spent in the probe there would delay
// the next request of every parked keep-alive connection, plus their idle
// deadlines -- the same stall moved to a worse place. The probe settles after
// one successful read, so on a host with an RTC this costs nothing within a
// minute of start; on a host without one each pass is an opendir.
//
// The interval comes from Config (HostClock::kRtcProbeRateLimit + 1 s by
// default, deliberately not the limiter itself: equal periods race, and a
// pass landing a few microseconds early is silently swallowed, which would
// make the effective period 60 s). It is settable for the same reason the keep-alive
// cap is -- a test cannot wait half a minute to see the thread do its job.
void Server::rtc_probe_loop() {
    const auto interval = std::chrono::seconds(config_.rtc_probe_interval_seconds());
    while (true) {
        {
            std::unique_lock<std::mutex> lock(rtc_probe_mutex_);
            rtc_probe_cv_.wait_for(lock, interval, [this] { return rtc_probe_stop_; });
            if (rtc_probe_stop_) {
                return;
            }
        }
        router_.refresh_rtc_probe();
    }
}

// The reactor: parks idle connections on a poll set and hands each one to the
// worker pool the moment its next request begins to arrive. One thread, no
// request parsing, no blocking calls except poll(). A connection is here
// exactly when no worker holds it, which is what makes thread_pool_size mean
// concurrent requests rather than concurrent connections: a hundred idle
// clients cost a hundred pollfds and nothing else.
void Server::reactor_loop() {
    std::vector<ConnectionPtr> idle;
    std::vector<struct pollfd> pfds;
    const int wake_fd = reactor_wake_fds_[0];
    while (true) {
        // Take in what workers and the accept loop parked since last time,
        // and find out whether stop() has begun (after draining, so nothing
        // handed over in the meantime is lost).
        bool accepting = true;
        {
            std::lock_guard<std::mutex> lock(reactor_mutex_);
            for (auto& conn : reactor_incoming_) {
                idle.push_back(std::move(conn));
            }
            reactor_incoming_.clear();
            accepting = reactor_accepting_;
        }
        if (!accepting) {
            break;
        }

        // Poll every idle connection plus the wake pipe, sleeping no longer
        // than the nearest deadline (capped, so a clock oddity cannot park
        // the reactor for long).
        pfds.clear();
        pfds.push_back({wake_fd, POLLIN, 0});
        auto now = std::chrono::steady_clock::now();
        int timeout_ms = 1000;
        for (const auto& conn : idle) {
            pfds.push_back({conn->fd, POLLIN, 0});
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(conn->deadline - now).count();
            timeout_ms = static_cast<int>(std::min<long long>(timeout_ms, std::max<long long>(remaining, 0)));
        }

        const int ready = ::poll(pfds.data(), pfds.size(), timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            util::log_error("Reactor poll error: " + util::socket_error_message(errno));
            // Do not spin on a persistent error; the connections still get
            // their deadlines applied below.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (pfds[0].revents != 0) {
            char drain[64];
            while (::read(wake_fd, drain, sizeof(drain)) > 0) {
            }
        }

        // Readable (or hung up, or errored: the worker's recv will find out
        // and close) goes to the pool; expired goes to the pool marked
        // close-only; the rest stay parked.
        now = std::chrono::steady_clock::now();
        std::vector<ConnectionPtr> still_idle;
        still_idle.reserve(idle.size());
        for (std::size_t i = 0; i < idle.size(); ++i) {
            ConnectionPtr& conn = idle[i];
            const short revents = ready > 0 ? pfds[i + 1].revents : short{0};
            if ((revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
                enqueue_ready(std::move(conn));
            } else if (now >= conn->deadline) {
                conn->close_only = true;
                enqueue_ready(std::move(conn));
            } else {
                still_idle.push_back(std::move(conn));
            }
        }
        idle.swap(still_idle);
    }

    // stop(). One last look, without waiting: a request that is already on
    // the wire goes to the workers, which are still draining their queue and
    // will answer it with Connection: close (serve_one_request sees
    // running_ false).
    pfds.clear();
    for (const auto& conn : idle) {
        pfds.push_back({conn->fd, POLLIN, 0});
    }
    std::vector<ConnectionPtr> quiet;
    quiet.reserve(idle.size());
    const int last_ready = idle.empty() ? 0 : ::poll(pfds.data(), pfds.size(), 0);
    for (std::size_t i = 0; i < idle.size(); ++i) {
        ConnectionPtr& conn = idle[i];
        if (last_ready > 0 && (pfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            enqueue_ready(std::move(conn));
        } else {
            quiet.push_back(std::move(conn));
        }
    }

    // The rest were silent at that poll, but a request could land in the
    // microseconds between it and a close(), and close() with unread bytes
    // is an RST that makes the peer discard what it has not read. So close
    // them the way socket_close_graceful does, in parallel rather than one
    // 400 ms drain at a time: send FIN to all of them at once, give every
    // peer one shared window to react (a well-behaved client closes on FIN
    // and sends nothing after it), then drain without blocking and close.
    // This is what keeps stop() at ~100 ms no matter how many clients were
    // parked, instead of a per-connection wait through the exiting workers.
    for (const auto& conn : quiet) {
        ::shutdown(conn->fd, SHUT_WR);
    }
    if (!quiet.empty()) {
        pfds.clear();
        for (const auto& conn : quiet) {
            pfds.push_back({conn->fd, POLLIN, 0});
        }
        ::poll(pfds.data(), pfds.size(), 100);
    }
    for (auto& conn : quiet) {
        char sink[2048];
        for (int i = 0; i < 4; ++i) {
            if (::recv(conn->fd, sink, sizeof(sink), MSG_DONTWAIT) <= 0) {
                break;
            }
        }
        close_connection(std::move(conn), false);
    }
}

void Server::handle_shutdown_request() {
    bool expected = false;
    if (!shutdown_requested_.compare_exchange_strong(expected, true)) {
        util::log_info("Shutdown request already in progress, ignoring duplicate request");
        return;
    }

    util::log_info("Shutdown requested via management endpoint");

    std::function<void()> callback_copy;
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        callback_copy = shutdown_callback_;
    }

    if (callback_copy) {
        try {
            callback_copy();
        } catch (const std::exception& e) {
            util::log_error("Shutdown callback threw exception: " + std::string(e.what()));
        } catch (...) {
            util::log_error("Shutdown callback threw unknown exception");
        }
    }

    stop();
}

void Server::handle_restart_request() {
    bool expected = false;
    if (!restart_requested_.compare_exchange_strong(expected, true)) {
        util::log_info("Restart request already in progress, ignoring duplicate request");
        return;
    }

    util::log_info("Restart requested via management endpoint");

    std::function<void()> callback_copy;
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        callback_copy = restart_callback_;
    }

    if (callback_copy) {
        try {
            callback_copy();
        } catch (const std::exception& e) {
            util::log_error("Restart callback threw exception: " + std::string(e.what()));
        } catch (...) {
            util::log_error("Restart callback threw unknown exception");
        }
    }

    util::log_info("Restarting HTTP server");
    stop();
    start_async();
    restart_requested_ = false;
}

} // namespace alpacahttp

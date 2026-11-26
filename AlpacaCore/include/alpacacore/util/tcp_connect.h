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

// The bounded outbound TCP connect every mount wrapper uses (#2711): a host name lookup and a non-blocking connect
// that both end by one deadline, or earlier when a cancel flag is set, with a reason text for every failure. A bare
// blocking connect() to a host that never answers waits for the kernel's TCP timeout (~127 s) while the wrapper holds
// its lock. The result is a plain file descriptor: the caller owns it and does its own I/O (decision 0013 section 7).

#ifndef _WIN32

#include <alpacacore/util/serial_io.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::util {

struct TcpAddress {
    sockaddr_storage storage{};
    socklen_t length = 0;
    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&storage); }
};

// The addresses a host name resolves to, or why there are none (error not empty).
struct TcpLookup {
    std::vector<TcpAddress> addresses;
    std::string error;
};

// Resolves a host name (never a literal address) for a port and an address family (AF_INET, AF_INET6, AF_UNSPEC).
// The default is getaddrinfo; a test injects one that hangs.
using TcpResolver = std::function<TcpLookup(const std::string& host, int port, int family)>;

// One deadline for a whole connect; timeout_ms is the bound the failure text names.
struct TcpDeadline {
    std::chrono::steady_clock::time_point at;
    int timeout_ms = 0;
    static TcpDeadline in(int timeout_ms) {
        return {std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms), timeout_ms};
    }
};

// fd >= 0 on success (blocking, close-on-exec, TCP_NODELAY); otherwise fd is -1 and error says why.
struct TcpConnectResult {
    int fd = -1;
    std::string error;
};

namespace detail {

// The longest a wait runs before it looks at the cancel flag again.
inline constexpr std::chrono::milliseconds kTcpCancelSlice{50};

inline bool tcp_cancelled(const std::atomic<bool>* cancel) { return cancel != nullptr && cancel->load(); }

inline int tcp_slice_ms(const TcpDeadline& deadline) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline.at - std::chrono::steady_clock::now());
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(left.count(), 0, kTcpCancelSlice.count()));
}

inline TcpLookup getaddrinfo_resolver(const std::string& host, int port, int family) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    TcpLookup lookup;
    if (rc != 0) {
        lookup.error = std::string("host not found (") + ::gai_strerror(rc) + ")";
        return lookup;
    }
    for (const addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(sockaddr_storage)) continue;
        TcpAddress address;
        std::memcpy(&address.storage, ai->ai_addr, ai->ai_addrlen);
        address.length = static_cast<socklen_t>(ai->ai_addrlen);
        lookup.addresses.push_back(address);
    }
    ::freeaddrinfo(result);
    if (lookup.addresses.empty()) lookup.error = "host not found (no address)";
    return lookup;
}

// A literal IPv4 or IPv6 address of the wanted family, which needs no lookup.
inline bool tcp_literal(const std::string& host, int port, int family, TcpAddress& out) {
    const auto net_port = htons(static_cast<std::uint16_t>(port));
    if (family != AF_INET6) {
        sockaddr_in v4{};
        if (::inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1) {
            v4.sin_family = AF_INET;
            v4.sin_port = net_port;
            std::memcpy(&out.storage, &v4, sizeof(v4));
            out.length = sizeof(v4);
            return true;
        }
    }
    if (family != AF_INET) {
        sockaddr_in6 v6{};
        if (::inet_pton(AF_INET6, host.c_str(), &v6.sin6_addr) == 1) {
            v6.sin6_family = AF_INET6;
            v6.sin6_port = net_port;
            std::memcpy(&out.storage, &v6, sizeof(v6));
            out.length = sizeof(v6);
            return true;
        }
    }
    return false;
}

}  // namespace detail

// The addresses of @p host (a literal address, or a name resolved off the calling thread) by @p deadline. A
// resolver still running when the deadline passes or @p cancel is set is left to finish on its own thread, which
// touches nothing but its own copy of the request: getaddrinfo cannot be interrupted.
inline TcpLookup lookup_tcp_host(const std::string& host, int port, int family, const TcpDeadline& deadline,
                                 const std::atomic<bool>* cancel = nullptr, TcpResolver resolver = {}) {
    TcpLookup lookup;
    if (port < 1 || port > 65535) {
        lookup.error = "the TCP port is outside 1-65535";
        return lookup;
    }
    TcpAddress literal;
    if (detail::tcp_literal(host, port, family, literal)) {
        lookup.addresses.push_back(literal);
        return lookup;
    }
    if (!resolver) resolver = detail::getaddrinfo_resolver;

    struct Pending {
        std::mutex mutex;
        std::condition_variable done_cv;
        bool done = false;
        TcpLookup result;
    };
    auto pending = std::make_shared<Pending>();
    // Detached on purpose: it holds only shared_ptr-owned state, never a caller's object (see above).
    std::thread([pending, resolver = std::move(resolver), host, port, family] {
        TcpLookup result = resolver(host, port, family);
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->result = std::move(result);
        pending->done = true;
        pending->done_cv.notify_all();
    }).detach();

    std::unique_lock<std::mutex> lock(pending->mutex);
    while (!pending->done) {
        if (detail::tcp_cancelled(cancel)) {
            lookup.error = "cancelled";
            return lookup;
        }
        const int slice = detail::tcp_slice_ms(deadline);
        if (slice == 0) {
            lookup.error = "host name lookup did not finish within " + std::to_string(deadline.timeout_ms) + " ms";
            return lookup;
        }
        pending->done_cv.wait_for(lock, std::chrono::milliseconds(slice));
    }
    return std::move(pending->result);
}

// A connect to one resolved address by @p deadline, @p cancel checked every 50 ms.
inline TcpConnectResult connect_tcp_address(const TcpAddress& address, const TcpDeadline& deadline,
                                            const std::atomic<bool>* cancel = nullptr) {
    TcpConnectResult out;
    const auto fail = [&](const std::string& why) {
        if (out.fd >= 0) ::close(out.fd);
        out.fd = -1;
        out.error = why;
        return out;
    };
    out.fd = ::socket(address.storage.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (out.fd < 0) return fail(errno_string(errno));
    if (!set_nonblocking(out.fd)) return fail("the socket could not be made non-blocking");
    const int rc = ::connect(out.fd, address.get(), address.length);
    // EINTR: the connect goes on asynchronously, as EINPROGRESS (POSIX connect()).
    if (rc < 0 && errno != EINPROGRESS && errno != EINTR) {
        return fail(errno_string(errno));  // ECONNREFUSED on loopback, ENETUNREACH with no route
    }
    if (rc < 0) {
        pollfd pfd{};
        pfd.fd = out.fd;
        pfd.events = POLLOUT;
        while (true) {
            if (detail::tcp_cancelled(cancel)) return fail("cancelled");
            const int slice = detail::tcp_slice_ms(deadline);
            if (slice == 0) return fail("no answer within " + std::to_string(deadline.timeout_ms) + " ms");
            const int poll_rc = ::poll(&pfd, 1, slice);
            if (poll_rc > 0) break;
            if (poll_rc < 0 && errno != EINTR) return fail(errno_string(errno));
        }
        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        if (::getsockopt(out.fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) != 0) return fail(errno_string(errno));
        if (sock_err != 0) return fail(errno_string(sock_err));  // a refusal or a reset during the handshake
    }
    if (!clear_nonblocking(out.fd)) return fail("the socket could not be made blocking again");
    // Mount protocols send one small command and wait for its reply: no Nagle delay behind an unacked one.
    int one = 1;
    ::setsockopt(out.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return out;
}

// Looks up @p host and connects to its addresses in turn, all within @p timeout_ms. A refusal moves to the next
// address; the last failure is the error.
inline TcpConnectResult connect_tcp(const std::string& host, int port, int timeout_ms,
                                    const std::atomic<bool>* cancel = nullptr, TcpResolver resolver = {},
                                    int family = AF_UNSPEC) {
    const auto deadline = TcpDeadline::in(timeout_ms);
    const TcpLookup lookup = lookup_tcp_host(host, port, family, deadline, cancel, std::move(resolver));
    TcpConnectResult out;
    out.error = lookup.error;
    for (const auto& address : lookup.addresses) {
        out = connect_tcp_address(address, deadline, cancel);
        if (out.fd >= 0 || detail::tcp_slice_ms(deadline) == 0 || detail::tcp_cancelled(cancel)) break;
    }
    return out;
}

}  // namespace alpacacore::util

#endif  // !_WIN32

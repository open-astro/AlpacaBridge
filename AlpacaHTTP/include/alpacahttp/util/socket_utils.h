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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

namespace alpacahttp::util {

using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
using SocketLen = socklen_t;

inline void ensure_winsock() {}

inline int socket_close(SocketHandle handle) {
    return close(handle);
}

inline int socket_shutdown(SocketHandle handle) {
    return shutdown(handle, SHUT_RDWR);
}

inline int socket_get_last_error() {
    return errno;
}

inline std::string socket_error_message(int err) {
    return std::string(strerror(err));
}

inline int socket_select(SocketHandle handle, fd_set* read_fds, fd_set* write_fds, fd_set* except_fds, timeval* timeout) {
    return select(handle + 1, read_fds, write_fds, except_fds, timeout);
}

inline int socket_recv(SocketHandle handle, char* buffer, int length) {
    return static_cast<int>(recv(handle, buffer, static_cast<size_t>(length), 0));
}

inline int socket_send(SocketHandle handle, const char* buffer, int length) {
    return static_cast<int>(send(handle, buffer, static_cast<size_t>(length), 0));
}

// Set receive and send timeouts on an accepted client socket so a slow or
// stalled peer (slowloris) cannot pin a worker thread indefinitely. Returns
// false if either setsockopt fails.
inline bool socket_set_timeouts(SocketHandle handle, int seconds) {
    struct timeval tv {};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    bool ok = setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    ok = setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0 && ok;
    return ok;
}

// Bound only the receive side. Used between keep-alive requests, where the
// peer may legitimately go quiet for a while but must not pin a worker
// thread indefinitely; the send timeout set by socket_set_timeouts is kept.
inline bool socket_set_recv_timeout(SocketHandle handle, int seconds) {
    struct timeval tv {};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    return setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

// Send two buffers as one logical write without concatenating them. Keeping
// headers and bodies in one sendmsg call avoids the small-write Nagle/delayed-
// ACK stall while large bodies do not need a second full response copy.
inline bool socket_send_allv(SocketHandle handle, const char* first, std::size_t first_length, const char* second,
                             std::size_t second_length) {
    std::array<iovec, 2> vectors{
        {{const_cast<char*>(first), first_length}, {const_cast<char*>(second), second_length}}};
    std::size_t index = 0;
    while (index < vectors.size()) {
        while (index < vectors.size() && vectors[index].iov_len == 0) {
            ++index;
        }
        if (index == vectors.size()) {
            return true;
        }

        struct msghdr message {};
        message.msg_iov = vectors.data() + index;
        message.msg_iovlen = vectors.size() - index;
        const ssize_t sent = sendmsg(handle, &message, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }

        std::size_t remaining = static_cast<std::size_t>(sent);
        while (remaining > 0 && index < vectors.size()) {
            auto& vector = vectors[index];
            if (remaining < vector.iov_len) {
                vector.iov_base = static_cast<char*>(vector.iov_base) + remaining;
                vector.iov_len -= remaining;
                remaining = 0;
            } else {
                remaining -= vector.iov_len;
                ++index;
            }
        }
    }
    return true;
}

// Close a client connection without destroying data the peer has not read
// yet. close() on a socket that still has unread bytes queued makes the
// kernel send RST rather than FIN, and the peer's TCP stack then discards its
// whole receive buffer -- including a response we sent successfully but it
// has not read. Keep-alive makes that reachable in normal operation: at the
// request-count or lifetime cap, and on the stop()/restart path, a polling
// client has usually already written its next request when we decide to
// close, so the bytes sitting in our receive queue would turn the clean close
// we advertised into an ECONNRESET on the client's previous request.
//
// Shut down the write side so the peer sees EOF, drain what it already sent,
// then close.
//
// The drain budget is deliberately small. What has to be cleared is the bytes
// ALREADY queued when we decided to close -- typically the peer's next
// request, which recv returns immediately -- and after our FIN a well-behaved
// peer closes, so the loop normally ends on EOF within a millisecond or two.
// Waiting longer than that would hold a worker for the same reason the
// keep-alive reserve exists to avoid, so a peer that neither sends nor closes
// costs one short timeout rather than seconds.
inline void socket_close_graceful(SocketHandle handle) {
    if (handle == kInvalidSocket) {
        return;
    }
    shutdown(handle, SHUT_WR);

    struct timeval tv {};
    tv.tv_usec = 100000;  // 100 ms per recv
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char sink[2048];
    for (int i = 0; i < 4; ++i) {
        // <= 0 is EOF, error, or the timeout: nothing left worth draining.
        if (socket_recv(handle, sink, static_cast<int>(sizeof(sink))) <= 0) {
            break;
        }
    }
    socket_close(handle);
}

inline bool socket_interrupted(int err) {
    return err == EINTR;
}

inline bool socket_bad_descriptor(int err) {
    return err == EBADF;
}

inline bool socket_not_socket(int err) {
    return err == ENOTSOCK;
}

inline bool socket_would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}

} // namespace alpacahttp::util

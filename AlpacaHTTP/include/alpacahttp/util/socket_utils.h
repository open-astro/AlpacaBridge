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
#include <unistd.h>

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

// Send the whole payload, looping over short sends and retrying EINTR.
// MSG_NOSIGNAL is always passed so a peer drop mid-send returns an error
// instead of delivering SIGPIPE (which would kill the server). Mirrors
// util::send_all in AlpacaCore's serial_io.h (AlpacaHTTP cannot depend on
// AlpacaCore, so the loop is implemented locally). Returns false on any
// unrecoverable error, timeout (EAGAIN/EWOULDBLOCK from SO_SNDTIMEO), or a
// 0 return (no infinite spin).
inline bool socket_send_all(SocketHandle handle, const char* buffer, std::size_t length) {
    std::size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t n = send(handle, buffer + total_sent, length - total_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        total_sent += static_cast<std::size_t>(n);
    }
    return true;
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

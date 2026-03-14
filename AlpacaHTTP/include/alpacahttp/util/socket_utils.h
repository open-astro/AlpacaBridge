// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <mutex>
#include <string>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

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

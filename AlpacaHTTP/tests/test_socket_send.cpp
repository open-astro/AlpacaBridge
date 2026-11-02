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

// Real socket sends with test-only poll fault injection. No signal handlers or
// worker threads survive a case; CTest bounds each invocation to five seconds.
#include <alpacahttp/util/socket_utils.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "test_assert.h"

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int peer = -1;
int expire_result = 2;  // 2 means ordinary poll; -1, 0 and 1 inject expiry results.
int interruptions = 0;
Clock::time_point expires;

struct SocketPair {
    int fd[2]{};
    SocketPair() {
        EXPECT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
        peer = fd[1];
    }
    ~SocketPair() {
        ::close(fd[0]);
        ::close(fd[1]);
        peer = -1;
    }
    void fill() const {
        const int size = 4096;
        EXPECT(::setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
        const std::string bytes(4096, 'x');
        while (::send(fd[0], bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {
        }
        EXPECT(errno == EAGAIN || errno == EWOULDBLOCK);
    }
};
}  // namespace

extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (expire_result != 2) {
        // Simulate a poll wakeup delayed by scheduling past the response deadline.
        std::this_thread::sleep_until(expires + 5ms);
        char bytes[4096];
        while (::recv(peer, bytes, sizeof(bytes), MSG_DONTWAIT) > 0) {
        }
        fds[0].revents = POLLOUT;
        errno = EINTR;
        return expire_result;
    }
    if (interruptions > 0 && timeout > 50) {
        // Spend real poll time before a harmless EINTR, without making progress.
        EXPECT(__real_poll(fds, count, 50) == 0);
        --interruptions;
        errno = EINTR;
        return -1;
    }
    return __real_poll(fds, count, timeout);
}

int main(int argc, char** argv) {
    EXPECT(argc == 2);
    const std::string selected = argv[1];
    {
        // case: expired writable send writes no bytes
        if (selected == "expired") {
            SocketPair sockets;
            const bool sent =
                alpacahttp::util::socket_send_allv(sockets.fd[0], "head", 4, "body", 4, Clock::now() - 1ms, 100ms);
            EXPECT(!sent);
            EXPECT(errno == ETIMEDOUT);
            char byte;
            EXPECT(::recv(sockets.fd[1], &byte, 1, MSG_DONTWAIT) == -1);
            EXPECT(errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }
    {
        // case: expired poll retry writes no bytes
        if (selected == "retry") {
            for (const int result : {-1, 0, 1}) {
                SocketPair sockets;
                sockets.fill();
                expire_result = result;
                expires = Clock::now() + 20ms;
                const bool sent = alpacahttp::util::socket_send_allv(sockets.fd[0], "head", 4, "body", 4, expires, 1s);
                EXPECT(!sent);
                EXPECT(errno == ETIMEDOUT);
                char byte;
                EXPECT(::recv(sockets.fd[1], &byte, 1, MSG_DONTWAIT) == -1);
                EXPECT(errno == EAGAIN || errno == EWOULDBLOCK);
                expire_result = 2;
            }
        }
    }
    {
        // case: interrupted poll preserves the no-progress deadline
        if (selected == "interrupted") {
            SocketPair sockets;
            sockets.fill();
            interruptions = 4;
            const auto start = Clock::now();
            const bool sent =
                alpacahttp::util::socket_send_allv(sockets.fd[0], "head", 4, "body", 4, start + 2s, 100ms);
            const auto elapsed = Clock::now() - start;
            EXPECT(!sent);
            EXPECT(errno == ETIMEDOUT);
            EXPECT(interruptions < 4);
            EXPECT(elapsed >= 100ms);
            EXPECT(elapsed < 200ms);
        }
    }
    {
        // case: nonpositive stall retains response expiry and permits progress
        if (selected == "nonpositive") {
            for (const auto stall : {0ms, -1ms}) {
                SocketPair sockets;
                EXPECT(
                    alpacahttp::util::socket_send_allv(sockets.fd[0], "head", 4, "body", 4, Clock::now() + 1s, stall));
                char bytes[8];
                EXPECT(::recv(sockets.fd[1], bytes, sizeof(bytes), MSG_DONTWAIT) == 8);
                EXPECT(std::string(bytes, sizeof(bytes)) == "headbody");
                EXPECT(!alpacahttp::util::socket_send_allv(sockets.fd[0], "head", 4, "body", 4, Clock::now() - 1ms,
                                                           stall));
                EXPECT(errno == ETIMEDOUT);
                sockets.fill();
                interruptions = 4;
                const auto start = Clock::now();
                EXPECT(!alpacahttp::util::socket_send_allv(sockets.fd[0], "head", 4, "body", 4, start + 100ms, stall));
                EXPECT(errno == ETIMEDOUT);
                EXPECT(interruptions < 4);
                EXPECT(Clock::now() - start >= 100ms);
                EXPECT(Clock::now() - start < 200ms);
                interruptions = 0;
            }
        }
    }
    EXPECT(selected == "expired" || selected == "retry" || selected == "interrupted" || selected == "nonpositive");
    std::cout << selected << ": passed\n";
}

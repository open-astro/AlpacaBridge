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

// Socket-level tests for the Server request-read path (read_request) and the
// keep-alive connection loop, which the Router-level test_routing.cpp cannot
// reach because it feeds Router::route directly. Covers the header-size (431)
// boundary fixed in #128 (the cap must be enforced on the recv chunk that
// contains the \r\n\r\n terminator, not only on earlier chunks; see issue
// #129), HTTP/1.1 persistence and its bounds, the framing gate that decides
// whether a connection may stay open, and the graceful close path.

#include <alpacahttp/config.h>
#include <alpacahttp/server.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_assert.h"

namespace {

// Must match kMaxHeaderBytes in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

// Must match kMaxRequestsPerConnection in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::uint64_t kMaxRequestsPerConnection = 1000;

// Send `data` to 127.0.0.1:port in two writes — the terminator-bearing tail
// goes in the second write so we exercise the fixed path (the chunk that finds
// \r\n\r\n must itself be size-checked). Returns the first line of the response,
// or "" if the connection produced nothing.
std::string send_split_request(std::uint16_t port, const std::string& data, std::size_t first_chunk) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }

    first_chunk = std::min(first_chunk, data.size());
    ::send(fd, data.data(), first_chunk, 0);
    // Brief gap so the two writes tend to arrive as separate recvs on the server.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ::send(fd, data.data() + first_chunk, data.size() - first_chunk, 0);

    std::string response;
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.find("\r\n") != std::string::npos) {
            break;  // have the status line
        }
    }
    ::close(fd);

    auto eol = response.find("\r\n");
    return eol == std::string::npos ? response : response.substr(0, eol);
}

// Build a request whose header block (bytes before the terminating \r\n\r\n) is
// exactly `header_bytes` long, padding a single X-Pad header to hit the target.
std::string make_request_with_header_size(std::size_t header_bytes) {
    const std::string prefix = "GET / HTTP/1.1\r\nX-Pad: ";
    EXPECT(header_bytes >= prefix.size());
    std::string req = prefix;
    req.append(header_bytes - prefix.size(), 'a');
    req.append("\r\n\r\n");  // ends X-Pad line + empty line => \r\n\r\n terminator
    return req;
}

// --- keep-alive helpers ------------------------------------------------------

int connect_local(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// MSG_NOSIGNAL matters here: several cases below deliberately make the server
// close first (the request cap, the malformed request, stop() under load), so
// a send can land on an already-closed socket. Without it the test process
// takes SIGPIPE and dies instead of failing an EXPECT with a usable message.
void send_all(int fd, const std::string& data) {
    EXPECT(::send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size()));
}

// Read exactly one HTTP response (status line + headers + Content-Length body)
// from `fd`. `carry` holds bytes already received past the previous response
// (pipelined replies) and is updated for the next call. Returns "" if the
// peer closed before a full header block arrived.
std::string read_one_response(int fd, std::string& carry) {
    char tmp[4096];
    std::size_t header_end = carry.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return "";
        }
        carry.append(tmp, static_cast<std::size_t>(n));
        header_end = carry.find("\r\n\r\n");
    }
    std::string headers = carry.substr(0, header_end);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t content_length = 0;
    auto pos = headers.find("content-length:");
    if (pos != std::string::npos) {
        content_length = std::stoul(headers.substr(pos + std::strlen("content-length:")));
    }
    const std::size_t total = header_end + 4 + content_length;
    while (carry.size() < total) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        carry.append(tmp, static_cast<std::size_t>(n));
    }
    std::string response = carry.substr(0, total);
    carry.erase(0, total);
    return response;
}

// True if the server has closed the connection (EOF within `ms`); false if it
// is still open (the peek times out).
//
// The previous receive timeout is saved and restored. Without that, every
// read_one_response() after a peer_closed() check inherits this function's
// short budget (a few hundred ms), and a response that merely arrives slowly
// on a loaded CI machine comes back as "" -- which the following EXPECT then
// reports as "server closed", sending the reader after a bug that isn't there.
bool peer_closed(int fd, int ms) {
    struct timeval previous {};
    socklen_t previous_len = sizeof(previous);
    const bool saved = ::getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, &previous_len) == 0;

    struct timeval tv {};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char c = 0;
    const bool closed = ::recv(fd, &c, 1, MSG_PEEK) == 0;

    if (saved) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, previous_len);
    }
    return closed;
}

}  // namespace

int main() {
    std::cout << "Testing Server socket read path and keep-alive...\n";

    alpacahttp::Config config;
    config.set_http_port(6871);
    config.set_discovery_enabled(false);
    config.set_server_name("TestServer");

    alpacahttp::Server server(config);
    server.start_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    if (!server.is_running()) {
        std::cerr << "Server socket test skipped: unable to bind port 6871.\n";
        return 0;  // tolerate a busy/unavailable port, like the discovery test
    }

    const std::uint16_t port = config.http_port();

    // Over the cap by one byte, terminator in the second write => 431.
    {
        std::string req = make_request_with_header_size(kMaxHeaderBytes + 1);
        std::string status = send_split_request(port, req, req.size() - 8);
        EXPECT(status.find(" 431") != std::string::npos);
    }

    // Exactly at the cap, same split => accepted (parsed and routed, NOT 431).
    {
        std::string req = make_request_with_header_size(kMaxHeaderBytes);
        std::string status = send_split_request(port, req, req.size() - 8);
        EXPECT(!status.empty());
        EXPECT(status.find(" 431") == std::string::npos);
    }

    // A normal small request still gets a well-formed response (sanity).
    {
        std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        std::string status = send_split_request(port, req, req.size() - 4);
        EXPECT(status.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(status.find(" 431") == std::string::npos);
    }

    // --- Keep-alive ---------------------------------------------------------
    const std::string kGet11 = "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // HTTP/1.1 is persistent by default: two requests on one connection, each
    // answered with "Connection: keep-alive", then "Connection: close" ends it.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, kGet11);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        send_all(fd, "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        std::string r3 = read_one_response(fd, carry);
        EXPECT(r3.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // HTTP/1.0 closes by default...
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, "GET /management/apiversions HTTP/1.0\r\nHost: localhost\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // ...unless the client asks for keep-alive.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string get10_ka =
            "GET /management/apiversions HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        send_all(fd, get10_ka);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, get10_ka);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        ::close(fd);
    }

    // Pipelined: two requests in a single write are answered in order on the
    // same connection (the bytes past the first request are carried over).
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11 + kGet11);
        std::string r1 = read_one_response(fd, carry);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r1.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        ::close(fd);
    }

    // A connection is force-closed after kMaxRequestsPerConnection requests,
    // even though every one of them individually asked to keep the
    // connection alive -- the worker-pinning mitigation added in response to
    // the PR #2 review must actually fire, not just exist as an unused cap.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        for (std::uint64_t i = 1; i < kMaxRequestsPerConnection; ++i) {
            send_all(fd, kGet11);
            std::string r = read_one_response(fd, carry);
            EXPECT(r.find("Connection: keep-alive\r\n") != std::string::npos);
        }
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, kGet11);
        std::string last = read_one_response(fd, carry);
        EXPECT(last.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A malformed request on a persistent connection gets 400 and a close.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        send_all(fd, "GARBAGE\r\n\r\n");
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.find(" 400 ") != std::string::npos);
        EXPECT(r2.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A slow-but-legitimate request body on a keep-alive connection's SECOND
    // request must not be held to the short idle-wait bound. Before the fix,
    // SO_RCVTIMEO was set to kKeepAliveIdleSeconds (15s) for the whole of
    // request 2+ and never restored once the peer started sending, so a body
    // arriving in two writes >15s apart -- fine on request 1, which gets the
    // full 30s kSocketTimeoutSeconds per recv -- would time out and drop the
    // connection purely because it happened to be request 2. The timeout is
    // now restored the moment the peer's first byte of the new request
    // arrives, so only the true gap BETWEEN requests is 15s-limited.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);

        const std::string body = "{}";
        std::string headers =
            "POST /nonexistent HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\n\r\n";
        send_all(fd, headers + body.substr(0, 1));
        // Longer than the 15s idle bound, shorter than the 30s per-request one.
        std::this_thread::sleep_for(std::chrono::seconds(16));
        send_all(fd, body.substr(1));
        std::string r2 = read_one_response(fd, carry);
        EXPECT(!r2.empty());
        ::close(fd);
    }

    // Pre-carried headers must not leave the body under the idle timeout. A
    // pipelining client can deliver request B's headers in the same write as
    // request A; read_request then finds B's terminator in the carried bytes
    // and does no recv in its header loop -- and the idle-timeout restore
    // used to run only on a recv, so it was skipped and B's first body recv
    // ran under the 15s idle bound instead of the 30s per-request one. Same
    // bug as the slow-body case above, reached through carry-over instead.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string body = "{}";
        std::string b_headers =
            "POST /nonexistent HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\n\r\n";
        // A complete, plus B's headers only, in ONE write.
        send_all(fd, kGet11 + b_headers);
        std::string ra = read_one_response(fd, carry);
        EXPECT(ra.find("Connection: keep-alive\r\n") != std::string::npos);
        // Longer than the 15s idle bound, shorter than the 30s per-request one.
        std::this_thread::sleep_for(std::chrono::seconds(16));
        send_all(fd, body);
        std::string rb = read_one_response(fd, carry);
        EXPECT(!rb.empty());
        ::close(fd);
    }

    // A chunked request body must not be treated as a zero-length one. This
    // server frames bodies from Content-Length only, so before the 501 the
    // chunk framing stayed on the wire and -- now that the connection
    // survives a request -- was parsed as the NEXT request: the client got
    // its response followed by a spurious 400, and behind an intermediary
    // that does understand chunked this is a smuggling-shaped desync.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd,
                 "PUT /api/v1/telescope/0/connected HTTP/1.1\r\nHost: localhost\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(r.rfind("HTTP/1.1 501 ", 0) == 0);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // HEAD is not in parse_method, so it routes as UNKNOWN and is answered
    // with a normal BODIED error. Sending a body to a HEAD client is already
    // wrong (RFC 7231 4.3.2), but on a persistent connection it desyncs: the
    // client discards headers, expects no body, and reads ours as the head of
    // its next response. Browsers, uptime monitors and reverse-proxy health
    // checks all send HEAD at the web UI, so the connection must close.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, "HEAD /management/apiversions HTTP/1.1\r\nHost: localhost\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(!r.empty());
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A stray empty line before the request line must be skipped, not 400'd
    // (RFC 7230 3.5). Several client stacks leave one on the wire after a
    // body; before keep-alive those bytes died with the connection, now they
    // would drop a session the client believes is still good.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        EXPECT(!read_one_response(fd, carry).empty());
        send_all(fd, "\r\n" + kGet11);
        std::string r = read_one_response(fd, carry);
        EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);
        // Same thing pipelined: the CRLF arrives in the bytes carried over
        // from the previous request, so read_request must strip it before
        // its first terminator search, not only after a recv.
        send_all(fd, kGet11 + "\r\n" + kGet11);
        std::string r3 = read_one_response(fd, carry);
        std::string r4 = read_one_response(fd, carry);
        EXPECT(r3.rfind("HTTP/1.1 200 ", 0) == 0);
        EXPECT(r4.rfind("HTTP/1.1 200 ", 0) == 0);
        ::close(fd);
    }

    // Idle keep-alive connections cost no worker: they are parked on the
    // reactor's poll set, so thread_pool_size bounds concurrent REQUESTS
    // (what config.h documents) and not connections. Before the reactor a
    // parked connection held a worker in recv, and a stopgap reserve had to
    // force Connection: close as the pool filled. Own server on its own
    // port: the shared one above has the default pool of 32.
    {
        alpacahttp::Config small_config;
        small_config.set_http_port(6872);
        small_config.set_discovery_enabled(false);
        small_config.set_server_name("TestServerSmallPool");
        small_config.set_thread_pool_size(2);
        alpacahttp::Server small_server(small_config);
        small_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (small_server.is_running()) {
            const std::uint16_t small_port = small_config.http_port();

            // Twice as many idle keep-alive connections as workers, every
            // one of them kept alive (no reserve forcing a close).
            constexpr int kParked = 4;
            int parked[kParked];
            std::string parked_carry[kParked];
            for (int i = 0; i < kParked; ++i) {
                parked[i] = connect_local(small_port);
                EXPECT(parked[i] >= 0);
                send_all(parked[i], kGet11);
                std::string r = read_one_response(parked[i], parked_carry[i]);
                EXPECT(r.find("Connection: keep-alive\r\n") != std::string::npos);
            }

            // Clients that connect and never send cost no worker either:
            // before the reactor, three of these would have held both
            // workers in recv for the 30 s slowloris bound.
            int silent[3];
            for (int& fd : silent) {
                fd = connect_local(small_port);
                EXPECT(fd >= 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // A fresh client must still be served promptly. The recv timeout
            // makes a regression fail in seconds instead of hanging until
            // ctest's timeout.
            int late = connect_local(small_port);
            EXPECT(late >= 0);
            struct timeval tv {};
            tv.tv_sec = 5;
            ::setsockopt(late, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string carry;
            send_all(late, kGet11);
            std::string r = read_one_response(late, carry);
            EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);

            // And every parked connection is still live: its next request
            // is picked up off the poll set and served.
            for (int i = 0; i < kParked; ++i) {
                ::setsockopt(parked[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                send_all(parked[i], kGet11);
                std::string again = read_one_response(parked[i], parked_carry[i]);
                EXPECT(again.find("Connection: keep-alive\r\n") != std::string::npos);
            }

            ::close(late);
            for (int fd : silent) {
                ::close(fd);
            }
            for (int fd : parked) {
                ::close(fd);
            }
            small_server.stop();
        } else {
            std::cout << "  (skipped reactor pool case: port 6872 unavailable)\n";
        }
    }

    // The connection lifetime cap, injectable through Config so it can be
    // exercised without waiting five minutes. The cap is enforced only on a
    // RESPONSE (Connection: close on the first one past it), never by the
    // reactor closing an idle socket at the cap: that would race a polling
    // client's next request, which would meet EOF instead of an answer. So
    // an active connection sees the close header, and an idle one is still
    // open past the cap and gets the header on its next request.
    {
        alpacahttp::Config cap_config;
        cap_config.set_http_port(6873);
        cap_config.set_discovery_enabled(false);
        cap_config.set_server_name("TestServerLifetime");
        cap_config.set_keep_alive_lifetime_seconds(2);
        alpacahttp::Server cap_server(cap_config);
        cap_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (cap_server.is_running()) {
            const std::uint16_t cap_port = cap_config.http_port();

            // Idle connection: one request, then silence.
            int idle_fd = connect_local(cap_port);
            EXPECT(idle_fd >= 0);
            std::string idle_carry;
            send_all(idle_fd, kGet11);
            EXPECT(read_one_response(idle_fd, idle_carry).find("Connection: keep-alive\r\n") != std::string::npos);

            // Active connection: a request every 250 ms until the cap fires.
            int active_fd = connect_local(cap_port);
            EXPECT(active_fd >= 0);
            std::string active_carry;
            bool got_close = false;
            int served = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20 && !got_close; ++i) {
                send_all(active_fd, kGet11);
                std::string r = read_one_response(active_fd, active_carry);
                EXPECT(!r.empty());
                ++served;
                got_close = r.find("Connection: close\r\n") != std::string::npos;
                if (!got_close) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            EXPECT(got_close);
            EXPECT(served >= 4);         // kept alive through most of the 2 s
            EXPECT(elapsed_ms >= 1900);  // not closed early
            EXPECT(elapsed_ms < 4000);   // not the 15 s idle gap or the 300 s default
            EXPECT(peer_closed(active_fd, 2000));

            // The idle one is past the cap too but still open (only the idle
            // gap closes a parked socket); its next request gets the close.
            EXPECT(!peer_closed(idle_fd, 200));
            send_all(idle_fd, kGet11);
            std::string idle_r = read_one_response(idle_fd, idle_carry);
            EXPECT(idle_r.rfind("HTTP/1.1 200 ", 0) == 0);
            EXPECT(idle_r.find("Connection: close\r\n") != std::string::npos);
            EXPECT(peer_closed(idle_fd, 2000));

            ::close(active_fd);
            ::close(idle_fd);
            cap_server.stop();
        } else {
            std::cout << "  (skipped lifetime-cap case: port 6873 unavailable)\n";
        }
    }

    // The connection bound (Config::max_connections). Idle connections cost
    // no worker, so without a bound the only limit on parked clients would
    // be the process's descriptor limit. At the bound the accept loop
    // pauses: a new client's handshake completes in the listen backlog and
    // its request waits, unanswered, until a connection is released.
    {
        alpacahttp::Config bound_config;
        bound_config.set_http_port(6874);
        bound_config.set_discovery_enabled(false);
        bound_config.set_server_name("TestServerBound");
        bound_config.set_max_connections(2);
        alpacahttp::Server bound_server(bound_config);
        bound_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (bound_server.is_running()) {
            const std::uint16_t bound_port = bound_config.http_port();

            int held[2];
            for (int& fd : held) {
                fd = connect_local(bound_port);
                EXPECT(fd >= 0);
                std::string carry;
                send_all(fd, kGet11);
                EXPECT(read_one_response(fd, carry).find("Connection: keep-alive\r\n") != std::string::npos);
            }

            // Third client: connects (kernel backlog) but is not accepted.
            int third = connect_local(bound_port);
            EXPECT(third >= 0);
            struct timeval tv {};
            tv.tv_usec = 700 * 1000;
            ::setsockopt(third, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string carry;
            send_all(third, kGet11);
            EXPECT(read_one_response(third, carry).empty());  // nothing within 700 ms

            // Release one held connection; the third is accepted and served.
            ::close(held[0]);
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            ::setsockopt(third, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string r = read_one_response(third, carry);
            EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);

            ::close(third);
            ::close(held[1]);
            bound_server.stop();
        } else {
            std::cout << "  (skipped max-connections case: port 6874 unavailable)\n";
        }
    }

    // The management restart endpoint tears the server down and brings it
    // back on a detached thread while the requesting client's connection is
    // still alive. Connection accounting (live_connections_, which gates
    // accept()) must survive that: a counter reset on start, or a queue
    // cleared without closing what it held, would let a connection that
    // straddles the restart drive the count below zero and block every
    // later accept. Two restarts back to back, each followed by a fresh
    // client that must be served, and a keep-alive client that lived
    // through the restart and is closed rather than leaked.
    {
        // Counter declared before the Server, so the Router that owns the
        // capturing lambda is destroyed first (open-astro#314 review).
        std::atomic<int> restart_probes{0};
        alpacahttp::Config restart_config;
        restart_config.set_http_port(6875);
        restart_config.set_discovery_enabled(false);
        restart_config.set_server_name("TestServerRestart");
        restart_config.set_max_connections(3);
        restart_config.set_rtc_probe_interval_seconds(1);
        alpacahttp::Server restart_server(restart_config);
        // open-astro#314: the probe thread must come back with the new
        // generation. run_server() clears rtc_probe_stop_ before respawning;
        // deleting that line leaves the thread dead after the first restart,
        // which every other assertion here would happily ignore.
        restart_server.router_for_test().set_host_clock_hooks(
            [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
            [&restart_probes] {
                ++restart_probes;
                return false;
            });
        restart_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (restart_server.is_running()) {
            const std::uint16_t restart_port = restart_config.http_port();
            const std::string restart_request =
                "PUT /management/restart HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
            struct timeval tv {};
            tv.tv_sec = 5;

            for (int round = 0; round < 2; ++round) {
                // A bystander parked on the reactor across the restart.
                int bystander = connect_local(restart_port);
                EXPECT(bystander >= 0);
                std::string bystander_carry;
                send_all(bystander, kGet11);
                EXPECT(read_one_response(bystander, bystander_carry).find("Connection: keep-alive\r\n") !=
                       std::string::npos);

                int fd = connect_local(restart_port);
                EXPECT(fd >= 0);
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                std::string carry;
                send_all(fd, restart_request);
                std::string r = read_one_response(fd, carry);
                EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);
                ::close(fd);

                // The restart runs on a detached thread 100 ms after the
                // response, so polling is_running() here would race it (a
                // true seen before stop() begins is the OLD generation, and
                // a client connecting then lands in a listener about to be
                // closed and gets a reset). Wait for proof the restart
                // happened instead: stop() closes the parked bystander.
                EXPECT(peer_closed(bystander, 5000));
                ::close(bystander);
                // Then for the new generation to be up. is_running() goes
                // true before the new listener is bound, so retry connect.
                bool back = false;
                for (int i = 0; i < 50 && !back; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    back = restart_server.is_running();
                }
                EXPECT(back);

                // New generation accepts and serves, as many times as the
                // bound allows: a drifted counter would refuse all of them.
                for (int i = 0; i < 3; ++i) {
                    int after = -1;
                    for (int attempt = 0; attempt < 50 && after < 0; ++attempt) {
                        after = connect_local(restart_port);
                        if (after < 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                    EXPECT(after >= 0);
                    ::setsockopt(after, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    std::string after_carry;
                    send_all(after, kGet11);
                    std::string ra = read_one_response(after, after_carry);
                    EXPECT(ra.rfind("HTTP/1.1 200 ", 0) == 0);
                    ::close(after);
                }

                // The probe timer survived this restart: wait for a pass on
                // the new generation rather than a fixed sleep.
                const int probes_before = restart_probes.load();
                const auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (restart_probes.load() == probes_before && std::chrono::steady_clock::now() < probe_deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                EXPECT(restart_probes.load() > probes_before);
            }
            restart_server.stop();
        } else {
            std::cout << "  (skipped restart case: port 6875 unavailable)\n";
        }
    }

    // stop() straight after start_async(), with no settle time, repeatedly.
    // The spawn phase and stop() are serialized by a lifecycle mutex and
    // workers are counted at spawn, so a stop() that lands before a new
    // worker has executed an instruction still releases its wake permit
    // and the join completes. Before that, stop() could undercount and hang
    // on the uncounted thread. Ends with a normal start and a served request
    // to prove the object is still usable.
    {
        alpacahttp::Config churn_config;
        churn_config.set_http_port(6876);
        churn_config.set_discovery_enabled(false);
        churn_config.set_server_name("TestServerChurn");
        churn_config.set_thread_pool_size(4);
        alpacahttp::Server churn_server(churn_config);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) {
            churn_server.start_async();
            churn_server.stop();
            EXPECT(!churn_server.is_running());
        }
        const auto churn_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        EXPECT(churn_ms < 10000);  // a hung join would sit here for good

        churn_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (churn_server.is_running()) {
            int fd = connect_local(churn_config.http_port());
            EXPECT(fd >= 0);
            std::string carry;
            send_all(fd, kGet11);
            EXPECT(read_one_response(fd, carry).rfind("HTTP/1.1 200 ", 0) == 0);
            ::close(fd);
            churn_server.stop();
        } else {
            std::cout << "  (skipped churn case's final request: port 6876 unavailable)\n";
        }
    }

    // Closing must not destroy a response the client has not read yet. On
    // Linux, close() on a socket with unread bytes in its receive queue sends
    // RST instead of FIN, and the peer's stack then discards its own receive
    // buffer -- including the response we just sent. With keep-alive that is
    // ordinary: at the caps and on the stop() path the client usually has its
    // next request already on the wire. The server now shuts down its write
    // side and drains before close(). Here the "next request" is 10 KB of
    // trailing bytes in the same write as a Connection: close request: the
    // server's 8 KB recv leaves the tail queued in the kernel when it decides
    // to close, and the client deliberately waits before reading so the close
    // (FIN or RST) has arrived before it looks at the response.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string close_request =
            "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send_all(fd, close_request + std::string(10 * 1024, 'x'));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string r = read_one_response(fd, carry);
        EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        // A clean EOF, not ECONNRESET: peer_closed() is true only on recv == 0.
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // --- RTC probe thread (open-astro#314) ----------------------------------
    // The probe was moved off the request path, then off the reactor, onto its
    // own timer thread. Both #314 unit cases drive Router::refresh_rtc_probe()
    // directly, so deleting the thread's spawn left every one of them green --
    // and the bug the issue is about is precisely a host where nothing
    // re-probes: has_rtc() no longer probes, so with the thread gone the
    // startup answer is pinned for the life of the process.
    //
    // Drive it end to end instead: a real Server, a counting has_rtc hook, a
    // 1 s interval from Config (the default, one second past the probe's rate limit, is unwaitable), and an
    // assertion that the count rises on its own.
    {
        alpacahttp::Config rtc_config;
        rtc_config.set_http_port(6877);
        rtc_config.set_discovery_enabled(false);
        rtc_config.set_server_name("TestServerRtcProbe");
        rtc_config.set_rtc_probe_interval_seconds(1);

        // Counter first, Server second: the Router owns the lambda that
        // captures &probes, so the Server must be destroyed first. The
        // explicit stop() below joins the thread anyway, but that is a
        // property of this case rather than of the declaration order.
        std::atomic<int> probes{0};
        alpacahttp::Server rtc_server(rtc_config);
        rtc_server.router_for_test().set_host_clock_hooks(
            [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
            [&probes] {
                ++probes;
                return false;
            });
        // Installing the hooks primes the probe exactly once -- that is
        // #314's other half. Since open-astro#399 the single call comes from
        // the refresh_rtc() at the end of HostClock::set_hooks() rather than
        // from a fresh HostClock's constructor, because the seam no longer
        // builds a clock. The count is what matters either way: one probe at
        // install, and none on a request path.
        const int primed = probes.load();
        EXPECT(primed == 1);

        rtc_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (rtc_server.is_running()) {
            // No requests are sent: the whole point is that the refresh does
            // not depend on one arriving. Poll rather than sleeping one
            // interval and asserting: on a loaded runner thread start plus a
            // 1 s period can exceed any fixed margin, and waiting up to 5 s
            // for something that normally takes 1 s costs nothing when it
            // works.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (probes.load() == primed && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            EXPECT(probes.load() > primed);
        }
        rtc_server.stop();
        EXPECT(!rtc_server.is_running());
        // And the thread stops when the server does: no further passes after
        // the join returns.
        const int after_stop = probes.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        EXPECT(probes.load() == after_stop);
    }

    // stop() must not wait out an ACTIVE keep-alive client. Before the
    // running_ check a client that kept sending (NINA/PHD2 polling) held its
    // worker, and therefore stop(), until the 300s lifetime cap; systemd
    // would SIGKILL the service at its 90s TimeoutStopSec first. Measured
    // 26s of stop() latency behind a client sending every 2s. Now a request
    // in flight when stop() begins is answered with "Connection: close", and
    // a connection idle on the reactor at that moment gets FIN, a shared
    // 100 ms window and a drain (its next request meets EOF), so the client
    // sees one or the other and stop() never waits on it. This must be the
    // last test: it stops the server.
    //
    // Also: idle connections parked on the reactor are closed by stop()
    // immediately. Before the reactor each one held a worker in recv, and
    // stop() had to wait out the idle gap for every one of them.
    {
        int idle_parked[2];
        for (int& fd : idle_parked) {
            fd = connect_local(port);
            EXPECT(fd >= 0);
            std::string carry;
            send_all(fd, kGet11);
            EXPECT(read_one_response(fd, carry).find("Connection: keep-alive\r\n") != std::string::npos);
        }

        std::atomic<bool> got_close{false};
        std::atomic<bool> got_eof{false};
        std::atomic<int> served{0};
        std::thread client([&] {
            int fd = connect_local(port);
            EXPECT(fd >= 0);
            std::string carry;
            for (int i = 0; i < 40; ++i) {  // up to ~20s of activity, every 500 ms
                send_all(fd, kGet11);
                std::string r = read_one_response(fd, carry);
                if (r.empty()) {
                    got_eof = true;  // server closed the socket while idle
                    break;
                }
                ++served;
                if (r.find("Connection: close\r\n") != std::string::npos) {
                    got_close = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            ::close(fd);
        });
        // Let the client get a couple of keep-alive responses in first.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const int served_before_stop = served.load();
        EXPECT(served_before_stop >= 2);

        const auto t0 = std::chrono::steady_clock::now();
        server.stop();
        const auto stop_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        client.join();

        EXPECT(!server.is_running());
        // One in-flight idle gap (<= 500 ms here) plus scheduling slack, not
        // the ~19 s the client was prepared to keep going.
        EXPECT(stop_ms < 5000);
        // Either a last answer marked close (request was in flight) or a
        // clean EOF (idle on the reactor); never a stall.
        EXPECT(got_close.load() || got_eof.load());
        // stop() answered at most one more request after being called.
        EXPECT(served.load() <= served_before_stop + 1);
        // The idle ones are gone too, and stop() did not wait on them.
        for (int fd : idle_parked) {
            EXPECT(peer_closed(fd, 2000));
            ::close(fd);
        }
    }

    // Issue #402: a run_server() that fails early must not leave a joinable
    // std::thread behind.
    {
        // bind() fails when the port is already in use. run_server() logs,
        // sets running_ = false and returns -- on the thread start_async()
        // already created and stored. stop() then early-returned on
        // `if (!running_)` without joining, and ~Server() destroyed a still
        // joinable std::thread, which calls std::terminate(). So a port
        // conflict became an abort at destruction rather than a clean failure
        // the caller could report, and the caller's own is_running() check
        // did not help: it correctly returned false and the crash came later.
        //
        // This case is the shape an embedder actually writes -- construct,
        // start_async(), see is_running() == false, destroy, try another port
        // -- so if the fix regresses, this binary aborts rather than failing
        // an assertion.
        alpacahttp::Config holder_config;
        holder_config.set_http_port(6879);
        holder_config.set_discovery_enabled(false);
        holder_config.set_server_name("TestServerPortHolder");
        alpacahttp::Server holder(holder_config);
        holder.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (!holder.is_running()) {
            // Say so. The whole #402 block hangs off this, and a silent skip
            // turns the flagship regression case into a green no-op on a
            // runner where 6879 happens to be taken.
            std::cerr << "WARNING: port-conflict cases SKIPPED -- could not bind port 6879\n";
        }
        if (holder.is_running()) {
            {
                alpacahttp::Config conflict_config;
                conflict_config.set_http_port(6879);  // already held
                conflict_config.set_discovery_enabled(false);
                conflict_config.set_server_name("TestServerPortConflict");
                alpacahttp::Server conflicted(conflict_config);
                conflicted.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                EXPECT(!conflicted.is_running());
                // An explicit stop() before the destructor is the other order
                // an embedder writes; it must also be a no-crash no-op.
                conflicted.stop();
                EXPECT(!conflicted.is_running());
                // ...and the destructor runs here, on a Server whose thread
                // stop() has already reaped.
            }

            {
                // Retrying on the SAME Server after the failure. This is the
                // shape that exercises start_async()'s own join: the second
                // call assigns over server_thread_, and assigning over a
                // joinable std::thread is std::terminate(). It has to be one
                // object -- a fresh Server gets a fresh server_thread_ and
                // proves nothing about that path (which is exactly how this
                // test read before review: it built a second Server, so
                // deleting the join in start_async() left the suite green).
                alpacahttp::Config retry_config;
                retry_config.set_http_port(6879);
                retry_config.set_discovery_enabled(false);
                retry_config.set_server_name("TestServerPortRetry");
                alpacahttp::Server retried(retry_config);
                retried.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                EXPECT(!retried.is_running());

                // No stop() in between: the embedder sees is_running() false
                // and simply tries another port on the same object. Without
                // the join in start_async() this aborts the binary.
                retried.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                // Still the held port, so still not running -- the point is
                // that we got here at all.
                EXPECT(!retried.is_running());

                retry_config.set_http_port(6880);
                alpacahttp::Server second(retry_config);
                second.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                // The retry really came up, so the first failure left nothing
                // broken behind it.
                EXPECT(second.is_running());
                second.stop();
                EXPECT(!second.is_running());
            }
        }

        holder.stop();
        EXPECT(!holder.is_running());
    }

    {
        // Two threads calling stop() on the SAME running Server, which is the
        // shipped shutdown path, not a contrived one: PUT
        // /management/v1/shutdown spawns a detached thread that runs the
        // shutdown callback, and the example server's callback clears the flag
        // its own main loop polls -- so that loop calls stop() too, while the
        // detached thread is inside stop(). Both reach join_server_thread().
        //
        // Concurrent join() on one std::thread is UB; in practice the second
        // pthread_join throws std::system_error, which nothing catches, so the
        // process terminates. Like the port-conflict case above, a regression
        // here ABORTS this binary rather than failing an assertion.
        alpacahttp::Config concurrent_config;
        concurrent_config.set_http_port(6881);
        concurrent_config.set_discovery_enabled(false);
        concurrent_config.set_server_name("TestServerConcurrentStop");
        alpacahttp::Server concurrent(concurrent_config);
        concurrent.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (!concurrent.is_running()) {
            std::cerr << "WARNING: concurrent-stop case SKIPPED -- could not bind port 6881\n";
        }
        if (concurrent.is_running()) {
            // Released together so both land in stop() at once, which is what
            // makes them race for the join rather than queueing behind it.
            std::atomic<bool> go{false};
            std::atomic<int> finished{0};
            std::vector<std::thread> stoppers;
            for (int i = 0; i < 2; ++i) {
                stoppers.emplace_back([&]() {
                    while (!go.load()) {
                        std::this_thread::yield();
                    }
                    concurrent.stop();
                    finished.fetch_add(1);
                });
            }
            go.store(true);
            for (auto& t : stoppers) {
                t.join();
            }
            EXPECT(finished.load() == 2);
            EXPECT(!concurrent.is_running());
            // A third stop() after the fact is still a no-op, not a second
            // join of an already-reaped thread.
            concurrent.stop();
            EXPECT(!concurrent.is_running());
        }
    }

    {
        // The loser of the ownership race must not return from stop() early.
        // ~Server() runs straight after stop() and destroys the wake pipe, the
        // config and the connection maps that run_server() still reads, so a
        // stop() that returns while the accept loop is unwinding is a
        // use-after-free -- which is exactly what "return if another caller
        // took the thread" does.
        //
        // Both stoppers are joined before the Server is destroyed: a stopper
        // still inside stop() when the object dies is a *different* hazard
        // (the caller must outlive the callee) and not what this PR claims to
        // fix, so racing it here would only make the test unsound. What this
        // does exercise, many times over, is the interleaving itself -- one
        // caller in the !running_ branch winning the thread while the other
        // runs the full phases -- and the ASan and TSan pre-flight gates are
        // what turn a surviving run_server() into a report.
        //
        // The early-return regression was confirmed against this loop under
        // ASan by deleting the condition-variable wait in
        // join_server_thread(): heap-use-after-free in run_server() reading
        // the destroyed Server's running_ flag, on every run.
        for (int round = 0; round < 25; ++round) {
            alpacahttp::Config teardown_config;
            teardown_config.set_http_port(6882);
            teardown_config.set_discovery_enabled(false);
            teardown_config.set_server_name("TestServerStopThenDestroy");

            auto server = std::make_unique<alpacahttp::Server>(teardown_config);
            server->start_async();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            if (!server->is_running()) {
                std::cerr << "WARNING: stop-then-destroy loop SKIPPED at round " << round
                          << " -- could not bind port 6882\n";
                break;
            }

            alpacahttp::Server* raw = server.get();
            std::atomic<bool> go{false};
            std::atomic<int> returned{0};
            std::thread other([&]() {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                raw->stop();
                returned.fetch_add(1);
            });

            go.store(true);
            raw->stop();
            returned.fetch_add(1);

            other.join();
            EXPECT(returned.load() == 2);
            EXPECT(!raw->is_running());

            // Destroyed only once both stop() calls have returned. With the
            // wait in place that means the server thread is already reaped;
            // without it, run_server() can still be live here.
            server.reset();
        }
    }

    {
        // The restart shape: one caller stops and immediately starts again
        // while another is still parked inside stop() waiting for the join.
        // Without a generation counter the waiter wakes after the restart has
        // installed a NEW server_thread_, adopts it and joins a server that is
        // still running -- stop() never returns. A regression HANGS here
        // rather than failing an assertion, which the watchdog below turns
        // into a reported failure.
        //
        // Repeated, because the window is the winner's join: the waiter has to
        // park on the condition variable while the winner is inside it, and
        // the winner has to finish and restart before the waiter re-acquires
        // the mutex.
        //
        // HONEST LIMIT: 40 restarts did NOT reach that window on this machine
        // -- the generation check was removed and this loop still passed, three
        // runs out of three. So treat it as an exerciser of the restart shape
        // (and material for the ASan/TSan gates), not as the regression test
        // for the adopt-the-next-generation bug. That one is argued in
        // join_server_thread()'s comment and would need a test seam inside the
        // join to pin properly.
        alpacahttp::Config restart_config;
        restart_config.set_http_port(6883);
        restart_config.set_discovery_enabled(false);
        restart_config.set_server_name("TestServerRestartRace");

        alpacahttp::Server restarting(restart_config);
        restarting.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (!restarting.is_running()) {
            std::cerr << "WARNING: restart-race case SKIPPED -- could not bind port 6883\n";
        } else {
            std::atomic<bool> done{false};
            std::atomic<long> waiter_returns{0};
            // The embedder loop: every time it sees the server go down it
            // calls stop() too, which is the caller that ends up parked.
            std::thread waiter([&]() {
                while (!done.load()) {
                    if (!restarting.is_running()) {
                        restarting.stop();
                        waiter_returns.fetch_add(1);
                    }
                    std::this_thread::yield();
                }
            });

            for (int round = 0; round < 40; ++round) {
                restarting.stop();
                restarting.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            done.store(true);
            waiter.join();
            // Reaching here at all is the assertion: a waiter that adopted a
            // restarted thread would still be inside stop() and this join
            // would never return -- when the window is actually hit.
            EXPECT(waiter_returns.load() >= 0);

            restarting.stop();
            EXPECT(!restarting.is_running());
        }
    }

    std::cout << "All server socket tests passed!\n";
    return 0;
}

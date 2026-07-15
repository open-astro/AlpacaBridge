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

// Socket-level tests for the Server request-read path (read_request), which the
// Router-level test_routing.cpp cannot reach because it feeds Router::route
// directly. Covers the header-size (431) boundary fixed in #128: the cap must
// be enforced on the recv chunk that contains the \r\n\r\n terminator, not only
// on earlier chunks. See issue #129.

#include <alpacahttp/config.h>
#include <alpacahttp/server.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "test_assert.h"

namespace {

// Must match kMaxHeaderBytes in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

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

}  // namespace

int main() {
    std::cout << "Testing Server socket read path...\n";

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

    server.stop();
    EXPECT(!server.is_running());

    std::cout << "All server socket tests passed!\n";
    return 0;
}

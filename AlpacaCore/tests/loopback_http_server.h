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

// A loopback HTTP/1.1 server for tests that need real HTTP on the wire (issue #1753): the util::HttpClient
// libcurl tests and the WeeWX feed tests. It binds 127.0.0.1 on an ephemeral port, serves each connection
// on its own thread in turn, answers once and closes. One helper replaces the two the WeeWX and catalog
// tests each kept.
//
// Modes: Answer replies with the status line, extra headers and body set below; Stall holds the connection
// open without reading or replying (a hung server behind an open socket); DropAfterAccept closes with no bytes;
// ServerError answers 500 with no body. A responder, when set, builds the Answer reply's status line and body
// from each request (the fake Alpaca server in fake_alpaca_server.h).

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"

namespace alpacacore::test {

class LoopbackHttpServer {
public:
    enum class Mode { Answer, Stall, DropAfterAccept, ServerError };

    LoopbackHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        // Non-blocking, so a connection reset between poll() and accept() makes accept() fail with EAGAIN
        // instead of blocking the server thread and the destructor's join. An accepted fd does not inherit
        // the flag on Linux.
        const int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        REQUIRE(::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) == 0);
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~LoopbackHttpServer() {
        stop_ = true;
        thread_.join();
        for (const int fd : held_) {
            ::close(fd);
        }
        ::close(listen_fd_);
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    int port() const { return port_; }
    std::string url(const std::string& path = "/current") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    void set_mode(Mode mode) { mode_ = mode; }

    // The Answer reply. `status_line` is the part after "HTTP/1.1 ", `extra_headers` whole header lines each
    // ending in "\r\n". Without a declared length the body ends when the connection closes.
    void set_body(std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = std::move(body);
    }
    void set_status_line(std::string status_line) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_line_ = std::move(status_line);
    }
    void set_extra_headers(std::string headers) {
        std::lock_guard<std::mutex> lock(mutex_);
        extra_headers_ = std::move(headers);
    }
    void set_declare_length(bool declare) { declare_length_ = declare; }

    // Builds the Answer reply from the request read in full: {status line, body}. Called on the server
    // thread with no lock held. Replaces the fixed status line and body while set.
    using Responder = std::function<std::pair<std::string, std::string>(const std::string& request)>;
    void set_responder(Responder responder) {
        std::lock_guard<std::mutex> lock(mutex_);
        responder_ = std::move(responder);
    }

    // Connections accepted so far, in any mode.
    int requests() const { return requests_.load(); }

    // The last request read in full: request line, headers and any Content-Length body.
    std::string last_request() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_request_;
    }

private:
    static std::string read_request(int fd) {
        std::string request;
        char buf[2048];
        std::size_t header_end = std::string::npos;
        while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                return request;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        std::size_t want = 0;
        const std::size_t at = request.find("Content-Length: ");
        if (at != std::string::npos && at < header_end) {
            want = static_cast<std::size_t>(std::strtoul(request.c_str() + at + 16, nullptr, 10));
        }
        while (request.size() < header_end + 4 + want) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        return request;
    }

    void serve() {
        while (!stop_) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) {
                continue;
            }
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            ++requests_;
            const Mode mode = mode_;
            if (mode == Mode::DropAfterAccept) {
                ::close(fd);
                continue;
            }
            if (mode == Mode::Stall) {
                // Held before any read: a blocking recv on a client that never finishes its request would park
                // this thread and the destructor's join with it.
                held_.push_back(fd);
                continue;
            }
            const std::string request = read_request(fd);
            Responder responder;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                responder = responder_;
            }
            std::optional<std::pair<std::string, std::string>> built;
            if (mode == Mode::Answer && responder) {
                built = responder(request);
            }
            std::string reply;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_request_ = request;
                if (mode == Mode::Answer) {
                    const std::string& status_line = built ? built->first : status_line_;
                    const std::string& body = built ? built->second : body_;
                    reply = "HTTP/1.1 " + status_line + "\r\nContent-Type: application/json\r\n" + extra_headers_;
                    if (declare_length_) {
                        reply += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                    }
                    reply += "Connection: close\r\n\r\n" + body;
                }
            }
            if (mode == Mode::ServerError) {
                reply = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
            std::size_t sent = 0;
            while (sent < reply.size()) {
                const ssize_t n = ::send(fd, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<Mode> mode_{Mode::Answer};
    std::atomic<bool> declare_length_{true};
    std::atomic<int> requests_{0};
    std::vector<int> held_;  // serve() thread only, until the join
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string status_line_ = "200 OK";
    std::string extra_headers_;
    std::string body_;
    std::string last_request_;
    Responder responder_;
};

}  // namespace alpacacore::test

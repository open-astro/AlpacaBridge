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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace alpacahttp {

Server::Server(const Config& config)
    : config_(config)
{
    router_.set_shutdown_callback([this]() { handle_shutdown_request(); });
    router_.set_restart_callback([this]() { handle_restart_request(); });
    router_.set_server_info(config_.server_name(),
                            config_.manufacturer(),
                            alpacahttp::kVersion,
                            config_.location());
    router_.set_config_path(config_.config_path());
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
}

void Server::start() {
    if (running_) {
        return;
    }

    shutdown_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = false;
        while (!connection_queue_.empty()) {
            connection_queue_.pop();
        }
    }
    running_ = true;
    run_server();
}

void Server::start_async() {
    if (running_) {
        return;
    }

    shutdown_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = false;
        while (!connection_queue_.empty()) {
            connection_queue_.pop();
        }
    }
    running_ = true;
    server_thread_ = std::thread(&Server::run_server, this);
}

void Server::stop() {
    if (!running_) {
        return;
    }

    util::log_info("Stopping HTTP server...");
    running_ = false;
    
    // Shutdown worker threads
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = true;
    }
    queue_condition_.notify_all();
    
    // Wait for all worker threads to finish
    const auto current_id = std::this_thread::get_id();
    for (auto& thread : worker_threads_) {
        if (!thread.joinable()) {
            continue;
        }
        if (thread.get_id() == current_id) {
            thread.detach();
            continue;
        }
        thread.join();
    }
    worker_threads_.clear();
    
    // Shutdown and close the server socket to interrupt accept() call
    auto fd = server_fd_.exchange(util::kInvalidSocket);
    if (fd != util::kInvalidSocket) {
        util::socket_shutdown(fd);  // Shutdown before close to ensure accept() wakes up
        util::socket_close(fd);
    }
    
    if (server_thread_.joinable()) {
        if (server_thread_.get_id() == current_id) {
            server_thread_.detach();
        } else {
            server_thread_.join();
        }
    }
    util::log_info("HTTP server stopped");
}

void Server::wait() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void Server::run_server() {
    util::ensure_winsock();
    util::SocketHandle server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == util::kInvalidSocket) {
        util::log_error("Failed to create socket");
        running_ = false;
        return;
    }

    // Store server_fd so we can close it from stop()
    server_fd_.store(server_fd);

    // Set socket options
    int opt = 1;
    const char* opt_ptr = reinterpret_cast<const char*>(&opt);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, opt_ptr, sizeof(opt));

    // Bind socket
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    const int port = config_.http_port();
    if (port < 0 || port > static_cast<int>(std::numeric_limits<u_short>::max())) {
        util::log_error("Invalid HTTP port: " + std::to_string(port));
        util::socket_close(server_fd);
        server_fd_.store(util::kInvalidSocket);
        running_ = false;
        return;
    }
    address.sin_port = htons(static_cast<u_short>(port));

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        util::log_error("Failed to bind socket to port " + std::to_string(config_.http_port()));
        util::socket_close(server_fd);
        server_fd_.store(util::kInvalidSocket);
        running_ = false;
        return;
    }

    // Listen
    if (listen(server_fd, 10) < 0) {
        util::log_error("Failed to listen on socket");
        util::socket_close(server_fd);
        server_fd_.store(util::kInvalidSocket);
        running_ = false;
        return;
    }

    util::log_info("Server listening on port " + std::to_string(config_.http_port()));
    
    // Start worker thread pool for handling concurrent requests
    std::size_t pool_size = config_.thread_pool_size();
    worker_threads_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        worker_threads_.emplace_back(&Server::worker_thread, this);
    }
    util::log_info("Started " + std::to_string(pool_size) + " worker threads for concurrent request handling");

    // Accept connections using select() to allow checking running_ flag periodically
    while (running_) {
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
                // Socket was closed - exit
                break;
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
            struct sockaddr_in client_address;
            util::SocketLen client_len = sizeof(client_address);

            util::SocketHandle client_fd =
                accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_address), &client_len);
            if (client_fd == util::kInvalidSocket) {
                int err = util::socket_get_last_error();
                if (util::socket_interrupted(err) || util::socket_would_block(err)) {
                    // Interrupted or would block - continue
                    continue;
                } else if (util::socket_bad_descriptor(err) || util::socket_not_socket(err)) {
                    // Socket was closed - exit
                    break;
                } else {
                    if (running_) {
                        util::log_error("Failed to accept connection: " + util::socket_error_message(err));
                    }
                    continue;
                }
            }

            // Dispatch connection to worker thread pool for concurrent handling
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                connection_queue_.push(client_fd);
            }
            queue_condition_.notify_one();
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

// Per-connection socket timeout: a peer that stalls mid-request (slowloris)
// or mid-response is disconnected after this many seconds of inactivity.
constexpr int kSocketTimeoutSeconds = 30;

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

// Read the full HTTP request: loop until the end-of-headers marker, then read
// exactly Content-Length body bytes (bounded by Request::kMaxBodyBytes).
// Returns false after sending an error response where possible (on a dead or
// timed-out socket nothing can be sent); the caller closes the connection.
bool read_request(util::SocketHandle socket_fd, std::string& raw_request) {
    char buffer[8192];
    std::size_t header_end = std::string::npos;

    // Read until \r\n\r\n (end of headers); SO_RCVTIMEO bounds each recv
    while (true) {
        int bytes_read = util::socket_recv(socket_fd, buffer, static_cast<int>(sizeof(buffer)));
        if (bytes_read <= 0) {
            // Peer closed, error, or receive timeout — drop the connection
            return false;
        }
        raw_request.append(buffer, static_cast<std::size_t>(bytes_read));
        header_end = raw_request.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
        if (raw_request.size() > kMaxHeaderBytes) {
            send_error(socket_fd, 431, "Request Header Fields Too Large", "Request headers too large");
            return false;
        }
    }

    // Parse Content-Length (case-insensitive) out of the header block
    std::size_t content_length = 0;
    {
        std::string headers = raw_request.substr(0, header_end + 2);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
        raw_request.append(buffer, static_cast<std::size_t>(bytes_read));
    }

    return true;
}

}  // namespace

void Server::handle_connection(util::SocketHandle socket_fd) {
    // Bound how long a slow or stalled peer can hold this worker (slowloris)
    if (!util::socket_set_timeouts(socket_fd, kSocketTimeoutSeconds)) {
        // Fail closed: without recv/send timeouts this connection could pin a
        // worker thread forever (the slowloris hole the timeouts exist to plug).
        util::log_warning("Dropping connection, failed to set socket timeouts: " +
                          util::socket_error_message(util::socket_get_last_error()));
        return;
    }

    // Read request (headers, then exactly Content-Length body bytes)
    std::string raw_request;
    if (!read_request(socket_fd, raw_request)) {
        return;
    }

    // Parse request
    Request request;
    if (!request.parse(raw_request)) {
        send_error(socket_fd, 400, "Bad Request", "Invalid request");
        return;
    }

    // Generate transaction ID (thread-safe)
    static std::atomic<std::uint32_t> transaction_counter{0};
    std::uint32_t server_tx_id = ++transaction_counter;

    // Route request
    Response response = router_.route(request, server_tx_id);

    // Send response (loop until fully sent; MSG_NOSIGNAL prevents SIGPIPE)
    std::string response_str = response.to_string();
    if (!util::socket_send_all(socket_fd, response_str.c_str(), response_str.size())) {
        util::log_warning("Failed to send full response: " + util::socket_error_message(util::socket_get_last_error()));
    }
}

void Server::worker_thread() {
    while (true) {
        util::SocketHandle client_fd = util::kInvalidSocket;
        
        // Wait for a connection to handle
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_condition_.wait(lock, [this] {
                return !connection_queue_.empty() || shutdown_workers_;
            });
            
            if (shutdown_workers_ && connection_queue_.empty()) {
                // Shutdown requested and no more work
                break;
            }
            
            if (!connection_queue_.empty()) {
                client_fd = connection_queue_.front();
                connection_queue_.pop();
            }
        }
        
        if (client_fd != util::kInvalidSocket) {
            // Handle the connection
            handle_connection(client_fd);
            util::socket_close(client_fd);
        }
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

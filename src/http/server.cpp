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

#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace alpacahttp {

Server::Server(const Config& config)
    : config_(config)
{
}

void Server::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    router_.set_management_driver(mgmt_driver);
}

void Server::set_shutdown_callback(std::function<void()> callback) {
    router_.set_shutdown_callback(callback);
}

Server::~Server() {
    stop();
}

void Server::start() {
    if (running_) {
        return;
    }

    running_ = true;
    run_server();
}

void Server::start_async() {
    if (running_) {
        return;
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
    
    // Shutdown and close the server socket to interrupt accept() call
    int fd = server_fd_.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);  // Shutdown before close to ensure accept() wakes up
        close(fd);
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    util::log_info("HTTP server stopped");
}

void Server::wait() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void Server::run_server() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        util::log_error("Failed to create socket");
        running_ = false;
        return;
    }

    // Store server_fd so we can close it from stop()
    server_fd_.store(server_fd);

    // Set socket options
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(config_.http_port());

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        util::log_error("Failed to bind socket to port " + std::to_string(config_.http_port()));
        close(server_fd);
        server_fd_.store(-1);
        running_ = false;
        return;
    }

    // Listen
    if (listen(server_fd, 10) < 0) {
        util::log_error("Failed to listen on socket");
        close(server_fd);
        server_fd_.store(-1);
        running_ = false;
        return;
    }

    util::log_info("Server listening on port " + std::to_string(config_.http_port()));

    // Accept connections using select() to allow checking running_ flag periodically
    while (running_) {
        // Use select() to wait for connections with a timeout, so we can check running_ periodically
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;  // 500ms timeout
        
        int select_result = select(server_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result < 0) {
            // Error in select
            if (errno == EINTR) {
                // Interrupted by signal - continue
                continue;
            } else if (errno == EBADF || errno == ENOTSOCK) {
                // Socket was closed - exit
                break;
            } else {
                if (running_) {
                    util::log_error("Server select error: " + std::string(strerror(errno)));
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
            socklen_t client_len = sizeof(client_address);
            
            int client_fd = accept(server_fd, (struct sockaddr*)&client_address, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Interrupted or would block - continue
                    continue;
                } else if (errno == EBADF || errno == ENOTSOCK) {
                    // Socket was closed - exit
                    break;
                } else {
                    if (running_) {
                        util::log_error("Failed to accept connection: " + std::string(strerror(errno)));
                    }
                    continue;
                }
            }

            // Handle connection (synchronous for now)
            // TODO: Implement async/thread pool for concurrent requests
            handle_connection(client_fd);
            close(client_fd);
        }
    }

    // Clean up socket if not already closed
    int fd = server_fd_.exchange(-1);
    if (fd >= 0) {
        close(fd);
    }
    util::log_info("Server stopped");
}

void Server::handle_connection(int socket_fd) {
    // Read request
    char buffer[8192] = {0};
    ssize_t bytes_read = read(socket_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        return;
    }

    // Parse request
    Request request;
    if (!request.parse(std::string_view(buffer, bytes_read))) {
        Response error_response;
        error_response.set_status(400, "Bad Request");
        error_response.set_body("Invalid request");
        std::string response_str = error_response.to_string();
        write(socket_fd, response_str.c_str(), response_str.size());
        return;
    }

    // Generate transaction ID
    static std::uint32_t transaction_counter = 0;
    std::uint32_t server_tx_id = ++transaction_counter;

    // Route request
    Response response = router_.route(request, server_tx_id);

    // Send response
    std::string response_str = response.to_string();
    write(socket_fd, response_str.c_str(), response_str.size());
}

} // namespace alpacahttp


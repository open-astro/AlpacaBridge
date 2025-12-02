// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>

namespace alpacahttp {

Server::Server(const Config& config)
    : config_(config)
{
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

    running_ = false;
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void Server::wait() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void Server::run_server() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_error("Failed to create socket");
        running_ = false;
        return;
    }

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
        log_error("Failed to bind socket to port " + std::to_string(config_.http_port()));
        close(server_fd);
        running_ = false;
        return;
    }

    // Listen
    if (listen(server_fd, 10) < 0) {
        log_error("Failed to listen on socket");
        close(server_fd);
        running_ = false;
        return;
    }

    log_info("Server listening on port " + std::to_string(config_.http_port()));

    // Accept connections
    while (running_) {
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_address, &client_len);
        if (client_fd < 0) {
            if (running_) {
                log_error("Failed to accept connection");
            }
            continue;
        }

        // Handle connection (synchronous for now)
        // TODO: Implement async/thread pool for concurrent requests
        handle_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
    log_info("Server stopped");
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


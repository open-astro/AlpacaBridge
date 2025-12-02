// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#pragma once

#include "config.h"
#include "router.h"
#include <alpacacore/managementdriver.h>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>

namespace alpacahttp {

class Server {
public:
    explicit Server(const Config& config);
    ~Server();

    // Set management driver (from AlpacaCore)
    void set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver);

    // Start the server (blocking)
    void start();

    // Start the server in background thread
    void start_async();

    // Stop the server
    void stop();

    // Check if server is running
    bool is_running() const { return running_; }

    // Wait for server to stop
    void wait();

private:
    Config config_;
    Router router_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

    void run_server();
    void handle_connection(int socket_fd);
};

} // namespace alpacahttp


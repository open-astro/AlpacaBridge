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


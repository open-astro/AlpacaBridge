// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
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
#include <alpacahttp/util/socket_utils.h>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace alpacahttp {

class Server {
public:
    explicit Server(const Config& config);
    ~Server();

    // Set management driver (from AlpacaCore)
    void set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver);

    // Set shutdown callback (called when shutdown endpoint is requested)
    void set_shutdown_callback(std::function<void()> callback);
    // Set restart callback (called when restart endpoint is requested)
    void set_restart_callback(std::function<void()> callback);

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
    std::atomic<util::SocketHandle> server_fd_{util::kInvalidSocket};
    std::thread server_thread_;
    
    // Thread pool for handling concurrent requests
    std::vector<std::thread> worker_threads_;
    std::queue<util::SocketHandle> connection_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    bool shutdown_workers_{false};

    void run_server();
    void handle_connection(util::SocketHandle socket_fd);
    void worker_thread();
    void handle_shutdown_request();
    void handle_restart_request();

    std::function<void()> shutdown_callback_;
    std::mutex shutdown_mutex_;
    std::atomic<bool> shutdown_requested_{false};
    std::function<void()> restart_callback_;
    std::mutex restart_mutex_;
    std::atomic<bool> restart_requested_{false};
};

} // namespace alpacahttp

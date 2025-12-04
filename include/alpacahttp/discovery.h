// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include "config.h"
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>

namespace alpacahttp {

class Discovery {
public:
    explicit Discovery(const Config& config);
    ~Discovery();

    // Start discovery service
    void start();

    // Stop discovery service
    void stop();

    // Check if discovery is running
    bool is_running() const { return running_; }

private:
    Config config_;
    std::atomic<bool> running_{false};
    std::thread discovery_thread_;
    int socket_fd_ = -1;

    void run_discovery();
    void handle_probe(const std::string& probe_data, const std::string& sender_address, std::uint16_t sender_port);
    std::string build_response() const;

    static constexpr std::uint16_t ALPACA_DISCOVERY_PORT = 32227;
    static constexpr const char* ALPACA_DISCOVERY_MULTICAST_GROUP = "239.12.255.254";
};

} // namespace alpacahttp


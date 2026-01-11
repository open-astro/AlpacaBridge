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
#include <alpacahttp/util/socket_utils.h>
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
    util::SocketHandle socket_fd_ = util::kInvalidSocket;

    void run_discovery();
    void handle_probe(const std::string& probe_data, const std::string& sender_address, std::uint16_t sender_port);
    std::string build_response() const;

    static constexpr std::uint16_t ALPACA_DISCOVERY_PORT = 32227;
    static constexpr const char* ALPACA_DISCOVERY_MULTICAST_GROUP = "239.12.255.254";
};

} // namespace alpacahttp


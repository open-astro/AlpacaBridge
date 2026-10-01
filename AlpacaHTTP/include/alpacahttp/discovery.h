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

    // The HTTP port a probe reply advertises. Starts at config.http_port();
    // an embedder whose http_port is 0 sets the port the server actually
    // bound once it is known. While it is 0 the responder does not answer.
    // Safe to call from any thread, before or after start().
    void set_advertised_port(std::uint16_t port) { advertised_port_.store(port); }

private:
    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> advertised_port_;
    std::thread discovery_thread_;
    util::SocketHandle socket_fd_ = util::kInvalidSocket;

    void run_discovery();
    void handle_probe(const std::string& probe_data, const std::string& sender_address, std::uint16_t sender_port);
    std::string build_response() const;
    static bool is_discovery_probe(const std::string& datagram);

    static constexpr std::uint16_t ALPACA_DISCOVERY_PORT = 32227;
    static constexpr const char* ALPACA_DISCOVERY_MULTICAST_GROUP = "239.12.255.254";
};

} // namespace alpacahttp


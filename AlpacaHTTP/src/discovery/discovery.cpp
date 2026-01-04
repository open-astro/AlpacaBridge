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

#include <alpacahttp/discovery.h>
#include <alpacahttp/util/logging_adapter.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sstream>

namespace alpacahttp {

Discovery::Discovery(const Config& config)
    : config_(config)
{
}

Discovery::~Discovery() {
    stop();
}

void Discovery::start() {
    if (running_ || !config_.discovery_enabled()) {
        return;
    }

    running_ = true;
    discovery_thread_ = std::thread(&Discovery::run_discovery, this);
}

void Discovery::stop() {
    running_ = false;
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }
}

void Discovery::run_discovery() {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        util::log_error("Failed to create discovery socket");
        running_ = false;
        return;
    }

    // Set socket options for multicast
    int reuse = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to discovery port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ALPACA_DISCOVERY_PORT);

    if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        util::log_error("Failed to bind discovery socket");
        close(socket_fd_);
        socket_fd_ = -1;
        running_ = false;
        return;
    }

    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ALPACA_DISCOVERY_MULTICAST_GROUP);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        util::log_error("Failed to join multicast group");
        close(socket_fd_);
        socket_fd_ = -1;
        running_ = false;
        return;
    }

    util::log_info("Discovery service started on port " + std::to_string(ALPACA_DISCOVERY_PORT));

    // Listen for probes
    char buffer[1024];
    while (running_) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);
        
        ssize_t bytes_received = recvfrom(
            socket_fd_, buffer, sizeof(buffer) - 1, 0,
            (struct sockaddr*)&sender_addr, &sender_len
        );

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            std::string probe_data(buffer, bytes_received);
            std::string sender_address = inet_ntoa(sender_addr.sin_addr);
            std::uint16_t sender_port = ntohs(sender_addr.sin_port);
            
            handle_probe(probe_data, sender_address, sender_port);
        }
    }

    close(socket_fd_);
    socket_fd_ = -1;
    util::log_info("Discovery service stopped");
}

void Discovery::handle_probe(const std::string& probe_data, const std::string& sender_address, std::uint16_t sender_port) {
    // Alpaca discovery protocol: respond to "alpacadiscovery1" probe
    util::log_info("Discovery: Received probe from " + sender_address + ":" + std::to_string(sender_port));
    
    if (probe_data.find("alpacadiscovery1") != std::string::npos) {
        // Modern Alpaca spec: Send JSON format with AlpacaPort
        // This is what NINA and other modern clients expect
        std::ostringstream json_oss;
        json_oss << "{\"AlpacaPort\":" << config_.http_port() << "}";
        std::string json_response = json_oss.str();
        
        struct sockaddr_in target_addr;
        std::memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_addr.s_addr = inet_addr(sender_address.c_str());
        target_addr.sin_port = htons(sender_port);

        ssize_t sent = sendto(
            socket_fd_, json_response.c_str(), json_response.size(), 0,
            (struct sockaddr*)&target_addr, sizeof(target_addr)
        );
        
        if (sent > 0) {
            util::log_info("Discovery: Sent JSON response to " + sender_address + ":" + std::to_string(sender_port) + 
                          " (AlpacaPort=" + std::to_string(config_.http_port()) + ", " + std::to_string(sent) + " bytes)");
        } else {
            util::log_error("Discovery: Failed to send response to " + sender_address + ":" + std::to_string(sender_port) + " - " + std::string(strerror(errno)));
        }
    } else {
        util::log_warning("Discovery: Received non-Alpaca probe from " + sender_address + ":" + std::to_string(sender_port));
    }
}

std::string Discovery::build_response() const {
    // Alpaca discovery response format:
    // alpacadiscovery1\n
    // {Manufacturer}\n
    // {Location}\n
    // {ServerName}\n
    // {BaseURL}\n
    std::ostringstream oss;
    oss << "alpacadiscovery1\n";
    oss << config_.manufacturer() << "\n";
    oss << config_.location() << "\n";
    oss << config_.server_name() << "\n";
    oss << "/api/v1\n";
    return oss.str();
}

} // namespace alpacahttp

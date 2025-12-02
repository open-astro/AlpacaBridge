// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/discovery.h>
#include <alpacahttp/util/logging_adapter.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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
    if (!running_) {
        return;
    }

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
        log_error("Failed to create discovery socket");
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
        log_error("Failed to bind discovery socket");
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
        log_error("Failed to join multicast group");
        close(socket_fd_);
        socket_fd_ = -1;
        running_ = false;
        return;
    }

    log_info("Discovery service started on port " + std::to_string(ALPACA_DISCOVERY_PORT));

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
    log_info("Discovery service stopped");
}

void Discovery::handle_probe(const std::string& probe_data, const std::string& sender_address, std::uint16_t sender_port) {
    // Alpaca discovery protocol: respond to "alpacadiscovery1" probe
    if (probe_data.find("alpacadiscovery1") != std::string::npos) {
        std::string response = build_response();
        
        struct sockaddr_in target_addr;
        std::memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_addr.s_addr = inet_addr(sender_address.c_str());
        target_addr.sin_port = htons(sender_port);

        sendto(
            socket_fd_, response.c_str(), response.size(), 0,
            (struct sockaddr*)&target_addr, sizeof(target_addr)
        );
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


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

#include <alpacahttp/discovery.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/util/socket_utils.h>

#include <cstring>
#include <sstream>
#include <string_view>

namespace alpacahttp {

Discovery::Discovery(const Config& config) : config_(config), advertised_port_(config.http_port()) {}

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
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }
}

void Discovery::run_discovery() {
    util::ensure_winsock();
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ == util::kInvalidSocket) {
        util::log_error("Failed to create discovery socket");
        running_ = false;
        return;
    }

    // Share UDP 32227 with other Alpaca processes on this host: Linux and
    // macOS need SO_REUSEPORT as well as SO_REUSEADDR for that. A failed
    // option is logged and the bind still tried; it may succeed alone.
    int reuse = 1;
    const char* reuse_ptr = reinterpret_cast<const char*>(&reuse);
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, reuse_ptr, sizeof(reuse)) < 0) {
        util::log_warning("Discovery: failed to set SO_REUSEADDR: " +
                          util::socket_error_message(util::socket_get_last_error()));
    }
#ifdef SO_REUSEPORT
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEPORT, reuse_ptr, sizeof(reuse)) < 0) {
        util::log_warning("Discovery: failed to set SO_REUSEPORT: " +
                          util::socket_error_message(util::socket_get_last_error()));
    }
#endif

    // Bind to discovery port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ALPACA_DISCOVERY_PORT);

    if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        // Read errno before socket_close() can overwrite it.
        const int err = util::socket_get_last_error();
        util::log_error("Failed to bind discovery socket on UDP port " + std::to_string(ALPACA_DISCOVERY_PORT) + ": " +
                        util::socket_error_message(err) + "; discovery is disabled");
        util::socket_close(socket_fd_);
        socket_fd_ = util::kInvalidSocket;
        running_ = false;
        return;
    }

    // Join multicast group. This is optional — the ASCOM Alpaca discovery
    // protocol primarily uses UDP broadcast to 255.255.255.255:32227, which a
    // socket bound to INADDR_ANY:32227 already receives. Multicast join can
    // fail on AP/hotspot interfaces that lack a default multicast route; in
    // that case we must NOT tear down the listener, or broadcast-based clients
    // (NINA, PHD2 auto-discover) will see no server.
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ALPACA_DISCOVERY_MULTICAST_GROUP);
    mreq.imr_interface.s_addr = INADDR_ANY;
    const char* mreq_ptr = reinterpret_cast<const char*>(&mreq);
    if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq_ptr, sizeof(mreq)) < 0) {
        util::log_warning("Failed to join Alpaca multicast group " +
                          std::string(ALPACA_DISCOVERY_MULTICAST_GROUP) +
                          " (continuing with broadcast/unicast discovery only)");
    }

    util::log_info("Discovery service started on port " + std::to_string(ALPACA_DISCOVERY_PORT));

    // Listen for probes.
    // Use a periodic select timeout so stop() can terminate quickly on all platforms.
    char buffer[1024];
    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000; // 200 ms

        int select_result = util::socket_select(socket_fd_, &read_fds, nullptr, nullptr, &timeout);
        if (!running_) {
            break;
        }
        if (select_result == 0) {
            continue;
        }
        if (select_result < 0) {
            int err = util::socket_get_last_error();
            if (util::socket_interrupted(err)) {
                continue;
            }
            util::log_error("Discovery select failed: " + util::socket_error_message(err));
            break;
        }

        struct sockaddr_in sender_addr;
        util::SocketLen sender_len = sizeof(sender_addr);

        int bytes_received = static_cast<int>(recvfrom(socket_fd_, buffer, static_cast<int>(sizeof(buffer) - 1), 0,
                                                       reinterpret_cast<struct sockaddr*>(&sender_addr), &sender_len));

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            std::string probe_data(buffer, bytes_received);
            std::string sender_address = inet_ntoa(sender_addr.sin_addr);
            std::uint16_t sender_port = ntohs(sender_addr.sin_port);
            
            handle_probe(probe_data, sender_address, sender_port);
        } else if (bytes_received < 0) {
            int err = util::socket_get_last_error();
            if (util::socket_interrupted(err) || util::socket_would_block(err)) {
                continue;
            }
            util::log_error("Discovery recvfrom failed: " + util::socket_error_message(err));
            break;
        }
    }

    util::socket_close(socket_fd_);
    socket_fd_ = util::kInvalidSocket;
    util::log_info("Discovery service stopped");
}

bool Discovery::is_discovery_probe(const std::string& datagram) {
    // Discovery v1 defines the request as exactly the 16 ASCII bytes
    // "alpacadiscovery1". Trailing CR, LF, NUL or space is tolerated as well:
    // no client is known to send one, and refusing it would silently hide
    // this server from such a client. Anything else is not a probe (#562).
    static constexpr std::string_view kProbe = "alpacadiscovery1";
    std::string_view data(datagram);
    const auto last = data.find_last_not_of(std::string_view("\r\n\0 ", 4));
    data = last == std::string_view::npos ? std::string_view() : data.substr(0, last + 1);
    return data == kProbe;
}

void Discovery::handle_probe(const std::string& probe_data, const std::string& sender_address, std::uint16_t sender_port) {
    // Alpaca discovery protocol: respond to "alpacadiscovery1" probe
    util::log_info("Discovery: Received probe from " + sender_address + ":" + std::to_string(sender_port));

    if (is_discovery_probe(probe_data)) {
        // With http_port 0 the port is unknown until the embedder hands over
        // the one the server bound: no reply beats telling a client port 0.
        const std::uint16_t port = advertised_port_.load();
        if (port == 0) {
            return;
        }
        // Modern Alpaca spec: Send JSON format with AlpacaPort
        // This is what NINA and other modern clients expect
        std::ostringstream json_oss;
        json_oss << "{\"AlpacaPort\":" << port << "}";
        std::string json_response = json_oss.str();
        
        struct sockaddr_in target_addr;
        std::memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_addr.s_addr = inet_addr(sender_address.c_str());
        target_addr.sin_port = htons(sender_port);

        const int payload_len = static_cast<int>(json_response.size());
        int sent = static_cast<int>(sendto(socket_fd_, json_response.c_str(), payload_len, 0,
                                           reinterpret_cast<struct sockaddr*>(&target_addr), sizeof(target_addr)));

        if (sent > 0) {
            util::log_info("Discovery: Sent JSON response to " + sender_address + ":" + std::to_string(sender_port) +
                           " (AlpacaPort=" + std::to_string(port) + ", " + std::to_string(sent) + " bytes)");
        } else {
            int err = util::socket_get_last_error();
            util::log_error("Discovery: Failed to send response to " + sender_address + ":" +
                            std::to_string(sender_port) + " - " + util::socket_error_message(err));
        }
    } else {
        util::log_debug("Discovery: Received non-Alpaca probe from " + sender_address + ":" +
                        std::to_string(sender_port));
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

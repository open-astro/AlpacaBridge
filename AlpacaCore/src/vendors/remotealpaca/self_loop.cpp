// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Decision 0023 rule 4: the self-loop check of a proxied device. Addresses are compared as bytes, an IPv4-mapped IPv6
// address as the IPv4 address it carries, so "::ffff:127.0.0.1" is the loopback it names.

namespace alpacacore::vendor::remotealpaca {

namespace {

struct Address {
    int family = AF_UNSPEC;                 // AF_INET or AF_INET6
    std::array<unsigned char, 16> bytes{};  // AF_INET uses the first four

    bool operator==(const Address& other) const { return family == other.family && bytes == other.bytes; }
};

std::optional<Address> address_of(const sockaddr* sa) {
    if (sa == nullptr) return std::nullopt;
    Address a;
    if (sa->sa_family == AF_INET) {
        a.family = AF_INET;
        std::memcpy(a.bytes.data(), &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, 4);
        return a;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* v6 = &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(v6)) {
            a.family = AF_INET;
            std::memcpy(a.bytes.data(), v6->s6_addr + 12, 4);
        } else {
            a.family = AF_INET6;
            std::memcpy(a.bytes.data(), v6->s6_addr, 16);
        }
        return a;
    }
    return std::nullopt;
}

// Every address `host` resolves to; empty when it does not. `numeric` resolves only an IP literal (no lookup).
std::vector<Address> resolve(const std::string& host, bool numeric) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = numeric ? AI_NUMERICHOST : 0;
    addrinfo* found = nullptr;
    std::vector<Address> out;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0) return out;
    for (const addrinfo* it = found; it != nullptr; it = it->ai_next) {
        if (const auto a = address_of(it->ai_addr); a && std::find(out.begin(), out.end(), *a) == out.end()) {
            out.push_back(*a);
        }
    }
    ::freeaddrinfo(found);
    return out;
}

bool is_loopback(const Address& a) {
    if (a.family == AF_INET) return a.bytes[0] == 127;
    static const std::array<unsigned char, 16> kLoopback6{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return a.bytes == kLoopback6;
}

// 0.0.0.0 or ::. A connect to it reaches this host.
bool is_unspecified(const Address& a) {
    const std::size_t n = a.family == AF_INET ? 4 : 16;
    return std::all_of(a.bytes.begin(), a.bytes.begin() + static_cast<std::ptrdiff_t>(n),
                       [](unsigned char b) { return b == 0; });
}

std::vector<Address> local_interface_addresses() {
    std::vector<Address> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (const ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
        if (const auto a = address_of(it->ifa_addr)) out.push_back(*a);
    }
    ::freeifaddrs(list);
    return out;
}

std::string text_of(const Address& a) {
    char buffer[INET6_ADDRSTRLEN] = {};
    ::inet_ntop(a.family, a.bytes.data(), buffer, sizeof(buffer));
    return buffer;
}

bool is_wildcard(const std::string& host) {
    if (host.empty()) return true;
    const auto parsed = resolve(host, true);
    return parsed.size() == 1 && is_unspecified(parsed.front());
}

}  // namespace

std::optional<std::string> self_loop_refusal(const std::string& host, int port,
                                             const std::vector<ServerAddress>& self) {
    std::vector<const ServerAddress*> same_port;
    for (const auto& server : self) {
        if (server.port == port) same_port.push_back(&server);
    }
    if (same_port.empty()) return std::nullopt;
    const std::vector<Address> upstream = resolve(host, false);
    std::optional<std::vector<Address>> interfaces;  // read once, only for a wildcard bind
    for (const ServerAddress* server : same_port) {
        const bool wildcard = is_wildcard(server->host);
        const std::vector<Address> bound = wildcard ? std::vector<Address>{} : resolve(server->host, true);
        for (const Address& a : upstream) {
            bool self_address = is_unspecified(a);
            if (!self_address && wildcard) {
                if (!interfaces) interfaces = local_interface_addresses();
                self_address =
                    is_loopback(a) || std::find(interfaces->begin(), interfaces->end(), a) != interfaces->end();
            } else if (!self_address) {
                self_address = std::find(bound.begin(), bound.end(), a) != bound.end();
            }
            if (self_address) {
                return "The upstream " + host + ":" + std::to_string(port) + " is this server: " + text_of(a) +
                       " is one of its own addresses and " + std::to_string(port) +
                       " its HTTP port, so the proxied device would call itself in a loop. Set the host and port of "
                       "another Alpaca server.";
            }
        }
    }
    return std::nullopt;
}

}  // namespace alpacacore::vendor::remotealpaca

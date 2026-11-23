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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/vendor/celestron/celestron_aux_beacon.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

namespace alpacacore::vendor::celestron {

namespace {

constexpr const char* kLog = "Celestron";
// Any host on the LAN can send a beacon, so the list is capped; a real site has one or two adapters.
constexpr std::size_t kMaxAdapters = 16;

class UdpSocket {
public:
    UdpSocket() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {}
    ~UdpSocket() {
        if (fd_ >= 0) ::close(fd_);
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    int fd() const { return fd_; }

private:
    int fd_;
};

std::string_view skip_spaces(std::string_view text) {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    return text;
}

// The string value of `"key" : "value"` in the beacon's flat JSON object; nullopt when the key is absent or its
// value is not a plain string. The adapter's values carry no escapes, so a backslash makes the field unreadable.
std::optional<std::string> string_field(std::string_view payload, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    for (std::size_t at = payload.find(quoted); at != std::string_view::npos; at = payload.find(quoted, at + 1)) {
        std::string_view rest = skip_spaces(payload.substr(at + quoted.size()));
        if (rest.empty() || rest.front() != ':') continue;
        rest = skip_spaces(rest.substr(1));
        if (rest.empty() || rest.front() != '"') return std::nullopt;
        rest.remove_prefix(1);
        const std::size_t end = rest.find_first_of("\"\\");
        if (end == std::string_view::npos || rest[end] != '"') return std::nullopt;
        return std::string(rest.substr(0, end));
    }
    return std::nullopt;
}

std::string printable(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20 && byte < 0x7f && byte != '\\') {
            out += c;
        } else {
            std::array<char, 5> hex{};
            std::snprintf(hex.data(), hex.size(), "\\x%02x", byte);
            out += hex.data();
        }
    }
    return out;
}

int poll_ms(std::chrono::steady_clock::time_point deadline) {
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return 0;
    // Round up so a sub-millisecond remainder waits instead of spinning on a zero poll.
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
}

}  // namespace

std::vector<AuxBeacon> listen_for_aux_beacons(std::chrono::milliseconds window, int port) {
    if (window <= std::chrono::milliseconds::zero() || window > kAuxBeaconMaxWindow) {
        throw AlpacaException("Celestron SkyPortal beacon: the listen window must be between 1 and " +
                                  std::to_string(kAuxBeaconMaxWindow.count()) + " ms",
                              AlpacaError::InvalidValue);
    }
    if (port < 1 || port > 65535) {
        throw AlpacaException("Celestron SkyPortal beacon: the port must be between 1 and 65535",
                              AlpacaError::InvalidValue);
    }
    const std::string where = "UDP port " + std::to_string(port);
    UdpSocket socket;
    if (socket.fd() < 0) {
        throw AlpacaException(
            "Celestron SkyPortal beacon: cannot open a socket for " + where + ": " + util::errno_string(errno),
            AlpacaError::DriverException);
    }
    // A passive receive of the adapter's broadcast: nothing is sent and nothing is answered (decision 0023 rule 1
    // is about serving). SO_REUSEADDR lets it share the port with another listener; each gets a copy of a broadcast.
    const int on = 1;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<std::uint16_t>(port));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        ::bind(socket.fd(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        throw AlpacaException(
            "Celestron SkyPortal beacon: cannot listen on " + where + ": " + util::errno_string(errno),
            AlpacaError::DriverException);
    }

    std::vector<AuxBeacon> found;
    std::size_t ignored = 0;
    const auto deadline = std::chrono::steady_clock::now() + window;
    // The beacon is about 120 bytes; a longer datagram is cut to the buffer and judged on what is left.
    std::array<char, 512> buffer{};
    for (;;) {
        pollfd descriptor{};
        descriptor.fd = socket.fd();
        descriptor.events = POLLIN;
        const int wait = poll_ms(deadline);
        if (wait == 0) break;
        const int ready = ::poll(&descriptor, 1, wait);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        for (;;) {
            sockaddr_in from{};
            socklen_t from_size = sizeof(from);
            const ssize_t n = ::recvfrom(socket.fd(), buffer.data(), buffer.size(), 0,
                                         reinterpret_cast<sockaddr*>(&from), &from_size);
            if (n < 0) break;  // EAGAIN: drained; anything else ends this burst and the poll decides
            const std::string_view payload(buffer.data(), static_cast<std::size_t>(n));
            if (string_field(payload, "manufacturer") != "Celestron") {
                ++ignored;
                continue;
            }
            std::array<char, INET_ADDRSTRLEN> address{};
            if (::inet_ntop(AF_INET, &from.sin_addr, address.data(), address.size()) == nullptr) {
                ++ignored;
                continue;
            }
            const std::string ip(address.data());
            if (std::any_of(found.begin(), found.end(), [&](const AuxBeacon& b) { return b.ip == ip; })) continue;
            if (found.size() >= kMaxAdapters) {
                ++ignored;  // a new adapter past the cap: tallied, not listed (#2542)
                continue;
            }
            ALPACA_LOG_DEBUG(kLog, "SkyPortal beacon from " + ip + ": " + printable(payload));
            found.push_back(AuxBeacon{.ip = ip,
                                      .ssid = string_field(payload, "ssid").value_or(""),
                                      .module = string_field(payload, "module").value_or("")});
        }
    }
    ALPACA_LOG_DEBUG(kLog, "SkyPortal beacon listen on " + where + ": " + std::to_string(found.size()) +
                               " adapter(s), " + std::to_string(ignored) + " other datagram(s) ignored");
    return found;
}

}  // namespace alpacacore::vendor::celestron

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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace alpacacore::vendor::celestron {

namespace {

constexpr std::uint8_t kAuxStart = 0x3b;
constexpr std::size_t kMaximumFrameSize = 32;
constexpr auto kMaximumExchangeTimeout = std::chrono::seconds(5);
constexpr std::uint32_t kAuxPositionModulus = 1U << 24;

constexpr std::uint8_t kTcpSource = 0x20;                        // SkyPortal clients talk from 0x20; USB from 0x00
constexpr auto kTcpRetryAfter = std::chrono::milliseconds(300);  // rig 2026-11-20: WiFi reply p99 33 ms, max 134 ms
constexpr auto kTcpBacklogQuiet = std::chrono::milliseconds(200);
constexpr auto kTcpBacklogBound = std::chrono::seconds(2);
constexpr auto kUsbDefaultTimeout = std::chrono::milliseconds(500);
constexpr auto kTcpDefaultTimeout = std::chrono::milliseconds(1000);  // user decision 2: room for the 300 ms re-send

// The SkyPortal endpoints held by a TCP link, process-wide, so two devices never share one adapter: "host:port" to the
// holder's owner name. The mutex is a leaf under the wrapper's I/O lock: map operations only.
struct TcpEndpointRegistry {
    std::mutex mutex;
    std::map<std::string, std::string> holders;
};
TcpEndpointRegistry& tcp_endpoints() {
    static auto* registry = new TcpEndpointRegistry();  // NOLINT(cppcoreguidelines-owning-memory): exit order
    return *registry;
}

const char* link_prefix(bool tcp) { return tcp ? "Celestron CGX TCP: " : "Celestron CGX USB: "; }

[[noreturn]] void throw_link_error(bool tcp, const std::string& message) {
    throw AlpacaException(link_prefix(tcp) + message, AlpacaError::DriverException);
}

// A wire deadline that passed. Callers see the DriverException; only the TCP read retry tells it apart.
class AuxTimeout final : public AlpacaException {
public:
    using AlpacaException::AlpacaException;
};

[[noreturn]] void throw_timeout(bool tcp) {
    throw AuxTimeout(std::string(link_prefix(tcp)) + "AUX transaction timed out", AlpacaError::DriverException);
}

// The read-only motor queries, the only requests the TCP link re-sends: position, model, home done, slew done, guide
// active and version on the RA and Dec controllers. They change nothing, so a second copy is harmless.
bool retryable_query(const AuxFrame& request) {
    if ((request.destination != 0x10 && request.destination != 0x11) || !request.payload.empty()) return false;
    switch (request.command) {
        case 0x01:
        case 0x05:
        case 0x12:
        case 0x13:
        case 0x27:
        case 0xfe:
            return true;
        default:
            return false;
    }
}

// The bytes after the start byte sum to zero with their checksum.
bool checksum_valid(const std::vector<std::uint8_t>& bytes, std::size_t frame_size) {
    unsigned int sum = 0;
    for (std::size_t i = 1; i < frame_size; ++i) sum += bytes[i];
    return (sum & 0xffU) == 0;
}

// errno values that say the TCP connection itself is gone (decision 0009 rule 2a).
bool tcp_connection_gone(int code) {
    return code == ECONNRESET || code == EPIPE || code == ENOTCONN || code == EBADF || code == ENOTSOCK;
}

std::uint8_t axis_address(CgxAxis axis) {
    switch (axis) {
        case CgxAxis::RightAscension:
        case CgxAxis::Declination:
            return static_cast<std::uint8_t>(axis);
    }
    throw AlpacaException("Celestron CGX AUX axis is invalid", AlpacaError::InvalidValue);
}

void require_empty_ack(bool tcp, const AuxFrame& response, const char* operation) {
    if (!response.payload.empty()) {
        throw_link_error(tcp, std::string(operation) + " returned an unexpected payload");
    }
}

bool decode_axis_completion(bool tcp, const AuxFrame& response, const char* operation) {
    if (response.payload.size() != 1 || (response.payload[0] != 0x00 && response.payload[0] != 0xff)) {
        throw_link_error(tcp, std::string(operation) + " returned an invalid completion status");
    }
    return response.payload[0] == 0xff;
}

int poll_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    return static_cast<int>(std::max<std::int64_t>(1, milliseconds.count()));
}

}  // namespace

CelestronCgxProtocol::~CelestronCgxProtocol() { disconnect(); }

void CelestronCgxProtocol::connect_serial(const std::string& port_path, AuxLinkSpeed speed,
                                          std::chrono::milliseconds probe_wait) {
    if (port_path.empty()) {
        throw_link_error(false, "a serial port path is required");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    link_lost_.store(false);
    recovered_retries_.store(0);
    {
        std::lock_guard<std::mutex> loss(loss_mutex_);
        loss_reason_.clear();
    }
    // A connect, failed or done, starts with a clear verdict on the link too (the fd kept across a logical
    // disconnect included); a new port moved in below brings its own clear one.
    link_.clear_loss();
    std::error_code path_error;
    const std::filesystem::path canonical = std::filesystem::canonical(port_path, path_error);
    const std::string registry_path = path_error ? port_path : canonical.string();
    struct stat held {
    }, current{};
    if (serial_fd_ >= 0 && registry_path == link_.key() && ::fstat(serial_fd_, &held) == 0 &&
        ::stat(port_path.c_str(), &current) == 0 && held.st_nlink != 0 && held.st_dev == current.st_dev &&
        held.st_ino == current.st_ino && held.st_rdev == current.st_rdev) {
        if (desynchronized_) {
            // Recover matching without an ownership gap. The failed exchange
            // already advanced the session generation and invalidated references.
            if (::tcflush(serial_fd_, TCIFLUSH) != 0) {
                disconnect_locked();
                throw_link_error(false, "cannot clear stale AUX input before recovery");
            }
            receive_buffer_.clear();
            desynchronized_ = false;  // healthy_ stays false until a reply (decision 0009 rule 1)
        }
        return;
    }
    disconnect_locked();
    tcp_.store(false);  // a socket held before was closed above
    // SerialLink (#1202) claims the port (a port another connected device
    // holds is refused, naming it; another device's probe is waited out),
    // takes TIOCEXCL and keeps the fd non-blocking. The lock is Required: the
    // process-local registry cannot exclude another daemon's serial probe, and
    // decision 0011 keeps this fd across logical disconnects.
    const speed_t baud = speed == AuxLinkSpeed::Baud9600 ? B9600 : B115200;
    std::string error;
    util::SerialLink link = util::SerialLink::open(
        port_path, util::SerialClaimKind::Link, owner_,
        [baud](struct termios& settings) {
            ::cfmakeraw(&settings);
            settings.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
            settings.c_cflag |= CS8 | CREAD | CLOCAL;
            ::cfsetispeed(&settings, baud);
            ::cfsetospeed(&settings, baud);
        },
        &error, probe_wait, util::SerialExclusive::Required);
    if (!link) {
        if (util::serial_port_holder(port_path)) {
            throw AlpacaException("Celestron CGX USB: " + error, AlpacaError::InvalidOperation);
        }
        throw_link_error(false, error);
    }
    if (::tcflush(link.fd(), TCIFLUSH) != 0) {
        throw_link_error(false, "cannot clear stale AUX input before reconnect: " + util::errno_string(errno));
    }

    link_ = std::move(link);
    receive_buffer_.clear();
    serial_fd_ = link_.fd();
    ++session_generation_;
    healthy_.store(true);
}

void CelestronCgxProtocol::connect_tcp(const std::string& host, int port, std::chrono::milliseconds timeout) {
    if (port < 1 || port > 65535) {
        throw AlpacaException("Celestron CGX TCP: the port must be between 1 and 65535", AlpacaError::InvalidValue);
    }
    if (timeout <= std::chrono::milliseconds::zero() || timeout > kMaximumExchangeTimeout) {
        throw AlpacaException("Celestron CGX TCP: the connect timeout must be between 1 and 5000 ms",
                              AlpacaError::InvalidValue);
    }
    sockaddr_storage address{};
    socklen_t address_size = 0;
    auto* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
    if (::inet_pton(AF_INET, host.c_str(), &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(static_cast<std::uint16_t>(port));
        address_size = sizeof(sockaddr_in);
    } else if (::inet_pton(AF_INET6, host.c_str(), &ipv6->sin6_addr) == 1) {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(static_cast<std::uint16_t>(port));
        address_size = sizeof(sockaddr_in6);
    } else {
        throw AlpacaException("Celestron CGX TCP: the host must be a numeric IPv4 or IPv6 address",
                              AlpacaError::InvalidValue);
    }
    const std::string endpoint = "the adapter at " + host + " port " + std::to_string(port);
    // The claim key is the address as parsed, so two spellings of one IPv6 address are one endpoint.
    std::array<char, INET6_ADDRSTRLEN> text{};
    ::inet_ntop(address.ss_family,
                address.ss_family == AF_INET ? static_cast<const void*>(&ipv4->sin_addr) : &ipv6->sin6_addr,
                text.data(), text.size());
    const std::string key = std::string(text.data()) + ":" + std::to_string(port);

    std::lock_guard<std::mutex> lock(mutex_);
    link_lost_.store(false);
    recovered_retries_.store(0);
    {
        std::lock_guard<std::mutex> loss(loss_mutex_);
        loss_reason_.clear();
    }
    disconnect_locked();
    link_.clear_loss();
    {
        auto& registry = tcp_endpoints();
        std::lock_guard<std::mutex> claim(registry.mutex);
        const auto [holder, claimed] = registry.holders.emplace(key, owner_);
        if (!claimed) {
            throw AlpacaException("Celestron CGX TCP: " + endpoint + " is in use by " + holder->second,
                                  AlpacaError::InvalidOperation);
        }
    }
    tcp_endpoint_ = key;
    tcp_.store(true);
    try {
        const int fd = ::socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw_link_error(true, "cannot open a socket: " + util::errno_string(errno));
        }
        serial_fd_ = fd;
        // Non-blocking and bounded: this runs under the I/O lock, and a blocking connect to a host that is gone
        // would hold it for the kernel's ~127 s.
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
            if (errno != EINPROGRESS) {
                throw_link_error(true, "cannot connect to " + endpoint + ": " + util::errno_string(errno));
            }
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            pollfd descriptor{};
            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            int ready = 0;
            do {
                ready = ::poll(&descriptor, 1, poll_timeout_ms(deadline));
            } while (ready < 0 && errno == EINTR);
            if (ready == 0) {
                throw_link_error(true, "connecting to " + endpoint + " timed out");
            }
            int error = 0;
            socklen_t error_size = sizeof(error);
            if (ready < 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error != 0) {
                throw_link_error(
                    true, "cannot connect to " + endpoint + ": " + util::errno_string(error != 0 ? error : errno));
            }
        }
        drain_backlog_locked();
    } catch (...) {
        disconnect_locked();
        throw;
    }
    ++session_generation_;
    healthy_.store(true);
}

// The Gen4 adapter replays frames of an earlier session on a fresh connection (216 of them once, replies to source
// 0x20 among them, October rig log), and those would answer our first queries. Discard input until the socket has
// been quiet for kTcpBacklogQuiet, at most kTcpBacklogBound; exchange() discards any straggler before each send.
void CelestronCgxProtocol::drain_backlog_locked() {
    const auto bound = std::chrono::steady_clock::now() + kTcpBacklogBound;
    std::size_t discarded = 0;
    while (std::chrono::steady_clock::now() < bound) {
        pollfd descriptor{};
        descriptor.fd = serial_fd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1,
                                 poll_timeout_ms(std::min(std::chrono::steady_clock::now() + kTcpBacklogQuiet, bound)));
        if (ready == 0) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw_link_error(true, "socket poll failed: " + util::errno_string(errno));
        }
        discarded += discard_input_locked();
    }
    if (discarded > 0) {
        ALPACA_LOG_INFO("Celestron CGX", owner_ + ": discarded " + std::to_string(discarded) +
                                             " bytes the WiFi adapter sent before the first request");
    }
}

// TCP only: drops the receive buffer and every byte the socket holds now, without waiting (the startup backlog: its
// reports are stale too).
std::size_t CelestronCgxProtocol::discard_input_locked() {
    read_available_locked();
    const std::size_t discarded = receive_buffer_.size();
    receive_buffer_.clear();
    return discarded;
}

// TCP only: before a request is sent and on resync(), drops what the socket holds now, without waiting: bytes that
// arrive before a request is sent cannot be its reply (a duplicate after a re-send, a backlog straggler). A motor
// report among them is still handled, so it is never dropped unacknowledged.
void CelestronCgxProtocol::drain_input_locked(std::chrono::steady_clock::time_point deadline) {
    read_available_locked();
    while (const auto bytes = buffered_frame_locked()) {
        try {
            static_cast<void>(take_report_locked(decode_aux_frame(*bytes), deadline));
        } catch (const std::invalid_argument&) {
            continue;  // stale and undecodable: dropped like the rest
        }
    }
    receive_buffer_.clear();
}

// TCP only: appends every byte the socket holds now to the receive buffer, without waiting.
void CelestronCgxProtocol::read_available_locked() {
    std::array<std::uint8_t, 128> incoming{};
    while (true) {
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): MSG_DONTWAIT on an O_NONBLOCK socket
        const ssize_t count = ::recv(serial_fd_, incoming.data(), incoming.size(), MSG_DONTWAIT);
        if (count > 0) {
            receive_buffer_.insert(receive_buffer_.end(), incoming.begin(),
                                   incoming.begin() + static_cast<std::ptrdiff_t>(count));
        } else if (count == 0) {
            lose_link_locked("the adapter closed the connection");
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else if (errno != EINTR) {
            fail_socket_locked("recv", errno);
        }
    }
}

// A failed socket call: a connection that is gone is a lost link (decision 0009 rule 2a); any other error closes the
// socket, since a half-sent frame or an unknown stream state must not reach the next command.
void CelestronCgxProtocol::fail_socket_locked(const char* operation, int code) {
    const auto error = util::errno_string(code);
    if (tcp_connection_gone(code)) lose_link_locked(std::string(operation) + ": " + error);
    disconnect_locked();
    throw_link_error(true, std::string("socket ") + operation + " failed: " + error);
}

void CelestronCgxProtocol::resync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (serial_fd_ < 0) {
        throw AlpacaException("Celestron AUX link is not connected", AlpacaError::NotConnected);
    }
    if (!desynchronized_) {
        return;
    }
    if (tcp_.load()) {
        drain_input_locked(std::chrono::steady_clock::now() + kTcpRetryAfter);  // tcflush() fails on a socket
    } else if (::tcflush(serial_fd_, TCIFLUSH) != 0) {
        disconnect_locked();
        throw_link_error(tcp_.load(), "cannot clear stale AUX input before recovery");
    }
    receive_buffer_.clear();
    desynchronized_ = false;  // healthy_ stays false until a reply (decision 0009 rule 1)
}

void CelestronCgxProtocol::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect_locked();
    // An explicit disconnect forgets the link's verdict (decision 0009 rule 5); lose_link_locked(), the port release
    // after a loss, keeps it.
    link_.clear_loss();
}

std::string CelestronCgxProtocol::link_fault() const {
    std::lock_guard<std::mutex> lock(loss_mutex_);
    return loss_reason_;
}

bool CelestronCgxProtocol::is_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_fd_ >= 0;
}

AuxFrame CelestronCgxProtocol::exchange(const AuxFrame& request, Timeout requested) {
    const auto timeout = requested.value_or(tcp_.load() ? kTcpDefaultTimeout : kUsbDefaultTimeout);
    if (timeout <= std::chrono::milliseconds::zero() || timeout > kMaximumExchangeTimeout) {
        throw AlpacaException("Celestron CGX AUX timeout must be between 1 and 5000 ms", AlpacaError::InvalidValue);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (serial_fd_ < 0) {
        throw AlpacaException(tcp_.load() ? "Celestron CGX TCP is not connected" : "Celestron CGX USB is not connected",
                              AlpacaError::NotConnected);
    }
    const bool tcp = tcp_.load();
    AuxFrame wire = request;
    if (tcp) wire.source = kTcpSource;  // the source is the transport's, so callers build one frame for both
    if (desynchronized_) {
        throw_link_error(tcp_.load(), "AUX response timed out previously; reconnect before issuing another command");
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeout;
    const auto log_failure = [&](std::string_view reason) noexcept {
        try {
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
            ALPACA_LOG_ERROR("Celestron CGX", "AUX transaction failed: source=" + std::to_string(wire.source) +
                                                  " destination=" + std::to_string(wire.destination) +
                                                  " command=" + std::to_string(wire.command) +
                                                  " elapsed_ms=" + std::to_string(elapsed.count()) + " timeout_ms=" +
                                                  std::to_string(timeout.count()) + " error=" + std::string(reason));
        } catch (...) {
            // Diagnostics must never replace the original transaction failure.
            return;
        }
    };
    std::vector<std::uint8_t> request_bytes;
    try {
        request_bytes = encode_aux_frame(wire);
    } catch (const std::invalid_argument& error) {
        throw_link_error(tcp_.load(), std::string("invalid AUX request: ") + error.what());
    }
    bool request_started = false;
    try {
        request_started = true;
        if (tcp) drain_input_locked(deadline);
        write_frame_locked(request_bytes, deadline);
        // The SkyPortal adapter sometimes TCP-acks a request and never puts it on the bus (about 1 in 1,300 to
        // 2,900, rig 2026-11-20); one re-send of a read-only query at 300 ms recovered every one. A motion or write
        // command is never re-sent: AUX has no transaction id, so a second copy could act twice.
        bool resend = tcp && retryable_query(wire) && started + kTcpRetryAfter < deadline;
        auto wait_until = resend ? started + kTcpRetryAfter : deadline;
        bool resent = false;

        while (true) {
            std::vector<std::uint8_t> bytes;
            try {
                bytes = read_frame_locked(wait_until);
            } catch (const AuxTimeout&) {
                if (!resend) throw;
                resend = false;
                resent = true;
                wait_until = deadline;
                ALPACA_LOG_WARN("Celestron CGX",
                                "no reply over TCP in 300 ms: destination=" + std::to_string(wire.destination) +
                                    " command=" + std::to_string(wire.command) + "; re-sent once");
                write_frame_locked(request_bytes, deadline);
                continue;
            }
            const AuxFrame response = decode_aux_frame(bytes);
            if (tcp && take_report_locked(response, deadline)) continue;
            // The reply swaps our addresses: a request echo (AMW006) and traffic for other devices never match.
            if (response.source == wire.destination && response.destination == wire.source &&
                response.command == wire.command) {
                if (resent) ++recovered_retries_;  // a counter, not a fault (user decision 5)
                healthy_.store(true);
                return response;
            }
            // AUX carries no transaction id. Discard traffic for another address or
            // command while preserving the one deadline for this exchange.
        }
    } catch (const std::invalid_argument& error) {
        if (request_started) {
            log_failure(error.what());
            desynchronized_ = true;
            ++session_generation_;
            healthy_.store(false);
        }
        throw_link_error(tcp_.load(), std::string("invalid AUX response: ") + error.what());
    } catch (...) {
        if (request_started) {
            try {
                throw;
            } catch (const std::exception& error) {
                log_failure(error.what());
            } catch (...) {
                log_failure("non-standard exception");
            }
            desynchronized_ = true;
            ++session_generation_;
            healthy_.store(false);
        }
        throw;
    }
}

std::uint32_t CelestronCgxProtocol::get_axis_position(CgxAxis axis, Timeout timeout) {
    const AuxFrame response =
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x01, .payload = {}}, timeout);
    if (response.payload.size() != 3) {
        throw_link_error(tcp_.load(), "MC_GET_POSITION returned a payload that is not 24 bits");
    }
    return (static_cast<std::uint32_t>(response.payload[0]) << 16) |
           (static_cast<std::uint32_t>(response.payload[1]) << 8) | static_cast<std::uint32_t>(response.payload[2]);
}

void CelestronCgxProtocol::set_axis_position(CgxAxis axis, std::uint32_t position, Timeout timeout) {
    if (position >= kAuxPositionModulus) {
        throw AlpacaException("Celestron CGX encoder position must fit in 24 bits", AlpacaError::InvalidValue);
    }
    const AuxFrame request{.source = 0x00,
                           .destination = axis_address(axis),
                           .command = 0x04,
                           .payload = {static_cast<std::uint8_t>(position >> 16),
                                       static_cast<std::uint8_t>(position >> 8), static_cast<std::uint8_t>(position)}};
    require_empty_ack(tcp_.load(), exchange(request, timeout), "MC_SET_POSITION");
}

void CelestronCgxProtocol::goto_axis_fast(CgxAxis axis, std::uint32_t position, Timeout timeout) {
    goto_axis(axis, position, 0x02, "MC_GOTO_FAST", timeout);
}

void CelestronCgxProtocol::goto_axis_slow(CgxAxis axis, std::uint32_t position, Timeout timeout) {
    goto_axis(axis, position, 0x17, "MC_GOTO_SLOW", timeout);
}

void CelestronCgxProtocol::goto_axis(CgxAxis axis, std::uint32_t position, std::uint8_t command, const char* operation,
                                     Timeout timeout) {
    if (position >= kAuxPositionModulus) {
        throw AlpacaException("Celestron CGX GOTO position must fit in 24 bits", AlpacaError::InvalidValue);
    }
    const AuxFrame request{.source = 0x00,
                           .destination = axis_address(axis),
                           .command = command,
                           .payload = {static_cast<std::uint8_t>(position >> 16),
                                       static_cast<std::uint8_t>(position >> 8), static_cast<std::uint8_t>(position)}};
    require_empty_ack(tcp_.load(), exchange(request, timeout), operation);
}

bool CelestronCgxProtocol::is_axis_slew_done(CgxAxis axis, Timeout timeout) {
    return decode_axis_completion(
        tcp_.load(),
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x13, .payload = {}}, timeout),
        "MC_SLEW_DONE");
}

void CelestronCgxProtocol::start_axis_home(CgxAxis axis, Timeout timeout) {
    require_empty_ack(
        tcp_.load(),
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x0b, .payload = {}}, timeout),
        "MC_LEVEL_START");
}

bool CelestronCgxProtocol::is_axis_home_done(CgxAxis axis, Timeout timeout) {
    return decode_axis_completion(
        tcp_.load(),
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x12, .payload = {}}, timeout),
        "MC_LEVEL_DONE");
}

void CelestronCgxProtocol::pulse_guide_axis(CgxAxis axis, int velocity_percent, int duration_centiseconds,
                                            Timeout timeout) {
    if (velocity_percent < -100 || velocity_percent > 100 || duration_centiseconds < 1 || duration_centiseconds > 255) {
        throw AlpacaException("Celestron CGX pulse guide rate or duration is out of range", AlpacaError::InvalidValue);
    }
    const auto velocity = static_cast<std::uint8_t>(static_cast<std::int8_t>(velocity_percent));
    const AuxFrame request{.source = 0x00,
                           .destination = axis_address(axis),
                           .command = 0x26,
                           .payload = {velocity, static_cast<std::uint8_t>(duration_centiseconds)}};
    require_empty_ack(tcp_.load(), exchange(request, timeout), "MC_AUX_GUIDE");
}

bool CelestronCgxProtocol::is_aux_guide_active(CgxAxis axis, Timeout timeout) {
    const AuxFrame response =
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x27, .payload = {}}, timeout);
    if (response.payload.size() != 1 || response.payload[0] > 1) {
        throw_link_error(tcp_.load(), "MC_AUX_GUIDE_ACTIVE returned an invalid status");
    }
    return response.payload[0] == 1;
}

// A motor controller's unsolicited report to this client (0x20) or to every client (0x00), forwarded by the adapter:
// 0x50 with payload 0 is a low battery; any other 0x50, and 0x51, a motion limit or motor error. Each is acknowledged
// (addresses swapped, same command, empty payload). A fault is a session error (decision 0011 rule 2): the stream is
// desynchronized and the session bumped before the throw, so motion needs FindHome again. False for any other frame.
bool CelestronCgxProtocol::take_report_locked(const AuxFrame& frame, std::chrono::steady_clock::time_point deadline) {
    if ((frame.command != 0x50 && frame.command != 0x51) || (frame.source != 0x10 && frame.source != 0x11) ||
        (frame.destination != kTcpSource && frame.destination != 0x00)) {
        return false;
    }
    write_frame_locked(
        encode_aux_frame(AuxFrame{
            .source = frame.destination, .destination = frame.source, .command = frame.command, .payload = {}}),
        deadline);
    const std::string axis = frame.source == 0x10 ? "RA" : "Dec";
    if (frame.command == 0x50 && frame.payload == std::vector<std::uint8_t>{0x00}) {
        ALPACA_LOG_WARN("Celestron CGX", owner_ + ": the " + axis + " motor controller reports a low battery");
        return true;
    }
    desynchronized_ = true;
    ++session_generation_;
    healthy_.store(false);
    throw_link_error(true, "the " + axis + " motor controller reports a motion limit or motor error (command " +
                               std::to_string(frame.command) + "); run FindHome before moving the mount again");
}

void CelestronCgxProtocol::send_stop(CgxAxis axis) {
    const auto address = axis_address(axis);
    std::lock_guard<std::mutex> lock(mutex_);
    if (serial_fd_ < 0) {
        throw AlpacaException(tcp_.load() ? "Celestron CGX TCP is not connected" : "Celestron CGX USB is not connected",
                              AlpacaError::NotConnected);
    }
    // No reply is read, so desynchronized_ does not stop it. A late ACK never matches a query (another command), and
    // the pre-send drain drops one that arrived before the next request.
    const std::uint8_t source = tcp_.load() ? kTcpSource : 0x00;
    const auto deadline = std::chrono::steady_clock::now() + kTcpRetryAfter;
    write_frame_locked(
        encode_aux_frame(AuxFrame{.source = source, .destination = address, .command = 0x24, .payload = {0}}),
        deadline);
    write_frame_locked(
        encode_aux_frame(AuxFrame{.source = source, .destination = address, .command = 0x06, .payload = {0, 0, 0}}),
        deadline);
}

void CelestronCgxProtocol::disconnect_locked() {
    healthy_.store(false);
    if (serial_fd_ >= 0) {
        ++session_generation_;
        if (tcp_.load()) ::close(serial_fd_);  // a serial fd belongs to link_
        serial_fd_ = -1;
    }
    link_.close();  // releases TIOCEXCL and the registry claim
    if (!tcp_endpoint_.empty()) {
        auto& registry = tcp_endpoints();
        std::lock_guard<std::mutex> claim(registry.mutex);
        registry.holders.erase(tcp_endpoint_);
        tcp_endpoint_.clear();
    }
    receive_buffer_.clear();
    desynchronized_ = false;
}

// Decision 0009 rule 2a: the port is gone. Release it so a replugged device can be reopened, then latch link_lost_ for
// the driver (its Connected getter, its poll and its connect) and fail with NotConnected. Caller holds mutex_ inside an
// exchange; the drivers call the wrapper with their own mutex_ free, so no driver lock is taken under this one. The
// errno verdict that leads here is the link's (link_.note_error(), util::SerialLink, #2262); the wrapper keeps its own
// latch and text because a hangup on poll() and an end of file lose the link with no errno, the drivers read the
// owner's text, and the latch is published only after the port is released.
void CelestronCgxProtocol::lose_link_locked(const std::string& reason) {
    const bool tcp = tcp_.load();
    ALPACA_LOG_ERROR("Celestron CGX",
                     owner_ + " link lost: " + reason +
                         (tcp ? " (socket closed; reconnect to resume)" : " (port released; reconnect to resume)"));
    {
        std::lock_guard<std::mutex> loss(loss_mutex_);
        loss_reason_ = owner_ + (tcp ? " TCP link lost (" : " serial link lost (") + reason + ")";
    }
    disconnect_locked();
    link_lost_.store(true);  // after the release: a reader that sees the flag finds the port free
    throw AlpacaException(link_fault(), AlpacaError::NotConnected);
}

void CelestronCgxProtocol::wait_for_locked(short events, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw_timeout(tcp_.load());
        }
        struct pollfd descriptor {};
        descriptor.fd = serial_fd_;
        descriptor.events = events;
        const int result = ::poll(&descriptor, 1, poll_timeout_ms(deadline));
        if (result > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw_timeout(tcp_.load());
            }
            // A socket can hold a last reply beside its FIN or RST; recv() and send() judge the connection.
            if (tcp_.load() && (descriptor.revents & events) != 0) {
                return;
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                lose_link_locked("hangup");
            }
            if ((descriptor.revents & events) != 0) {
                return;
            }
            continue;
        }
        if (result == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw_timeout(tcp_.load());
            }
            continue;
        }
        if (errno != EINTR) {
            throw_link_error(tcp_.load(), std::string(tcp_.load() ? "socket" : "serial") +
                                              " poll failed: " + util::errno_string(errno));
        }
    }
}

void CelestronCgxProtocol::write_frame_locked(const std::vector<std::uint8_t>& bytes,
                                              std::chrono::steady_clock::time_point deadline) {
    if (tcp_.load()) {
        wait_for_locked(POLLOUT, deadline);
        // send_all() completes a short send; when it cannot, the adapter may hold half a frame, so the socket closes.
        if (!util::send_all(serial_fd_, reinterpret_cast<const char*>(bytes.data()), bytes.size(), MSG_NOSIGNAL)) {
            fail_socket_locked("send", errno);
        }
        return;
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        wait_for_locked(POLLOUT, deadline);
        const ssize_t count = ::write(serial_fd_, bytes.data() + written, bytes.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            const int code = errno;
            const auto error = util::errno_string(code);
            // The verdict is the link's (util::SerialLink, #2262), fed from this raw loop.
            if (link_.note_error(code)) lose_link_locked("write: " + error);
            disconnect_locked();
            throw_link_error(tcp_.load(), "serial write failed: " + error);
        }
    }
}

// The next complete frame in the receive buffer, junk before it skipped; nullopt until one is there.
std::optional<std::vector<std::uint8_t>> CelestronCgxProtocol::buffered_frame_locked() {
    while (true) {
        const auto start = std::find(receive_buffer_.begin(), receive_buffer_.end(), kAuxStart);
        receive_buffer_.erase(receive_buffer_.begin(), start);
        if (receive_buffer_.size() >= 2) {
            const std::size_t length = receive_buffer_[1];
            if (length < 3 || length + 3 > kMaximumFrameSize) {
                receive_buffer_.erase(receive_buffer_.begin());
                continue;
            }
            const std::size_t frame_size = length + 3;
            if (receive_buffer_.size() >= frame_size && tcp_.load() && !checksum_valid(receive_buffer_, frame_size)) {
                // The adapter forwards collision debris (rig 2026-11-20): drop this start byte and resync on the
                // next one. A read-only query whose reply was lost this way is re-sent by exchange().
                receive_buffer_.erase(receive_buffer_.begin());
                continue;
            }
            if (receive_buffer_.size() >= frame_size) {
                std::vector<std::uint8_t> frame(receive_buffer_.begin(),
                                                receive_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
                receive_buffer_.erase(receive_buffer_.begin(),
                                      receive_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
                return frame;
            }
        }
        return std::nullopt;
    }
}

std::vector<std::uint8_t> CelestronCgxProtocol::read_frame_locked(std::chrono::steady_clock::time_point deadline) {
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw_timeout(tcp_.load());
        }
        if (auto frame = buffered_frame_locked()) return std::move(*frame);

        wait_for_locked(POLLIN, deadline);
        std::array<std::uint8_t, 128> incoming{};
        const bool tcp = tcp_.load();
        ssize_t count = 0;
        // The owned fd (or socket) stays O_NONBLOCK; the lock serializes whole AUX exchanges.
        if (tcp) {
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): the socket is O_NONBLOCK
            count = ::recv(serial_fd_, incoming.data(), incoming.size(), 0);
        } else {
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): the fd is O_NONBLOCK
            count = ::read(serial_fd_, incoming.data(), incoming.size());
        }
        if (count > 0) {
            receive_buffer_.insert(receive_buffer_.end(), incoming.begin(),
                                   incoming.begin() + static_cast<std::ptrdiff_t>(count));
        } else if (count == 0) {
            lose_link_locked(tcp ? "the adapter closed the connection" : "end of file");
        } else if (tcp && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            fail_socket_locked("recv", errno);
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            const int code = errno;
            const auto error = util::errno_string(code);
            if (link_.note_error(code)) lose_link_locked("read: " + error);  // the link's verdict (#2262)
            disconnect_locked();
            throw_link_error(false, "serial read failed: " + error);
        }
    }
}

}  // namespace alpacacore::vendor::celestron

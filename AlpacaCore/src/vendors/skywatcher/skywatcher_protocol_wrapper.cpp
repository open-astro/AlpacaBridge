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
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <filesystem>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace alpacacore::vendor::skywatcher {

namespace {

constexpr char kFrameStart = ':';
constexpr char kFrameEnd = '\r';
constexpr char kReplyOk = '=';
constexpr char kReplyError = '!';
// UDP datagrams can be silently dropped (spec: one command per datagram, one
// response per datagram) — retransmit a bounded number of times on timeout.
constexpr int kUdpRetries = 3;

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

char nibble_hex(uint32_t v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

std::string format_mc_version(const std::string& data) {
    // ":e" replies with 6 hex chars. TODO: validate the byte order of the
    // version fields against Wave 100i hardware (INDI swaps the first and
    // third bytes of the straight-parsed value).
    if (data.size() < 6) {
        return "";
    }
    auto byte_at = [&data](std::size_t i) {
        int hi = hex_nibble(data[i]);
        int lo = hex_nibble(data[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        return hi * 16 + lo;
    };
    int b0 = byte_at(0);
    int b1 = byte_at(2);
    int b2 = byte_at(4);
    if (b0 < 0 || b1 < 0 || b2 < 0) {
        return "";
    }
    std::ostringstream oss;
    oss << b0 << "." << b1 << "." << b2;
    return oss.str();
}

// Expected "=" reply payload length for each command word, used by the UDP
// transport to reject a mis-paired stale reply (a duplicate ACK from a
// retransmitted command otherwise pairs itself with the NEXT command --
// seen on Wave 100i Wi-Fi as ':f' answered by a bare '='). -1 = unknown.
int expected_reply_data_len(char command) {
    switch (command) {
        case 'e':
        case 'a':
        case 'b':
        case 'j':
        case 'h':
        case 'i':
        case 'D':
        case 'q':
            return 6;
        case 'f':
            return 3;
        case 'g':
            return 2;
        case 'c':
            return -1;
        case 'E':
        case 'F':
        case 'G':
        case 'S':
        case 'I':
        case 'J':
        case 'K':
        case 'L':
        case 'O':
        case 'P':
        case 'V':
        case 'W':
            return 0;
        default:
            return -1;
    }
}

std::string mc_error_message(const std::string& code) {
    // Error codes from the MC command set; unknown codes are surfaced raw.
    if (code == "0") return "Unknown command";
    if (code == "1") return "Command length error";
    if (code == "2") return "Motor not stopped";
    if (code == "3") return "Invalid character";
    if (code == "4") return "Not initialized";
    if (code == "5") return "Driver sleeping";
    if (code == "7") return "PEC training running";
    if (code == "8") return "No valid PEC data";
    return "Motor controller error code " + code;
}

}  // namespace

std::string SkyWatcherProtocolWrapper::encode_u24(uint32_t value) {
    // 0x123456 -> "563412": low byte first, each byte high-nibble-first.
    std::string out(6, '0');
    out[0] = nibble_hex((value >> 4) & 0xF);
    out[1] = nibble_hex(value & 0xF);
    out[2] = nibble_hex((value >> 12) & 0xF);
    out[3] = nibble_hex((value >> 8) & 0xF);
    out[4] = nibble_hex((value >> 20) & 0xF);
    out[5] = nibble_hex((value >> 16) & 0xF);
    return out;
}

uint32_t SkyWatcherProtocolWrapper::decode_u24(const std::string& data) {
    if (data.size() < 6) {
        throw AlpacaException("Motor controller reply too short for 24-bit value: '" + data + "'");
    }
    uint32_t value = 0;
    // "563412" -> bytes 0x56, 0x34, 0x12 -> 0x123456
    static constexpr int kByteShift[3] = {0, 8, 16};
    for (int b = 0; b < 3; ++b) {
        auto idx = static_cast<std::size_t>(b) * 2;
        int hi = hex_nibble(data[idx]);
        int lo = hex_nibble(data[idx + 1]);
        if (hi < 0 || lo < 0) {
            throw AlpacaException("Motor controller reply is not hex: '" + data + "'");
        }
        value |= static_cast<uint32_t>(hi * 16 + lo) << kByteShift[b];
    }
    return value;
}

// ── Serial probe / enumeration ──────────────────────────────────────────────

namespace {

#ifndef _WIN32
// Open a serial port at 9600 8N1 and probe it with ":e1\r". Returns the motor
// board version string on success, empty on failure.
std::string probe_skywatcher_port(const std::string& port_path, int baud_rate) {
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return "";
    }

    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return "";
    }
    speed_t speed = baud_rate == 115200 ? B115200 : B9600;
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return "";
    }
    // CH340/CH341-style adapters assert DTR on open and reset an attached MCU
    // on close when HUPCL is set; clear it so the probe doesn't double-reset
    // the controller (~4 s penalty per probe).
    tty.c_cflag &= ~HUPCL;
    tcsetattr(fd, TCSANOW, &tty);

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return "";
    }
    tcflush(fd, TCIOFLUSH);

    const char probe[] = ":e1\r";
    if (!util::write_all(fd, probe, sizeof(probe) - 1)) {
        close(fd);
        return "";
    }

    std::string reply;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500)) {
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            if (ch == kFrameEnd) break;
            reply.push_back(ch);
            if (reply.size() > 16) break;
        } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    close(fd);

    if (reply.size() < 7 || reply[0] != kReplyOk) {
        return "";
    }
    std::string version = format_mc_version(reply.substr(1));
    return version.empty() ? "unknown" : version;
}

bool raw_port_looks_like_skywatcher_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    // Wave-series mounts expose an STM32 CDC-ACM virtual COM port (0483:5740,
    // /dev/ttyACM*); also accept the classic EQDIRECT cable chips.
    return alpacacore::util::usb_tty_descriptor_matches(
        *descriptor, {"STM32", "STMicroelectronics", "0483", "Prolific", "PL2303", "067b", "FTDI", "CP210", "CH340",
                      "CH341", "1a86", "Silicon_Labs", "USB_Serial", "USB-Serial"});
}
#endif  // _WIN32

}  // namespace

std::vector<SkyWatcherPortInfo> enumerate_skywatcher_ports() {
    std::vector<SkyWatcherPortInfo> results;

#ifndef _WIN32
    std::set<std::string> probed;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            bool is_candidate =
                (name.find("STM32") != std::string::npos) || (name.find("STMicroelectronics") != std::string::npos) ||
                (name.find("Prolific") != std::string::npos) || (name.find("PL2303") != std::string::npos) ||
                (name.find("067b") != std::string::npos) || (name.find("FTDI") != std::string::npos) ||
                (name.find("CP210") != std::string::npos) || (name.find("CH340") != std::string::npos) ||
                (name.find("1a86") != std::string::npos) || (name.find("Silicon_Labs") != std::string::npos) ||
                (name.find("USB_Serial") != std::string::npos) || (name.find("USB-Serial") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            {
                std::string msg = "Probing ";
                msg += resolved;
                msg += " (";
                msg += name;
                msg += ")...";
                ALPACA_LOG_INFO("SkyWatcher", msg);
            }
            std::string fw = probe_skywatcher_port(resolved, 9600);
            if (!fw.empty()) {
                std::string msg = "Found Sky-Watcher motor controller on ";
                msg += resolved;
                msg += " (MC firmware ";
                msg += fw;
                msg += ")";
                ALPACA_LOG_INFO("SkyWatcher", msg);
                results.push_back({resolved, name, fw});
            }
        }
    }

    // Raw /dev/ttyUSB* fallback: udev by-id naming collides for serial-number-
    // less adapters, silently dropping one of two identical dongles from by-id
    // (same rationale as the SynScan/Gemini scans).
    std::vector<std::string> raw_candidates;
    for (int i = 0; i < 10; ++i) {
        // Wave mounts enumerate as CDC-ACM (/dev/ttyACM*); EQDIRECT cables as
        // /dev/ttyUSB*.
        raw_candidates.push_back("/dev/ttyACM" + std::to_string(i));
        raw_candidates.push_back("/dev/ttyUSB" + std::to_string(i));
    }
    for (const std::string& port : raw_candidates) {
        if (!alpacacore::util::path_exists(port)) continue;
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_skywatcher_candidate(resolved)) continue;
        probed.insert(resolved);
        ALPACA_LOG_INFO("SkyWatcher", "Probing " + resolved + "...");
        std::string fw = probe_skywatcher_port(resolved, 9600);
        if (!fw.empty()) {
            std::string msg = "Found Sky-Watcher motor controller on ";
            msg += resolved;
            msg += " (MC firmware ";
            msg += fw;
            msg += ")";
            ALPACA_LOG_INFO("SkyWatcher", msg);
            results.push_back({resolved, "", fw});
        }
    }
#endif

    return results;
}

// ── Wi-Fi (UDP) discovery ───────────────────────────────────────────────────

namespace {

#ifndef _WIN32
// Probe one host with ":e1\r" over UDP. Returns firmware string or empty.
std::string probe_skywatcher_udp(const std::string& host, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return "";
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return "";
    }
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = static_cast<long>(timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char probe[] = ":e1\r";
    if (sendto(fd, probe, sizeof(probe) - 1, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }
    char buf[64] = {};
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    close(fd);
    if (n < 7 || buf[0] != kReplyOk) {
        return "";
    }
    std::string reply(buf, static_cast<std::size_t>(n));
    while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
        reply.pop_back();
    }
    std::string version = format_mc_version(reply.substr(1));
    return version.empty() ? "unknown" : version;
}

// Broadcast the version probe on every broadcast-capable IPv4 interface and
// collect responders until the timeout expires.
void broadcast_discover(std::vector<SkyWatcherHostInfo>& results, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = static_cast<long>(200) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char probe[] = ":e1\r";
    std::set<std::string> targets = {"255.255.255.255"};
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_BROADCAST) || !ifa->ifa_broadaddr) continue;
            char buf[INET_ADDRSTRLEN] = {};
            auto* baddr = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
            if (inet_ntop(AF_INET, &baddr->sin_addr, buf, sizeof(buf))) {
                targets.insert(buf);
            }
        }
        freeifaddrs(ifaddr);
    }

    for (const auto& target : targets) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, target.c_str(), &addr.sin_addr) <= 0) continue;
        sendto(fd, probe, sizeof(probe) - 1, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }

    std::set<std::string> seen;
    for (const auto& r : results) {
        seen.insert(r.host);
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        char buf[64] = {};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 7 || buf[0] != kReplyOk) {
            continue;
        }
        char host_buf[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &from.sin_addr, host_buf, sizeof(host_buf))) {
            continue;
        }
        std::string host(host_buf);
        if (seen.count(host) != 0) continue;
        seen.insert(host);
        std::string reply(buf, static_cast<std::size_t>(n));
        while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
            reply.pop_back();
        }
        std::string version = format_mc_version(reply.substr(1));
        results.push_back({host, port, version.empty() ? "unknown" : version});
        ALPACA_LOG_INFO("SkyWatcher", "Discovered motor controller at " + host + " (MC firmware " +
                                          (version.empty() ? "unknown" : version) + ")");
    }
    close(fd);
}
#endif  // _WIN32

}  // namespace

std::vector<SkyWatcherHostInfo> discover_skywatcher_hosts(int timeout_ms) {
    std::vector<SkyWatcherHostInfo> results;
#ifndef _WIN32
    constexpr int kPort = 11880;
    // AP-mode address per the MC command set spec.
    std::string fw = probe_skywatcher_udp("192.168.4.1", kPort, 500);
    if (!fw.empty()) {
        results.push_back({"192.168.4.1", kPort, fw});
        ALPACA_LOG_INFO("SkyWatcher", "Found motor controller at 192.168.4.1 (MC firmware " + fw + ")");
    }
    broadcast_discover(results, kPort, timeout_ms);
#else
    (void)timeout_ms;
#endif
    return results;
}

// ── Transport implementation ────────────────────────────────────────────────

class SkyWatcherProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    bool connect(const ConnectionInfo& info) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (connected_) {
            disconnect_locked();
        }
        if (info.type == ConnectionType::Serial && info.port_path.empty()) {
            ALPACA_LOG_ERROR("SkyWatcher", "Serial connection requested but port_path is empty");
            return false;
        }
        if (info.type == ConnectionType::Network && info.host.empty()) {
            ALPACA_LOG_ERROR("SkyWatcher", "Network connection requested but host is empty");
            return false;
        }
        info_ = info;
        response_timeout_ms_.store(info.response_timeout_ms > 0 ? info.response_timeout_ms : 1000,
                                   std::memory_order_relaxed);
        bool ok = info.type == ConnectionType::Serial ? connect_serial(info) : connect_udp(info);
        connected_ = ok;
        return ok;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        disconnect_locked();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return connected_;
    }

    std::string exchange(const std::string& frame, int timeout_ms, int expected_data_len = -1) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to Sky-Watcher motor controller", AlpacaError::NotConnected);
        }
        if (info_.type == ConnectionType::Serial) {
            return exchange_serial(frame, timeout_ms);
        }
        return exchange_udp(frame, timeout_ms, expected_data_len);
    }

    // Read lock-free from send paths; published in connect() under io_mutex_.
    int default_timeout() const { return response_timeout_ms_.load(std::memory_order_relaxed); }

private:
    void disconnect_locked() {
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
#endif
        connected_ = false;
    }

    bool connect_serial(const ConnectionInfo& info) {
#ifndef _WIN32
        serial_fd_ = open(info.port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            ALPACA_LOG_ERROR("SkyWatcher", "Failed to open " + info.port_path + ": " + std::strerror(errno));
            return false;
        }
        struct termios tty {};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        speed_t speed = info.baud_rate == 115200 ? B115200 : B9600;
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= CREAD | CLOCAL;
        tty.c_cflag &= ~HUPCL;  // keep DTR asserted on close (CH340 MCU-reset quirk)
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tty.c_oflag &= ~OPOST;
        tty.c_oflag &= ~ONLCR;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;
        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        if (!util::clear_nonblocking(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        tcflush(serial_fd_, TCIOFLUSH);
        return true;
#else
        (void)info;
        return false;
#endif
    }

    bool connect_udp(const ConnectionInfo& info) {
#ifndef _WIN32
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(info.udp_port));
        if (inet_pton(AF_INET, info.host.c_str(), &addr.sin_addr) <= 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(info.host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        // connect() the datagram socket so recv() only accepts the mount's
        // replies and send() needs no per-call address.
        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        // Verify the controller answers before declaring the link up: UDP
        // "connect" succeeds even with nothing listening.
        try {
            std::string reply = exchange_udp(":e1\r", default_timeout(), 6);
            if (reply.empty() || reply[0] != kReplyOk) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            fw_reply_ = reply;  // known ":e1" answer, used by resync_udp()
        } catch (const std::exception&) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        return true;
#else
        (void)info;
        return false;
#endif
    }

    std::string exchange_serial(const std::string& frame, int timeout_ms) {
#ifndef _WIN32
        // Leftover bytes from a timed-out earlier exchange would be parsed as
        // this command's reply — drain them first.
        tcflush(serial_fd_, TCIFLUSH);
        if (!util::write_all(serial_fd_, frame.data(), frame.size())) {
            throw AlpacaException("Serial write failed: " + std::string(std::strerror(errno)));
        }
        std::string reply;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            char ch = 0;
            // Serialized transport: one in-flight command per link, bounded by VTIME.
            ssize_t r = read(serial_fd_, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
            if (r == 1) {
                if (ch == kFrameEnd) {
                    return reply;
                }
                reply.push_back(ch);
                if (reply.size() > 32) {
                    throw AlpacaException("Motor controller reply overflow");
                }
            } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
                throw AlpacaException("Serial read failed: " + std::string(std::strerror(errno)));
            }
        }
        throw AlpacaException("Timeout waiting for motor controller reply to '" + frame + "'");
#else
        (void)frame;
        (void)timeout_ms;
        throw AlpacaException("Serial not supported on this platform");
#endif
    }

    std::string exchange_udp(const std::string& frame, int timeout_ms, int expected_data_len) {
#ifndef _WIN32
        for (int attempt = 0; attempt < kUdpRetries; ++attempt) {
            // Drain any stale datagram (a late reply to a timed-out command)
            // before sending, so replies can't get off-by-one.
            drain_udp();
            bool was_dirty = link_dirty_;
            if (link_dirty_) {
                // A previous exchange timed out, so its reply may still be in
                // flight and would otherwise be consumed as THIS command's
                // reply — the mis-pairing that garbled positions and made the
                // mount swing erratically over Wi-Fi. Soak up late arrivals
                // for a settle window, then RESYNC: probe with ":e1" (whose
                // reply value is fixed and known from connect) and require the
                // known answer before trusting the stream — a same-length
                // stale reply to a different command cannot fake that.
                settle_drain(300);
                if (!resync_udp()) {
                    link_dirty_ = true;
                    continue;  // burn this attempt; settle and probe again
                }
                link_dirty_ = false;
            }
            if (send(socket_fd_, frame.data(), frame.size(), MSG_NOSIGNAL) < 0) {
                link_dirty_ = true;
                // A Wi-Fi drop/rejoin can change the interface's IP address,
                // permanently invalidating a connect()ed datagram socket
                // (every send then fails ENETUNREACH even after the link is
                // back). Rebuild the socket and retry instead of wedging until
                // the client power-cycles the connection.
                if ((errno == ENETUNREACH || errno == EADDRNOTAVAIL || errno == EHOSTUNREACH) && rebuild_udp_socket() &&
                    send(socket_fd_, frame.data(), frame.size(), MSG_NOSIGNAL) >= 0) {
                    ALPACA_LOG_WARN("SkyWatcher",
                                    "UDP socket went stale (interface address changed); rebuilt and resent");
                } else {
                    throw AlpacaException("UDP send failed: " + std::string(std::strerror(errno)));
                }
            }
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (true) {
                auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0) {
                    break;  // timeout — retransmit
                }
                timeval tv{};
                tv.tv_sec = static_cast<time_t>(remaining.count() / 1000);
                tv.tv_usec = static_cast<suseconds_t>((remaining.count() % 1000) * 1000);
                setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                char buf[64] = {};
                // Serialized transport: bounded by SO_RCVTIMEO.
                ssize_t n =
                    recv(socket_fd_, buf, sizeof(buf) - 1, 0);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
                if (n > 0) {
                    std::string reply(buf, static_cast<std::size_t>(n));
                    while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
                        reply.pop_back();
                    }
                    if (!reply.empty() && reply[0] == kReplyError) {
                        if (was_dirty) {
                            // Just after a timeout, a stale error reply from
                            // the timed-out command may still arrive: discard
                            // it and retransmit rather than surfacing a
                            // misleading rejection for THIS command. (The next
                            // attempt recomputes was_dirty from link_dirty_.)
                            link_dirty_ = true;
                            break;
                        }
                        return reply;
                    }
                    if (!reply.empty() && reply[0] == kReplyOk &&
                        (expected_data_len < 0 || static_cast<int>(reply.size()) - 1 == expected_data_len)) {
                        return reply;
                    }
                    // Wrong shape for THIS command (a mis-paired duplicate ACK
                    // from a retransmission) or non-protocol garbage: discard
                    // and keep waiting for the matching reply.
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    break;  // timeout — retransmit
                }
                if (n < 0) {
                    link_dirty_ = true;
                    throw AlpacaException("UDP receive failed: " + std::string(std::strerror(errno)));
                }
            }
            link_dirty_ = true;
        }
        throw AlpacaException("Timeout waiting for motor controller reply to '" + frame + "' after " +
                              std::to_string(kUdpRetries) + " attempts");
#else
        (void)frame;
        (void)timeout_ms;
        throw AlpacaException("Network not supported on this platform");
#endif
    }

#ifndef _WIN32
    void drain_udp() {
        char buf[64];
        while (true) {
            // MSG_DONTWAIT: non-blocking drain, never actually blocks.
            ssize_t n =
                recv(socket_fd_, buf, sizeof(buf), MSG_DONTWAIT);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
            if (n <= 0) {
                break;
            }
        }
    }

    // Recreate the connect()ed datagram socket against the stored peer after
    // the kernel invalidated the old one (interface address change). Returns
    // false if the peer cannot be resolved/connected right now.
    bool rebuild_udp_socket() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(info_.udp_port));
        if (inet_pton(AF_INET, info_.host.c_str(), &addr.sin_addr) <= 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(info_.host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        return true;
    }

    // Blocking drain: absorb late-arriving datagrams for up to @p window_ms.
    // Verify reply-stream identity after a timeout: ":e1" always answers with
    // the motor-board version captured at connect. Returns true when the known
    // reply is received (stream aligned); false when the probe times out or
    // answers wrongly (caller settles and retries).
    bool resync_udp() {
        if (fw_reply_.empty()) {
            return true;  // no baseline captured; fall back to drains only
        }
        const std::string probe = ":e1\r";
        if (send(socket_fd_, probe.data(), probe.size(), MSG_NOSIGNAL) < 0) {
            return false;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < deadline) {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<long>(100) * 1000;
            setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[64] = {};
            ssize_t n =
                recv(socket_fd_, buf, sizeof(buf) - 1, 0);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
            if (n <= 0) {
                continue;
            }
            std::string reply(buf, static_cast<std::size_t>(n));
            while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
                reply.pop_back();
            }
            if (reply == fw_reply_) {
                return true;  // stream aligned on the known probe answer
            }
            // Stale reply from an earlier command — keep draining until the
            // probe's answer arrives or the window closes.
        }
        return false;
    }

    void settle_drain(int window_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
        char buf[64];
        while (std::chrono::steady_clock::now() < deadline) {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<long>(50) * 1000;
            setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            recv(socket_fd_, buf, sizeof(buf), 0);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        }
    }
#endif

    mutable std::mutex io_mutex_;
    bool connected_ = false;
    ConnectionInfo info_{};
#ifndef _WIN32
    int serial_fd_ = -1;
    int socket_fd_ = -1;
    // Set after a UDP timeout/error: the next exchange runs a settle drain
    // before sending so a late reply cannot be mis-paired. Guarded by io_mutex_.
    bool link_dirty_ = false;
    std::string fw_reply_;
#endif
    // Outside the platform guard: connect()/default_timeout() touch it on
    // every platform (the Windows stubs still compile against it).
    std::atomic<int> response_timeout_ms_{1000};
};

// ── Public wrapper API ──────────────────────────────────────────────────────

SkyWatcherProtocolWrapper::SkyWatcherProtocolWrapper() : pimpl_(std::make_unique<Impl>()) {}
SkyWatcherProtocolWrapper::~SkyWatcherProtocolWrapper() = default;

SkyWatcherProtocolWrapper& SkyWatcherProtocolWrapper::instance() {
    static SkyWatcherProtocolWrapper wrapper;
    return wrapper;
}

bool SkyWatcherProtocolWrapper::connect(const ConnectionInfo& info) { return pimpl_->connect(info); }

void SkyWatcherProtocolWrapper::disconnect() { pimpl_->disconnect(); }

bool SkyWatcherProtocolWrapper::is_connected() const { return pimpl_->is_connected(); }

std::string SkyWatcherProtocolWrapper::send_command(char command, int axis, const std::string& data,
                                                    int timeout_ms_override) {
    if (axis != kAxisRa && axis != kAxisDec) {
        throw AlpacaException("Invalid motor controller axis " + std::to_string(axis));
    }
    std::string frame;
    frame.reserve(4 + data.size());
    frame.push_back(kFrameStart);
    frame.push_back(command);
    frame.push_back(static_cast<char>('0' + axis));
    frame.append(data);
    frame.push_back(kFrameEnd);

    int timeout = timeout_ms_override > 0 ? timeout_ms_override : pimpl_->default_timeout();
    std::string reply = pimpl_->exchange(frame, timeout, expected_reply_data_len(command));
    if (!reply.empty() && reply[0] == kReplyOk) {
        return reply.substr(1);
    }
    if (!reply.empty() && reply[0] == kReplyError) {
        throw AlpacaException("Motor controller rejected '" + std::string(1, command) + std::to_string(axis) +
                              "': " + mc_error_message(reply.substr(1)));
    }
    throw AlpacaException("Malformed motor controller reply to '" + std::string(1, command) + std::to_string(axis) +
                          "': '" + reply + "'");
}

std::string SkyWatcherProtocolWrapper::send_raw_command(const std::string& frame, int timeout_ms_override) {
    int timeout = timeout_ms_override > 0 ? timeout_ms_override : pimpl_->default_timeout();
    return pimpl_->exchange(frame, timeout);
}

std::string SkyWatcherProtocolWrapper::get_motor_board_version() {
    std::string data = send_command('e', kAxisRa);
    std::string version = format_mc_version(data);
    if (version.empty()) {
        throw AlpacaException("Unparseable motor board version reply: '" + data + "'");
    }
    return version;
}

AxisParameters SkyWatcherProtocolWrapper::get_axis_parameters(int axis) {
    AxisParameters params;
    params.counts_per_revolution = decode_u24(send_command('a', axis));
    params.timer_frequency = decode_u24(send_command('b', axis));
    params.high_speed_ratio = decode_u24(send_command('g', axis) + "0000") & 0xFF;
    if (params.counts_per_revolution == 0 || params.timer_frequency == 0) {
        throw AlpacaException("Motor controller reported zero CPR or timer frequency on axis " + std::to_string(axis));
    }
    if (params.high_speed_ratio == 0) {
        params.high_speed_ratio = 1;
    }
    return params;
}

uint32_t SkyWatcherProtocolWrapper::inquire_position(int axis) { return decode_u24(send_command('j', axis)); }

AxisStatus SkyWatcherProtocolWrapper::inquire_status(int axis) {
    std::string data = send_command('f', axis);
    if (data.size() < 3) {
        throw AlpacaException("Short status reply: '" + data + "'");
    }
    int n0 = hex_nibble(data[0]);
    int n1 = hex_nibble(data[1]);
    int n2 = hex_nibble(data[2]);
    if (n0 < 0 || n1 < 0 || n2 < 0) {
        throw AlpacaException("Non-hex status reply: '" + data + "'");
    }
    AxisStatus status;
    status.speed_mode = (n0 & 0x1) != 0;
    status.ccw = (n0 & 0x2) != 0;
    status.fast = (n0 & 0x4) != 0;
    status.running = (n1 & 0x1) != 0;
    status.blocked = (n1 & 0x2) != 0;
    status.init_done = (n2 & 0x1) != 0;
    status.level_switch_on = (n2 & 0x2) != 0;
    return status;
}

void SkyWatcherProtocolWrapper::set_position(int axis, uint32_t counts) { send_command('E', axis, encode_u24(counts)); }

void SkyWatcherProtocolWrapper::initialization_done(int axis) { send_command('F', axis); }

void SkyWatcherProtocolWrapper::set_motion_mode(int axis, char mode, char direction) {
    send_command('G', axis, std::string(1, mode) + std::string(1, direction));
}

void SkyWatcherProtocolWrapper::set_goto_target(int axis, uint32_t counts) {
    send_command('S', axis, encode_u24(counts));
}

void SkyWatcherProtocolWrapper::set_step_period(int axis, uint32_t t1_preset) {
    send_command('I', axis, encode_u24(t1_preset));
}

void SkyWatcherProtocolWrapper::start_motion(int axis) { send_command('J', axis); }

void SkyWatcherProtocolWrapper::stop_motion(int axis) { send_command('K', axis); }

void SkyWatcherProtocolWrapper::instant_stop(int axis) { send_command('L', axis); }

void SkyWatcherProtocolWrapper::set_autoguide_speed(int axis, int speed_code) {
    if (speed_code < 0 || speed_code > 4) {
        throw AlpacaException("Autoguide speed code must be 0-4", AlpacaError::InvalidValue);
    }
    send_command('P', axis, std::string(1, static_cast<char>('0' + speed_code)));
}

uint32_t SkyWatcherProtocolWrapper::get_feature(int axis, uint32_t inquiry) {
    return decode_u24(send_command('q', axis, encode_u24(inquiry)));
}

void SkyWatcherProtocolWrapper::set_feature(int axis, uint32_t command) {
    send_command('W', axis, encode_u24(command));
}

}  // namespace alpacacore::vendor::skywatcher

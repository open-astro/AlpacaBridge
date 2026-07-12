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
#include <alpacacore/util/units.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

#ifndef _WIN32
#include <filesystem>
#endif

namespace alpacacore::vendor::ioptron {

namespace {

// Probe a serial port for an iOptron mount by sending :MountInfo# and checking
// for a valid 4-digit model code response.
// iOptron returns exactly 4 ASCII digits with no '#' terminator.
// Returns the model code (e.g. "0025") on success, empty string on failure.
std::string probe_ioptron_port(const std::string& port_path) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return "";
    }

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return "";
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
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
    tty.c_cc[VTIME] = 20;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return "";
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return "";
    }
    tcflush(fd, TCIOFLUSH);

    const char cmd[] = ":MountInfo#";
    if (!util::write_all(fd, cmd, sizeof(cmd) - 1)) {
        close(fd);
        return "";
    }

    // iOptron :MountInfo# returns exactly 4 ASCII digit bytes, no terminator.
    char resp[4] = {};
    int total = 0;
    auto start = std::chrono::steady_clock::now();
    while (total < 4) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > 3) {
            break;
        }
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            resp[total++] = ch;
        } else if (r == 0) {
            continue;
        } else {
            break;
        }
    }

    close(fd);

    if (total == 4) {
        bool all_digit = true;
        for (int i = 0; i < 4; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(resp[i]))) {
                all_digit = false;
                break;
            }
        }
        if (all_digit) {
            return std::string(resp, 4);
        }
    }

    return "";
#else
    (void)port_path;
    return "";
#endif
}

// Probe a TCP host:port for an iOptron mount by sending :MountInfo# over a
// non-blocking socket with a short timeout. Returns the 4-digit model code on
// success, empty string on failure.
std::string probe_ioptron_network(const std::string& host, int port, int timeout_ms = 1500) {
#ifndef _WIN32
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return "";
    }

    if (!util::set_nonblocking(fd)) {
        close(fd);
        return "";
    }

    sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return "";
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return "";
    }

    if (rc < 0) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int poll_rc = poll(&pfd, 1, timeout_ms);
        if (poll_rc <= 0) {
            close(fd);
            return "";
        }
        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len);
        if (sock_err != 0) {
            close(fd);
            return "";
        }
    }

    // Restore blocking mode for the synchronous request/response below. If this
    // fails the socket stays non-blocking and write_all()/read would spuriously
    // report the mount absent, so treat it as a probe failure.
    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return "";
    }

    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const char cmd[] = ":MountInfo#";
    // This is a TCP socket, not a serial fd: use send_all with MSG_NOSIGNAL so a
    // probed host closing the connection mid-send can't SIGPIPE-kill the server.
    if (!util::send_all(fd, cmd, sizeof(cmd) - 1, MSG_NOSIGNAL)) {
        close(fd);
        return "";
    }

    char resp[4] = {};
    int total = 0;
    auto start = std::chrono::steady_clock::now();
    while (total < 4) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            break;
        }
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            resp[total++] = ch;
        } else if (r == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
    }

    close(fd);

    if (total == 4) {
        bool all_digit = true;
        for (int i = 0; i < 4; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(resp[i]))) {
                all_digit = false;
                break;
            }
        }
        if (all_digit) {
            return std::string(resp, 4);
        }
    }

    return "";
#else
    (void)host;
    (void)port;
    (void)timeout_ms;
    return "";
#endif
}

struct LocalSubnet {
    uint32_t base;
    uint32_t mask;
    uint32_t self;
    std::string iface_name;
};

std::vector<LocalSubnet> get_local_subnets() {
    std::vector<LocalSubnet> subnets;
#ifndef _WIN32
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return subnets;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }
        if (!(ifa->ifa_flags & IFF_UP)) {
            continue;
        }
        if (!ifa->ifa_netmask) {
            continue;
        }

        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        auto* nm = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
        uint32_t ip = ntohl(sa->sin_addr.s_addr);
        uint32_t mask = ntohl(nm->sin_addr.s_addr);
        uint32_t base = ip & mask;
        std::string name = ifa->ifa_name ? ifa->ifa_name : "";

        bool duplicate = false;
        for (const auto& s : subnets) {
            if (s.base == base && s.mask == mask) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            subnets.push_back({base, mask, ip, name});
        }
    }

    freeifaddrs(ifaddr);
#endif
    return subnets;
}

struct KnownIOptronAddress {
    const char* host;
    int port;
};

// Well-known default IPs used by iOptron WiFi modules.
// The mount acts as an AP and always assigns itself a fixed address.
static constexpr KnownIOptronAddress KNOWN_IOPTRON_ADDRESSES[] = {
    {"10.10.100.254", 8899},
    {"10.10.100.254", 4030},
    {"10.10.100.1",   8899},
    {"10.10.100.1",   4030},
    {"192.168.100.1",  8899},
    {"192.168.100.1",  4030},
};

// Read default gateway for a given interface from /proc/net/route.
// Returns the gateway IP in host byte order, or 0 if not found.
uint32_t get_interface_gateway(const std::string& iface_name) {
#ifndef _WIN32
    FILE* fp = fopen("/proc/net/route", "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    // Skip header
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        char iface[64];
        unsigned int dest, gw;
        if (sscanf(line, "%63s %x %x", iface, &dest, &gw) == 3) {
            if (iface_name == iface && dest == 0 && gw != 0) {
                fclose(fp);
                return ntohl(gw);
            }
        }
    }

    fclose(fp);
#else
    (void)iface_name;
#endif
    return 0;
}

} // anonymous namespace

std::string model_code_to_name(const std::string& code) {
    // Current v3 protocol assignments (from INDI ioptronv3 driver).
    // iOptron has reassigned some codes from earlier products:
    //   0010: Cube II EQ → SkyHunter EQ
    //   0011: SmartEQ Pro+ → SkyHunter AA
    //   0025: CEM25 → HEM27
    //   0030: iEQ30 Pro → HEM27-EC
    if (code == "0010") return "SkyHunter EQ";
    if (code == "0011") return "SkyHunter AA";
    if (code == "0012") return "HAE16 EQ";
    if (code == "0013") return "HAE16 AA";
    if (code == "0014") return "HAE18 EQ";
    if (code == "0015") return "HEM15";
    if (code == "0022") return "HAE18 AA";
    if (code == "0025") return "HEM27";
    if (code == "0026") return "CEM26";
    if (code == "0027") return "CEM26-EC";
    if (code == "0028") return "GEM28";
    if (code == "0029") return "GEM28-EC";
    if (code == "0030") return "HEM27-EC";
    if (code == "0031") return "HAE29 EQ";
    if (code == "0032") return "HAE29-EC AA";
    if (code == "0033") return "HAE29 AA";
    if (code == "0034") return "HAE29-EC AA";
    if (code == "0035") return "HAZ31";
    if (code == "0036") return "HAE29C EQ";
    if (code == "0037") return "HAE29C-EC EQ";
    if (code == "0038") return "HAE29C AA";
    if (code == "0039") return "HAE29C-EC EQ";
    if (code == "0040") return "CEM40";
    if (code == "0041") return "CEM40-EC";
    if (code == "0043") return "GEM45";
    if (code == "0045") return "HEM44-EC";
    if (code == "0046") return "HEM44A";
    if (code == "0047") return "HEM44A-EC";
    if (code == "0048") return "HAE43 EQ";
    if (code == "0049") return "HAE43-EC EQ";
    if (code == "0050") return "HAE43 AA";
    if (code == "0051") return "HAE43-EC AA";
    if (code == "0052") return "HAZ46";
    if (code == "0053") return "HAE43C EQ";
    if (code == "0054") return "HAE43C-EC EQ";
    if (code == "0055") return "HAE43C AA";
    if (code == "0056") return "HAE43C-EC AA";
    if (code == "0060") return "CEM60";
    if (code == "0061") return "CEM60-EC";
    if (code == "0062") return "HAE69 EQ";
    if (code == "0063") return "HAZ69-EC EQ";
    if (code == "0064") return "HAE69 AA";
    if (code == "0065") return "HAE69-EC AA";
    if (code == "0066") return "HAE69C EQ";
    if (code == "0067") return "HAE69C-EC EQ";
    if (code == "0068") return "HAE69C AA";
    if (code == "0069") return "HAE69C-EC AA";
    if (code == "0070") return "CEM70";
    if (code == "0071") return "CEM70-EC";
    if (code == "0072") return "CEM70-EC2";
    if (code == "0073") return "HAZ71";
    if (code == "0120") return "CEM120";
    if (code == "0121") return "CEM120-EC";
    if (code == "0122") return "CEM120-EC2";
    if (code == "5010") return "Cube II AA";
    if (code == "5035") return "AZ Mount Pro";
    if (code == "5045") return "iEQ45 Pro AA";
    return "";
}

std::vector<iOptronPortInfo> enumerate_ioptron_ports() {
    std::vector<iOptronPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (std::filesystem::exists(port)) {
                ALPACA_LOG_INFO("iOptron", "Probing " + port + "...");
                std::string code = probe_ioptron_port(port);
                if (!code.empty()) {
                    std::string name = model_code_to_name(code);
                    ALPACA_LOG_INFO("iOptron", "Found iOptron mount on " + port +
                                    " (model " + code + (name.empty() ? "" : " / " + name) + ")");
                    results.push_back({port, "", code});
                }
            }
        }
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(serial_by_id)) {
        if (!entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();

        bool is_candidate = (name.find("Prolific") != std::string::npos) ||
                            (name.find("PL2303") != std::string::npos) ||
                            (name.find("067b") != std::string::npos) ||
                            (name.find("FTDI") != std::string::npos) ||
                            (name.find("CP210") != std::string::npos) ||
                            (name.find("Silicon_Labs") != std::string::npos) ||
                            (name.find("USB_Serial") != std::string::npos) ||
                            (name.find("USB-Serial") != std::string::npos);
        if (!is_candidate) continue;

        std::string resolved = std::filesystem::canonical(entry.path()).string();
        ALPACA_LOG_INFO("iOptron", "Probing " + resolved + " (" + name + ")...");

        std::string code = probe_ioptron_port(resolved);
        if (!code.empty()) {
            std::string model = model_code_to_name(code);
            ALPACA_LOG_INFO("iOptron", "Found iOptron mount on " + resolved +
                            " (model " + code + (model.empty() ? "" : " / " + model) + ")");
            results.push_back({resolved, name, code});
        }
    }
#endif

    return results;
}

// Try to connect + probe a single host:port in one step for the fast-path
// known-address check. Returns model code or empty string.
iOptronNetworkHostInfo try_known_address(const char* host, int port, int timeout_ms) {
    std::string code = probe_ioptron_network(host, port, timeout_ms);
    if (!code.empty()) {
        return {host, port, code};
    }
    return {"", 0, ""};
}

std::vector<iOptronNetworkHostInfo> enumerate_ioptron_network_hosts() {
    std::vector<iOptronNetworkHostInfo> results;

#ifndef _WIN32
    static constexpr int PROBE_PORTS[] = {8899, 4030};
    static constexpr int NUM_PROBE_PORTS = 2;
    static constexpr int CONNECT_BATCH_SIZE = 256;
    static constexpr int CONNECT_TIMEOUT_MS = 1200;
    static constexpr int PROBE_TIMEOUT_MS = 2500;
    static constexpr int KNOWN_ADDR_TIMEOUT_MS = 2500;
    static constexpr uint32_t MAX_SCANNABLE_HOST_BITS = 255;

    // --- Phase 1: Parallel probe of well-known iOptron default addresses ---
    // All known addresses are connected concurrently so multiple iOptron WiFi
    // modules on the host network are all discovered (mount_index > 0 needs
    // every responding mount, not just the first).
    ALPACA_LOG_INFO("iOptron", "Network discovery: trying known iOptron default addresses...");
    {
        struct PendingConnect { int fd; std::string host; int port; };
        std::vector<PendingConnect> pending;
        pending.reserve(sizeof(KNOWN_IOPTRON_ADDRESSES) / sizeof(KNOWN_IOPTRON_ADDRESSES[0]));

        for (const auto& addr : KNOWN_IOPTRON_ADDRESSES) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                continue;
            }

            // Non-blocking connect for the poll-with-timeout scan; if we can't
            // set it, connect() would block for the full OS TCP timeout (~127s)
            // and stall the whole subnet scan, so skip this candidate instead.
            if (!util::set_nonblocking(fd)) {
                close(fd);
                continue;
            }

            sockaddr_in sin{};
            std::memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(static_cast<uint16_t>(addr.port));
            inet_pton(AF_INET, addr.host, &sin.sin_addr);

            int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin));
            if (rc == 0 || errno == EINPROGRESS) {
                pending.push_back({fd, addr.host, addr.port});
            } else {
                close(fd);
            }
        }

        if (!pending.empty()) {
            std::vector<struct pollfd> pfds(pending.size());
            for (std::size_t i = 0; i < pending.size(); ++i) {
                pfds[i].fd = pending[i].fd;
                pfds[i].events = POLLOUT;
                pfds[i].revents = 0;
            }
            poll(pfds.data(), static_cast<nfds_t>(pfds.size()), KNOWN_ADDR_TIMEOUT_MS);

            for (std::size_t i = 0; i < pending.size(); ++i) {
                bool connected = false;
                if (pfds[i].revents & POLLOUT) {
                    int sock_err = 0;
                    socklen_t len = sizeof(sock_err);
                    getsockopt(pending[i].fd, SOL_SOCKET, SO_ERROR, &sock_err, &len);
                    connected = (sock_err == 0);
                }
                close(pending[i].fd);
                if (!connected) {
                    continue;
                }

                std::string code = probe_ioptron_network(pending[i].host, pending[i].port, PROBE_TIMEOUT_MS);
                if (!code.empty()) {
                    std::string model = model_code_to_name(code);
                    ALPACA_LOG_INFO("iOptron", "Network discovery: found mount at " +
                                    pending[i].host + ":" + std::to_string(pending[i].port) +
                                    " (model " + code +
                                    (model.empty() ? "" : " / " + model) + ")");
                    results.push_back({pending[i].host, pending[i].port, code});
                }
            }
        }
    }

    ALPACA_LOG_INFO("iOptron", "Network discovery: scanning local subnets for additional mounts...");

    // --- Phase 2: Enumerate local subnets and scan ---
    auto subnets = get_local_subnets();
    if (subnets.empty()) {
        ALPACA_LOG_INFO("iOptron", "Network discovery: no local network interfaces found");
        return results;
    }

    for (const auto& subnet : subnets) {
        char self_str[INET_ADDRSTRLEN];
        struct in_addr self_addr{};
        self_addr.s_addr = htonl(subnet.self);
        inet_ntop(AF_INET, &self_addr, self_str, sizeof(self_str));

        char mask_str[INET_ADDRSTRLEN];
        struct in_addr mask_addr{};
        mask_addr.s_addr = htonl(subnet.mask);
        inet_ntop(AF_INET, &mask_addr, mask_str, sizeof(mask_str));

        uint32_t host_bits = ~subnet.mask;

        ALPACA_LOG_INFO("iOptron", "Network discovery: interface " + subnet.iface_name +
                        " addr=" + std::string(self_str) + " mask=" + std::string(mask_str) +
                        " (" + std::to_string(host_bits > 1 ? host_bits - 1 : 0) + " scannable hosts)");

        // Try the gateway address first — on mount WiFi networks, the mount IS the gateway
        uint32_t gw = get_interface_gateway(subnet.iface_name);
        if (gw != 0 && gw != subnet.self) {
            struct in_addr gw_addr{};
            gw_addr.s_addr = htonl(gw);
            char gw_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &gw_addr, gw_str, sizeof(gw_str));

            ALPACA_LOG_INFO("iOptron", "Network discovery: trying gateway " +
                            std::string(gw_str) + " on " + subnet.iface_name);
            for (int port : PROBE_PORTS) {
                auto found = try_known_address(gw_str, port, KNOWN_ADDR_TIMEOUT_MS);
                if (!found.model_code.empty()) {
                    std::string model = model_code_to_name(found.model_code);
                    ALPACA_LOG_INFO("iOptron", "Network discovery: found mount at gateway " +
                                    found.host + ":" + std::to_string(found.tcp_port) +
                                    " (model " + found.model_code +
                                    (model.empty() ? "" : " / " + model) + ")");
                    results.push_back(found);
                }
            }
        }

        if (host_bits < 2) {
            continue;
        }

        if (host_bits > MAX_SCANNABLE_HOST_BITS) {
            ALPACA_LOG_INFO("iOptron", "Network discovery: skipping large subnet on " +
                            subnet.iface_name + " (" + std::to_string(host_bits) +
                            " host bits, max scannable is " +
                            std::to_string(MAX_SCANNABLE_HOST_BITS) + ")");
            continue;
        }

        // Build list of (ip, port) candidates
        struct Candidate { uint32_t ip; int port; };
        std::vector<Candidate> candidates;
        candidates.reserve(host_bits * NUM_PROBE_PORTS);
        for (uint32_t i = 1; i < host_bits; ++i) {
            uint32_t ip = subnet.base | i;
            if (ip == subnet.self) {
                continue;
            }
            for (int port : PROBE_PORTS) {
                candidates.push_back({ip, port});
            }
        }

        ALPACA_LOG_INFO("iOptron", "Network discovery: scanning " +
                        std::to_string(candidates.size()) + " candidates on " + subnet.iface_name);

        struct PendingConnect {
            int fd;
            std::string host;
            int port;
        };

        for (std::size_t batch_start = 0; batch_start < candidates.size();
             batch_start += CONNECT_BATCH_SIZE) {
            std::size_t batch_end = std::min(batch_start + static_cast<std::size_t>(CONNECT_BATCH_SIZE),
                                             candidates.size());

            std::vector<PendingConnect> pending;
            pending.reserve(batch_end - batch_start);

            for (std::size_t ci = batch_start; ci < batch_end; ++ci) {
                const auto& c = candidates[ci];
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                    continue;
                }

                // Non-blocking connect for the poll-with-timeout scan; if we
                // can't set it, connect() would block for the full OS TCP
                // timeout and stall the scan, so skip this candidate.
                if (!util::set_nonblocking(fd)) {
                    close(fd);
                    continue;
                }

                sockaddr_in addr{};
                std::memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<uint16_t>(c.port));
                addr.sin_addr.s_addr = htonl(c.ip);

                int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                if (rc == 0 || errno == EINPROGRESS) {
                    struct in_addr a{};
                    a.s_addr = htonl(c.ip);
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &a, ip_str, sizeof(ip_str));
                    pending.push_back({fd, std::string(ip_str), c.port});
                } else {
                    close(fd);
                }
            }

            if (pending.empty()) {
                continue;
            }

            std::vector<struct pollfd> pfds(pending.size());
            for (std::size_t i = 0; i < pending.size(); ++i) {
                pfds[i].fd = pending[i].fd;
                pfds[i].events = POLLOUT;
                pfds[i].revents = 0;
            }

            poll(pfds.data(), static_cast<nfds_t>(pfds.size()), CONNECT_TIMEOUT_MS);

            for (std::size_t i = 0; i < pending.size(); ++i) {
                bool connected = false;
                if (pfds[i].revents & POLLOUT) {
                    int sock_err = 0;
                    socklen_t len = sizeof(sock_err);
                    getsockopt(pending[i].fd, SOL_SOCKET, SO_ERROR, &sock_err, &len);
                    connected = (sock_err == 0);
                }
                close(pending[i].fd);

                if (!connected) {
                    continue;
                }

                ALPACA_LOG_INFO("iOptron", "Network discovery: TCP open at " +
                                pending[i].host + ":" + std::to_string(pending[i].port) +
                                ", sending :MountInfo#...");
                std::string code = probe_ioptron_network(pending[i].host, pending[i].port, PROBE_TIMEOUT_MS);
                if (!code.empty()) {
                    std::string model = model_code_to_name(code);
                    ALPACA_LOG_INFO("iOptron", "Network discovery: found iOptron mount at " +
                                    pending[i].host + ":" + std::to_string(pending[i].port) +
                                    " (model " + code +
                                    (model.empty() ? "" : " / " + model) + ")");
                    results.push_back({pending[i].host, pending[i].port, code});
                }
            }
        }

    }

    // Deduplicate by host:port — Phase 1 known addresses can overlap with the
    // subnet scan when a known iOptron default IP also sits in a scanned /24.
    if (results.size() > 1) {
        std::unordered_set<std::string> seen;
        seen.reserve(results.size());
        std::vector<iOptronNetworkHostInfo> deduped;
        deduped.reserve(results.size());
        for (auto& r : results) {
            std::string key = r.host + ":" + std::to_string(r.tcp_port);
            if (seen.insert(key).second) {
                deduped.push_back(std::move(r));
            }
        }
        results = std::move(deduped);
    }
#endif

    return results;
}

namespace {

std::string strip_status_prefix(const std::string& response) {
    auto sign_pos = response.find_first_of("+-");
    if (sign_pos == std::string::npos || sign_pos == 0) {
        return response;
    }
    for (std::size_t i = 0; i < sign_pos; ++i) {
        char ch = response[i];
        if (ch != '0' && ch != '1') {
            return response;
        }
    }
    return response.substr(sign_pos);
}

// Parse a decimal integer field from a device response. A raw std::stoll/stoi
// on a garbled response throws std::invalid_argument/std::out_of_range, which
// escapes the Alpaca error mapping — convert to AlpacaException instead.
int64_t parse_int64_field(const std::string& text, const char* context) {
    try {
        std::size_t consumed = 0;
        int64_t value = std::stoll(text, &consumed);
        if (consumed == 0) {
            throw std::invalid_argument("empty");
        }
        return value;
    } catch (const std::exception&) {
        throw AlpacaException(std::string("Invalid numeric field in mount response (") + context + "): \"" + text +
                              "\"");
    }
}

} // namespace

// PIMPL implementation class
class iOptronProtocolWrapper::Impl {
public:
    Impl() : connected_(false), connection_type_(ConnectionType::Serial) {
        serial_fd_ = -1;
        socket_fd_ = -1;
    }

    ~Impl() {
        disconnect();
    }
    
    bool connect(const ConnectionInfo& info) {
        ALPACA_LOG_INFO("iOptron", "Impl::connect() called");
        
        // Validate connection info before proceeding
        if (info.type == ConnectionType::Serial) {
            if (info.port_path.empty()) {
                ALPACA_LOG_ERROR("iOptron", "Serial connection requested but port_path is empty");
                return false;
            }
            ALPACA_LOG_INFO("iOptron", "Serial connection - port: [" + info.port_path + "], baud: " + std::to_string(info.baud_rate));
        } else {
            if (info.host.empty()) {
                ALPACA_LOG_ERROR("iOptron", "Network connection requested but host is empty");
                return false;
            }
            ALPACA_LOG_INFO("iOptron", "Network connection - host: [" + info.host + "], port: " + std::to_string(info.tcp_port));
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        ALPACA_LOG_INFO("iOptron", "Got mutex lock");
        
        if (connected_) {
            // The protocol wrapper is a per-vendor singleton: a second device
            // connecting through it would silently steal/tear down the first
            // device's connection. Refuse instead.
            throw AlpacaException(
                "Only one iOptron mount per bridge: the shared iOptron "
                "protocol wrapper is already connected");
        }
        
        ALPACA_LOG_INFO("iOptron", "Connection type: " + std::string(info.type == ConnectionType::Serial ? "Serial" : "Network"));
        connection_type_ = info.type;
        connection_info_ = info;
        
        bool success = false;
        if (info.type == ConnectionType::Serial) {
            ALPACA_LOG_INFO("iOptron", "Connecting via serial, port: [" + info.port_path + "], baud: " + std::to_string(info.baud_rate));
            // Make a copy of port_path to ensure it's valid
            std::string port_path_copy = info.port_path;
            ALPACA_LOG_INFO("iOptron", "Made copy of port_path, calling connect_serial()...");
            success = connect_serial(port_path_copy, info.baud_rate);
            ALPACA_LOG_INFO("iOptron", "connect_serial() returned: " + std::string(success ? "true" : "false"));
        } else {
            ALPACA_LOG_INFO("iOptron", "Connecting via network, host: [" + info.host + "], port: " + std::to_string(info.tcp_port));
            // Make a copy of host to ensure it's valid
            std::string host_copy = info.host;
            ALPACA_LOG_INFO("iOptron", "Made copy of host, calling connect_network()...");
            success = connect_network(host_copy, info.tcp_port);
            ALPACA_LOG_INFO("iOptron", "connect_network() returned: " + std::string(success ? "true" : "false"));
        }
        
        if (success) {
            connected_ = true;
            std::string conn_type = (info.type == ConnectionType::Serial ? "Serial" : "Network");
            ALPACA_LOG_INFO("iOptron", "Connected to mount via " + conn_type);
        } else {
            ALPACA_LOG_ERROR("iOptron", "Failed to connect to mount");
        }
        
        return success;
    }

    bool is_network_connection() const {
        return connection_type_ == ConnectionType::Network;
    }
    
    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    // Tear down the connection. Caller MUST already hold mutex_ (so connect()
    // can reuse it without the non-recursive mutex deadlocking on re-lock).
    void disconnect_locked() {
        if (!connected_) {
            return;
        }

        if (connection_type_ == ConnectionType::Serial) {
            disconnect_serial();
        } else {
            disconnect_network();
        }

        connected_ = false;
        ALPACA_LOG_INFO("iOptron", "Disconnected from mount");
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }
    
    std::string send_command(const std::string& command, bool require_hash_terminator, int timeout_ms_override) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }
        
        const auto start = std::chrono::steady_clock::now();

        // Format command with # terminator
        std::string full_command = command;
        if (full_command.empty() || full_command.back() != '#') {
            full_command += "#";
        }
        
        // Send command
        if (!write_data(full_command)) {
            throw AlpacaException("Failed to send command to mount");
        }
        
        // Read response
        int timeout_ms = timeout_ms_override;
        if (timeout_ms <= 0) {
            timeout_ms = connection_info_.response_timeout_ms;
        }
        if (timeout_ms <= 0) {
            timeout_ms = 5000;
        }
        std::string response = read_response(require_hash_terminator, timeout_ms);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        ALPACA_LOG_TRACE("iOptron", "CMD " + full_command + " RESP " + response +
                                       " (" + std::to_string(elapsed.count()) + " ms)");
        
        return response;
    }
    
    void send_command_blind(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }

        std::string full_command = command;
        if (full_command.empty() || full_command.back() != '#') {
            full_command += "#";
        }

        ALPACA_LOG_TRACE("iOptron", "CMD " + full_command + " (blind)");

        if (!write_data(full_command)) {
            throw AlpacaException("Failed to send command to mount");
        }

        // Over TCP, "blind" commands still produce acknowledgment bytes.
        // Drain them to prevent stale data from accumulating in the receive
        // buffer, which overwhelms the mount's WiFi module on rapid-fire
        // command sequences.
        if (connection_type_ == ConnectionType::Network) {
            drain_network_stale(50);
        }
    }

    void drain_network_stale(int wait_ms) {
#ifndef _WIN32
        struct pollfd pfd{};
        pfd.fd = socket_fd_;
        pfd.events = POLLIN;
        while (true) {
            int ret = poll(&pfd, 1, wait_ms);
            if (ret <= 0) break;
            char ch;
            ssize_t n = recv(socket_fd_, &ch, 1, 0);
            if (n <= 0) break;
            wait_ms = 0;
        }
#else
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_handle_, &readfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = wait_ms * 1000;
        while (true) {
            int ret = select(0, &readfds, nullptr, nullptr, &tv);
            if (ret <= 0) break;
            char ch;
            int n = recv(socket_handle_, &ch, 1, 0);
            if (n <= 0) break;
            tv.tv_usec = 0;
            FD_ZERO(&readfds);
            FD_SET(socket_handle_, &readfds);
        }
#endif
    }

    void flush_input() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            return;
        }
        if (connection_type_ == ConnectionType::Serial) {
#ifndef _WIN32
            if (serial_fd_ >= 0) {
                tcflush(serial_fd_, TCIFLUSH);
            }
#else
            if (serial_handle_ != INVALID_HANDLE_VALUE) {
                PurgeComm(serial_handle_, PURGE_RXCLEAR);
            }
#endif
        } else {
            char discard[64];
            for (int i = 0; i < 16; ++i) {
                bool got = read_network_char(discard[0]);
                if (!got) break;
            }
        }
    }

private:
    bool connect_serial(const std::string& port_path, int baud_rate) {
        ALPACA_LOG_INFO("iOptron", "connect_serial() called with port: [" + port_path + "], baud: " + std::to_string(baud_rate));
#ifdef _WIN32
        ALPACA_LOG_INFO("iOptron", "Windows serial port path");
        // Windows serial port
        std::wstring wport_path(port_path.begin(), port_path.end());
        serial_handle_ = CreateFileW(
            wport_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        
        if (serial_handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        
        DCB dcb = {0};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        
        dcb.BaudRate = baud_rate;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        
        if (!SetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        
        // Set timeouts
        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 100;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 100;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(serial_handle_, &timeouts);
        
        return true;
#else
        // Linux/macOS serial port
        ALPACA_LOG_INFO("iOptron", "Opening serial port: [" + port_path + "]");
        
        // Validate port_path is not empty
        if (port_path.empty()) {
            ALPACA_LOG_ERROR("iOptron", "Port path is empty!");
            return false;
        }
        
        // Make sure we have a valid C string
        const char* port_cstr = port_path.c_str();
        if (port_cstr == nullptr) {
            ALPACA_LOG_ERROR("iOptron", "Port path C string is null!");
            return false;
        }
        
        ALPACA_LOG_INFO("iOptron", "Got C string pointer: [" + std::string(port_cstr) + "], calling open()...");
        
        // Call open() with error handling
        errno = 0;  // Clear errno before call
        serial_fd_ = open(port_cstr, O_RDWR | O_NOCTTY | O_NONBLOCK);
        int open_errno = errno;
        ALPACA_LOG_INFO("iOptron", "open() returned: " + std::to_string(serial_fd_) + ", errno: " + std::to_string(open_errno));
        if (serial_fd_ < 0) {
            const char* errmsg = (open_errno != 0) ? std::strerror(open_errno) : "unknown";
            ALPACA_LOG_ERROR("iOptron", "Failed to open serial port [" + port_path + "]: " + std::string(errmsg) + " (errno " + std::to_string(open_errno) + "). Check port exists, permissions (e.g. user in dialout group), and that no other process has it open.");
            return false;
        }
        
        ALPACA_LOG_INFO("iOptron", "Getting terminal attributes...");
        // Configure serial port
        struct termios tty;
        if (tcgetattr(serial_fd_, &tty) != 0) {
            int tc_err = errno;
            ALPACA_LOG_ERROR("iOptron", "tcgetattr failed: " + std::string(std::strerror(tc_err)) + " (errno " + std::to_string(tc_err) + ")");
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        ALPACA_LOG_INFO("iOptron", "Got terminal attributes");
        
        // Set baud rate
        speed_t speed = B9600;
        switch (baud_rate) {
            case 9600: speed = B9600; break;
            case 19200: speed = B19200; break;
            case 38400: speed = B38400; break;
            case 57600: speed = B57600; break;
            case 115200: speed = B115200; break;
            default: speed = B9600; break;
        }
        
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);
        
        // 8N1 configuration
        tty.c_cflag &= ~PARENB;  // No parity
        tty.c_cflag &= ~CSTOPB;   // 1 stop bit
        tty.c_cflag &= ~CSIZE;    // Clear size bits
        tty.c_cflag |= CS8;       // 8 data bits
        tty.c_cflag &= ~CRTSCTS;  // No hardware flow control
        tty.c_cflag |= CREAD | CLOCAL;  // Enable receiver, ignore modem controls
        
        // Input flags
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // No software flow control
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        
        // Output flags
        tty.c_oflag &= ~OPOST;
        
        // Local flags
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        
        // Set timeouts
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;  // 0.1 second timeout
        
        ALPACA_LOG_INFO("iOptron", "Setting terminal attributes...");
        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            int tc_err = errno;
            ALPACA_LOG_ERROR("iOptron", "tcsetattr failed: " + std::string(std::strerror(tc_err)) + " (errno " + std::to_string(tc_err) + ")");
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        ALPACA_LOG_INFO("iOptron", "Set terminal attributes");

        // Set to blocking mode (matches the probe path and the other wrappers).
        ALPACA_LOG_INFO("iOptron", "Setting to blocking mode...");
        if (!util::clear_nonblocking(serial_fd_)) {
            int fc_err = errno;
            ALPACA_LOG_ERROR("iOptron", "Clearing O_NONBLOCK failed: " + std::string(std::strerror(fc_err)) +
                                            " (errno " + std::to_string(fc_err) + ")");
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        ALPACA_LOG_INFO("iOptron", "Serial port configured successfully");
        
        return true;
#endif
    }
    
    void disconnect_serial() {
#ifdef _WIN32
        if (serial_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
#endif
    }
    
    bool connect_network(const std::string& host, int port) {
        ALPACA_LOG_INFO("iOptron", "connect_network() called with host: [" + host + "], port: " + std::to_string(port));
#ifdef _WIN32
        if (port < 0 || port > static_cast<int>(std::numeric_limits<u_short>::max())) {
            ALPACA_LOG_ERROR("iOptron", "Network port out of range: " + std::to_string(port));
            return false;
        }
        const u_short port_value = static_cast<u_short>(port);
        socket_handle_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_handle_ == INVALID_SOCKET) {
            return false;
        }
        
        sockaddr_in addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_value);
        
        // Resolve hostname
        addrinfo hints{};
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
            return false;
        }
        
        addr.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
        freeaddrinfo(result);
        
        if (::connect(socket_handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
            return false;
        }

        configure_network_timeouts();
        
        // Set socket to blocking mode
        u_long mode = 0;
        ioctlsocket(socket_handle_, FIONBIO, &mode);
        
        return true;
#else
        constexpr int kConnectTimeoutMs = 7000;
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }

        sockaddr_in addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Resolve hostname — getaddrinfo is the reentrant replacement for
        // gethostbyname (mirrors the Windows branch above).
        addrinfo hints{};
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);

        // Non-blocking connect with a bounded poll timeout: this runs under the
        // driver mutex, and a bare blocking connect() to an unreachable host
        // would stall every GET (and disconnect) for the full OS TCP timeout
        // (~127 s). Same pattern as the network prober above.
        if (!util::set_nonblocking(socket_fd_)) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        int rc = ::connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        if (rc < 0) {
            struct pollfd pfd {};
            pfd.fd = socket_fd_;
            pfd.events = POLLOUT;
            int poll_rc = poll(&pfd, 1, kConnectTimeoutMs);
            int sock_err = 0;
            socklen_t len = sizeof(sock_err);
            if (poll_rc <= 0 || getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &sock_err, &len) != 0 || sock_err != 0) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
        }
        // Restore blocking mode for the synchronous request/response I/O below;
        // a stuck-non-blocking fd would make every recv spin with EAGAIN.
        if (!util::clear_nonblocking(socket_fd_)) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        configure_network_timeouts();

        return true;
#endif
    }
    
    void disconnect_network() {
#ifdef _WIN32
        if (socket_handle_ != INVALID_SOCKET) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
        }
#else
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
#endif
    }
    
    bool write_data(const std::string& data) {
        if (connection_type_ == ConnectionType::Serial) {
            return write_serial(data);
        } else {
            return write_network(data);
        }
    }
    
    bool write_serial(const std::string& data) {
#ifdef _WIN32
        const auto data_size = data.size();
        if (data_size > std::numeric_limits<DWORD>::max()) {
            return false;
        }
        // Loop until the whole payload is written so a short WriteFile (which can
        // return TRUE with bytes_written < requested under backpressure) can't
        // drop trailing bytes and corrupt framing (mirrors POSIX write_all).
        std::size_t total = 0;
        while (total < data_size) {
            DWORD bytes_written = 0;
            if (!WriteFile(serial_handle_, data.c_str() + total, static_cast<DWORD>(data_size - total), &bytes_written,
                           nullptr) ||
                bytes_written == 0) {
                return false;
            }
            total += bytes_written;
        }
        return true;
#else
        return util::write_all(serial_fd_, data.c_str(), data.length());
#endif
    }

    bool write_network(const std::string& data) {
#ifdef _WIN32
        const auto data_size = data.size();
        if (data_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        // Loop over short sends so partial bytes can't corrupt the next command's
        // framing (mirrors the POSIX send_all path below).
        std::size_t total = 0;
        while (total < data_size) {
            int bytes_sent = send(socket_handle_, data.c_str() + total, static_cast<int>(data_size - total), 0);
            if (bytes_sent <= 0) {
                return false;
            }
            total += static_cast<std::size_t>(bytes_sent);
        }
        return true;
#else
        // MSG_NOSIGNAL: a peer disconnect mid-send must not deliver SIGPIPE and
        // kill the server. send_all loops over short sends so partial bytes
        // can't corrupt the next command's framing on this connection.
        return util::send_all(socket_fd_, data.c_str(), data.length(), MSG_NOSIGNAL);
#endif
    }

    std::string read_response(bool require_hash_terminator, int timeout_ms) {
        std::string response;
        auto start = std::chrono::steady_clock::now();
        
        auto last_char_time = start;
        const int idle_break_ms = 100;
        
        while (true) {
            char ch;
            bool got_char = false;
            
            if (connection_type_ == ConnectionType::Serial) {
                got_char = read_serial_char(ch);
            } else {
                got_char = read_network_char(ch);
            }
            
            if (got_char) {
                last_char_time = std::chrono::steady_clock::now();

                // TCP bridges can insert CR/LF; ignore them to keep parsing aligned.
                if (ch == '\r' || ch == '\n') {
                    continue;
                }

                if (ch == '#') {
                    break;  // Command terminator found
                }
                
                response += ch;
            }
            
            // Check timeout
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            if (elapsed.count() > timeout_ms) {
                if (!require_hash_terminator) {
                    return response;
                }
                throw AlpacaException("Timeout waiting for mount response");
            }

            if (!require_hash_terminator && !got_char && !response.empty()) {
                auto idle_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_char_time);
                if (idle_elapsed.count() > idle_break_ms) {
                    break;
                }
            }
            
            // Small delay to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return response;
    }
    
    bool read_serial_char(char& ch) {
#ifdef _WIN32
        DWORD bytes_read = 0;
        if (ReadFile(serial_handle_, &ch, 1, &bytes_read, nullptr) && bytes_read == 1) {
            return true;
        }
        return false;
#else
        ssize_t bytes_read = read(serial_fd_, &ch, 1);
        return bytes_read == 1;
#endif
    }
    
    bool read_network_char(char& ch) {
#ifdef _WIN32
        int bytes_received = recv(socket_handle_, &ch, 1, 0);
        return bytes_received == 1;
#else
        ssize_t bytes_received = recv(socket_fd_, &ch, 1, 0);
        return bytes_received == 1;
#endif
    }

    void configure_network_timeouts() {
        constexpr int kSocketTimeoutMs = 200;
        int nodelay = 1;
#ifdef _WIN32
        DWORD timeout = kSocketTimeoutMs;
        setsockopt(socket_handle_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_handle_, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_handle_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#else
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = kSocketTimeoutMs * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#endif
    }

    
public:
    // Helper: Convert RA hours to iOptron format (0.01 arc-seconds)
    int64_t ra_to_ioptron_format(double ra_hours) {
        // RA in hours -> degrees -> arc-seconds -> 0.01 arc-seconds
        double ra_degrees = units::hours_to_deg(ra_hours);
        double ra_arcsec = ra_degrees * 3600.0;
        return static_cast<int64_t>(std::round(ra_arcsec * 100.0));
    }
    
    // Helper: Convert Dec degrees to iOptron format (0.01 arc-seconds)
    int64_t dec_to_ioptron_format(double dec_degrees) {
        double dec_arcsec = dec_degrees * 3600.0;
        return static_cast<int64_t>(std::round(dec_arcsec * 100.0));
    }
    
    // Helper: Convert iOptron format (0.01 arc-seconds) to RA hours
    double ioptron_format_to_ra(int64_t value) {
        double ra_arcsec = value / 100.0;
        double ra_degrees = ra_arcsec / 3600.0;
        return units::deg_to_hours(ra_degrees);
    }
    
    // Helper: Convert iOptron format (0.01 arc-seconds) to Dec degrees
    double ioptron_format_to_dec(int64_t value) {
        double dec_arcsec = value / 100.0;
        return dec_arcsec / 3600.0;
    }
    
    // Helper: Parse signed integer from string
    int64_t parse_signed_int(const std::string& str) {
        if (str.empty()) return 0;
        bool negative = (str[0] == '-');
        size_t start = (str[0] == '-' || str[0] == '+') ? 1 : 0;
        return (negative ? -1 : 1) * parse_int64_field(str.substr(start), "signed field");
    }
    
    // Helper: Format signed integer to string with sign
    std::string format_signed_int(int64_t value, int width) {
        std::ostringstream oss;
        if (value >= 0) {
            oss << "+";
        } else {
            oss << "-";
            value = -value;
        }
        oss << std::setfill('0') << std::setw(width) << value;
        return oss.str();
    }

private:
    mutable std::mutex mutex_;
    bool connected_;
    ConnectionType connection_type_;
    ConnectionInfo connection_info_;
    
#ifdef _WIN32
    HANDLE serial_handle_;
    SOCKET socket_handle_;
#else
    int serial_fd_;
    int socket_fd_;
#endif
};

// Singleton instance
iOptronProtocolWrapper& iOptronProtocolWrapper::instance() {
    static iOptronProtocolWrapper wrapper;
    return wrapper;
}

// Public interface implementation
// Constructor must initialize pimpl_
iOptronProtocolWrapper::iOptronProtocolWrapper() 
    : pimpl_(std::make_unique<Impl>()) {
}

iOptronProtocolWrapper::~iOptronProtocolWrapper() = default;

bool iOptronProtocolWrapper::connect(const ConnectionInfo& info) {
    return pimpl_->connect(info);
}

void iOptronProtocolWrapper::disconnect() {
    pimpl_->disconnect();
}

bool iOptronProtocolWrapper::is_connected() const {
    return pimpl_->is_connected();
}

std::string iOptronProtocolWrapper::send_command(const std::string& command,
                                                 bool require_hash_terminator,
                                                 int timeout_ms_override) {
    return pimpl_->send_command(command, require_hash_terminator, timeout_ms_override);
}

void iOptronProtocolWrapper::send_command_blind(const std::string& command) {
    pimpl_->send_command_blind(command);
}

void iOptronProtocolWrapper::flush_input() {
    pimpl_->flush_input();
}

// Mount information queries
MountInfo iOptronProtocolWrapper::get_mount_info() {
    MountInfo info;
    try {
        std::string response = send_command(":MountInfo", false);
        info.model_code = response;
        info.model_name = model_code_to_name(response);
        info.has_encoder = (response == "0027" || response == "0029" ||
                            response == "0030" || response == "0032" ||
                            response == "0034" || response == "0037" ||
                            response == "0039" || response == "0041" ||
                            response == "0045" || response == "0047" ||
                            response == "0049" || response == "0051" ||
                            response == "0054" || response == "0056" ||
                            response == "0061" || response == "0063" ||
                            response == "0065" || response == "0067" ||
                            response == "0069" || response == "0071" ||
                            response == "0072" || response == "0121" ||
                            response == "0122");
    } catch (const std::exception&) {
    }
    return info;
}

Position iOptronProtocolWrapper::get_position() {
    std::string response = send_command(":GEP");

    response = strip_status_prefix(std::move(response));
    
    if (response.length() < 20) {
        throw AlpacaException("Invalid position response from mount");
    }
    
    // Parse :GEP# response: sTTTTTTTTTTTTTTTTTnn#
    // Sign + 8 digits: Dec (0.01 arc-seconds)
    // 9 digits: RA (0.01 arc-seconds)
    // 18th digit: side of pier (0=pier east, 1=pier west, 2=indeterminate)
    // 19th digit: pointing state (0=counterweight up, 1=normal)
    
    std::string dec_str = response.substr(0, 9);  // Sign + 8 digits
    std::string ra_str = response.substr(9, 9);   // 9 digits
    
    int64_t dec_value = pimpl_->parse_signed_int(dec_str);
    int64_t ra_value = parse_int64_field(ra_str, ":GEP RA");

    Position pos;
    pos.dec_degrees = pimpl_->ioptron_format_to_dec(dec_value);
    pos.ra_hours = pimpl_->ioptron_format_to_ra(ra_value);
    pos.side_of_pier = response[18] - '0';
    pos.pointing_state = response[19] - '0';
    
    return pos;
}

AltAz iOptronProtocolWrapper::get_alt_az() {
    std::string response = send_command(":GAC");

    response = strip_status_prefix(std::move(response));
    
    if (response.length() < 17) {
        throw AlpacaException("Invalid Alt/Az response from mount");
    }
    
    // Parse :GAC# response: sTTTTTTTTTTTTTTTTT#
    // Sign + 8 digits: Alt (0.01 arc-seconds)
    // 9 digits: Az (0.01 arc-seconds)
    
    std::string alt_str = response.substr(0, 9);  // Sign + 8 digits
    std::string az_str = response.substr(9, 9);   // 9 digits
    
    int64_t alt_value = pimpl_->parse_signed_int(alt_str);
    int64_t az_value = parse_int64_field(az_str, ":GAC azimuth");

    AltAz altaz;
    altaz.altitude_degrees = pimpl_->ioptron_format_to_dec(alt_value);
    altaz.azimuth_degrees = pimpl_->ioptron_format_to_dec(az_value);
    
    return altaz;
}

MountStatus iOptronProtocolWrapper::get_status() {
    std::string response = send_command(":GLS");

    response = strip_status_prefix(std::move(response));
    
    if (response.length() < 23) {
        throw AlpacaException("Invalid status response from mount");
    }
    
    // Parse :GLS# response: sTTTTTTTTTTTTTTTTnnnnnn#
    // Various status digits at positions 17-22 (1-based, after the sign)
    
    MountStatus status;
    int system_status = response[18] - '0';
    int tracking_rate_digit = response[19] - '0';
    
    status.system_status = system_status;
    status.tracking_rate = tracking_rate_digit;
    status.is_tracking = (system_status == 1 || system_status == 5);
    status.is_slewing = (system_status == 2);
    status.is_parked = (system_status == 6);
    status.is_at_home = (system_status == 7);
    
    return status;
}

SiteInfo iOptronProtocolWrapper::get_site_info() {
    std::string gls_response = send_command(":GLS");
    std::string gut_response = send_command(":GUT");

    gls_response = strip_status_prefix(std::move(gls_response));
    gut_response = strip_status_prefix(std::move(gut_response));
    
    if (gls_response.length() < 23 || gut_response.length() < 17) {
        throw AlpacaException("Invalid site info response from mount");
    }
    
    SiteInfo site;
    
    // Parse longitude (first 8 digits after sign)
    std::string lon_str = gls_response.substr(0, 9);
    int64_t lon_value = pimpl_->parse_signed_int(lon_str);
    site.longitude_degrees = pimpl_->ioptron_format_to_dec(lon_value);
    
    // Parse latitude (next 8 digits, but stored as lat + 90 degrees)
    std::string lat_str = gls_response.substr(9, 8);
    int64_t lat_offset = parse_int64_field(lat_str, ":GLS latitude");
    // Convert back: stored value = lat + 90 degrees
    double lat_arcsec = (lat_offset / 100.0) - (90.0 * 3600.0);
    site.latitude_degrees = lat_arcsec / 3600.0;
    
    // Hemisphere (22nd digit)
    site.is_northern_hemisphere = (gls_response[22] == '1');
    
    // Timezone and DST from :GUT#
    std::string tz_str = gut_response.substr(0, 4);  // Sign + 3 digits
    site.timezone_offset_minutes = static_cast<int>(pimpl_->parse_signed_int(tz_str));
    site.dst_observed = (gut_response[4] == '1');
    
    return site;
}

AltAz iOptronProtocolWrapper::get_park_position() {
    std::string response = send_command(":GPC");
    
    if (response.length() < 17) {
        throw AlpacaException("Invalid park position response from mount");
    }
    
    // Parse :GPC# response: TTTTTTTTTTTTTTTTT#
    // 8 digits: Alt (0.01 arc-seconds)
    // 9 digits: Az (0.01 arc-seconds)
    
    std::string alt_str = response.substr(0, 8);
    std::string az_str = response.substr(8, 9);

    int64_t alt_value = parse_int64_field(alt_str, ":GPC altitude");
    int64_t az_value = parse_int64_field(az_str, ":GPC azimuth");

    AltAz park;
    park.altitude_degrees = pimpl_->ioptron_format_to_dec(alt_value);
    park.azimuth_degrees = pimpl_->ioptron_format_to_dec(az_value);
    
    return park;
}

int iOptronProtocolWrapper::get_altitude_limit_degrees() {
    std::string response = send_command(":GAL");
    if (response.empty()) {
        throw AlpacaException("Invalid altitude limit response from mount");
    }
    int64_t limit = pimpl_->parse_signed_int(response);
    return static_cast<int>(limit);
}

void iOptronProtocolWrapper::set_altitude_limit_degrees(int limit_degrees) {
    if (limit_degrees < -89) {
        limit_degrees = -89;
    } else if (limit_degrees > 89) {
        limit_degrees = 89;
    }
    std::string cmd = ":SAL" + pimpl_->format_signed_int(limit_degrees, 2) + "#";
    std::string response = send_command(cmd, false);
    if (!response.empty() && response != "1") {
        ALPACA_LOG_WARN("iOptron", "Unexpected :SAL response: " + response);
    }
}

MeridianTreatment iOptronProtocolWrapper::get_meridian_treatment() {
    std::string response = send_command(":GMT");
    if (response.size() < 3) {
        throw AlpacaException("Invalid meridian treatment response from mount");
    }
    MeridianTreatment treatment;
    treatment.behavior = response[0] - '0';
    treatment.degrees_past = static_cast<int>(parse_int64_field(response.substr(1, 2), ":GMT degrees"));
    return treatment;
}

void iOptronProtocolWrapper::set_meridian_treatment(int behavior, int degrees_past) {
    if (behavior != 0 && behavior != 1) {
        behavior = 0;
    }
    if (degrees_past < 0) {
        degrees_past = 0;
    } else if (degrees_past > 90) {
        degrees_past = 90;
    }
    std::ostringstream cmd;
    cmd << ":SMT" << behavior << std::setfill('0') << std::setw(2) << degrees_past << "#";
    std::string response = send_command(cmd.str(), false);
    if (!response.empty() && response != "1") {
        ALPACA_LOG_WARN("iOptron", "Unexpected :SMT response: " + response);
    }
}

// Mount motion commands
void iOptronProtocolWrapper::set_target_ra(double ra_hours) {
    int64_t ra_value = pimpl_->ra_to_ioptron_format(ra_hours);
    if (ra_value < 0) {
        ra_value = 0;
    } else if (ra_value > 129600000) {
        ra_value = 129600000;
    }
    std::ostringstream cmd;
    cmd << ":SRA" << std::setfill('0') << std::setw(9) << ra_value << "#";
    send_command(cmd.str(), false);
}

void iOptronProtocolWrapper::set_target_dec(double dec_degrees) {
    int64_t dec_value = pimpl_->dec_to_ioptron_format(dec_degrees);
    if (dec_value < -32400000) {
        dec_value = -32400000;
    } else if (dec_value > 32400000) {
        dec_value = 32400000;
    }
    // iOptron expects ":Sd" + signed 8-digit dec (0.01 arc-seconds).
    std::string cmd = ":Sd" + pimpl_->format_signed_int(dec_value, 8) + "#";
    send_command(cmd, false);
}

bool iOptronProtocolWrapper::slew_to_ra_dec() {
    flush_input();
    try {
        std::string response = send_command(":MS1", false);
        if (response == "0") {
            return false;
        }
        if (!response.empty() && response != "1") {
            ALPACA_LOG_WARN("iOptron", "Unexpected :MS1 response: " + response);
        }
        return true;
    } catch (const AlpacaException& e) {
        const std::string message = e.what();
        if (pimpl_->is_network_connection() &&
            message.find("Timeout waiting for mount response") != std::string::npos) {
            // TODO: Confirm HAE29C WiFi behavior for :MS1 responses.
            ALPACA_LOG_WARN("iOptron", "No :MS1 response over network; assuming slew accepted");
            return true;
        }
        throw;
    }
}

bool iOptronProtocolWrapper::slew_to_ra_dec_cw_up() {
    flush_input();
    try {
        std::string response = send_command(":MS2", false);
        if (response == "0") {
            return false;
        }
        if (!response.empty() && response != "1") {
            ALPACA_LOG_WARN("iOptron", "Unexpected :MS2 response: " + response);
        }
        return true;
    } catch (const AlpacaException& e) {
        const std::string message = e.what();
        if (pimpl_->is_network_connection() &&
            message.find("Timeout waiting for mount response") != std::string::npos) {
            // TODO: Confirm HAE29C WiFi behavior for :MS2 responses.
            ALPACA_LOG_WARN("iOptron", "No :MS2 response over network; assuming slew accepted");
            return true;
        }
        throw;
    }
}

void iOptronProtocolWrapper::stop_slewing() {
    send_command_blind(":Q#");
}

void iOptronProtocolWrapper::start_tracking() {
    send_command_blind(":ST1#");
}

void iOptronProtocolWrapper::stop_tracking() {
    send_command_blind(":ST0#");
}

void iOptronProtocolWrapper::sync_to_coordinates() {
    send_command(":CM", false);
}

// Parking commands
bool iOptronProtocolWrapper::park() {
    // Some iOptron mounts go quiet while parking and never return a response.
    send_command_blind(":MP1#");
    return true;
}

void iOptronProtocolWrapper::unpark() {
    // Some mounts do not respond to unpark, especially after power-cycle auto-unpark.
    send_command_blind(":MP0#");
}

void iOptronProtocolWrapper::set_park_position(double alt_degrees, double az_degrees) {
    int64_t alt_value = pimpl_->dec_to_ioptron_format(alt_degrees);
    int64_t az_value = pimpl_->dec_to_ioptron_format(az_degrees);

    // Protocol expects fixed-width zero-padded fields (matching :GPC's layout):
    // :SPH takes 8 digits (park altitude, 0..90° in 0.01"), :SPA takes 9 digits
    // (park azimuth, 0..360° in 0.01"). An unpadded to_string() produces a
    // malformed command the mount rejects or misparses.
    alt_value = std::clamp<int64_t>(alt_value, 0, 32400000);
    az_value = std::clamp<int64_t>(az_value, 0, 129600000);

    std::ostringstream alt_cmd;
    alt_cmd << ":SPH" << std::setfill('0') << std::setw(8) << alt_value << "#";
    std::ostringstream az_cmd;
    az_cmd << ":SPA" << std::setfill('0') << std::setw(9) << az_value << "#";

    send_command(alt_cmd.str(), false);
    send_command(az_cmd.str(), false);
}

// Home position commands
void iOptronProtocolWrapper::go_to_home() {
    // Some mounts do not respond while slewing to home.
    send_command_blind(":MH#");
}

void iOptronProtocolWrapper::find_home() {
    send_command(":MSH");
}

// Pulse guiding commands
void iOptronProtocolWrapper::pulse_guide(int direction, int duration_ms) {
    if (duration_ms < 0 || duration_ms > 99999) {
        throw AlpacaException("Pulse guide duration must be 0-99999 ms");
    }
    
    std::ostringstream cmd;
    cmd << ":";
    
    // Direction: 0=North (Dec+), 1=South (Dec-), 2=East (RA+), 3=West (RA-)
    if (direction == 0) cmd << "ZE";      // Dec+
    else if (direction == 1) cmd << "ZC"; // Dec-
    else if (direction == 2) cmd << "ZS"; // RA+
    else if (direction == 3) cmd << "ZQ"; // RA-
    else throw AlpacaException("Invalid pulse guide direction");
    
    cmd << std::setfill('0') << std::setw(5) << duration_ms << "#";
    send_command_blind(cmd.str());
}

// Site settings
void iOptronProtocolWrapper::set_longitude(double longitude_degrees) {
    int64_t lon_value = pimpl_->dec_to_ioptron_format(longitude_degrees);
    std::string cmd = ":SLO" + pimpl_->format_signed_int(lon_value, 8) + "#";
    send_command(cmd, false);
}

void iOptronProtocolWrapper::set_latitude(double latitude_degrees) {
    int64_t lat_value = pimpl_->dec_to_ioptron_format(latitude_degrees);
    std::string cmd = ":SLA" + pimpl_->format_signed_int(lat_value, 8) + "#";
    send_command(cmd, false);
}

void iOptronProtocolWrapper::set_hemisphere(bool is_northern) {
    send_command_blind(is_northern ? ":SHE1#" : ":SHE0#");
}

void iOptronProtocolWrapper::set_utc_time(std::chrono::system_clock::time_point utc_time) {
    // Convert to milliseconds since J2000
    // J2000 = 2000-01-01 12:00:00 UTC
    auto j2000 = std::chrono::system_clock::time_point(
        std::chrono::seconds(946728000));  // J2000 epoch
    
    auto duration = utc_time - j2000;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    
    std::ostringstream cmd;
    cmd << ":SUT" << std::setfill('0') << std::setw(13) << ms << "#";
    // Some mounts do not respond to :SUT.
    send_command_blind(cmd.str());
}

std::chrono::system_clock::time_point iOptronProtocolWrapper::get_utc_time() {
    // Some mounts reply to :GUT without a trailing '#', so allow responses
    // that omit the terminator.
    std::string response = send_command(":GUT", false);
    response = strip_status_prefix(std::move(response));
    if (response.length() < 18) {
        throw AlpacaException("Invalid UTC time response from mount");
    }
    
    // Response: sMMMnXXXXXXXXXXXXX
    // Sign + 3 digits: timezone offset (ignored here)
    // Next digit: DST flag
    // Remaining digits: time in milliseconds since J2000
    std::string tz_str = response.substr(0, 4);
    int tz_offset_minutes = static_cast<int>(pimpl_->parse_signed_int(tz_str));
    bool dst_observed = (response[4] == '1');
    std::string time_digits = response.substr(5);
    int64_t ms_since_j2000 = parse_int64_field(time_digits, ":GUT time");

    auto j2000 = std::chrono::system_clock::time_point(
        std::chrono::seconds(946728000));  // 2000-01-01 12:00:00 UTC
    auto raw_time = j2000 + std::chrono::milliseconds(ms_since_j2000);
    
    // Some mounts return local time in the GUT payload. If so, adjust using the
    // reported timezone/DST or the host's timezone when that brings us closer
    // to system UTC.
    int total_offset_minutes = tz_offset_minutes + (dst_observed ? 60 : 0);
    auto adjusted_by_mount = raw_time - std::chrono::minutes(total_offset_minutes);

    auto compute_host_offset_minutes = []() -> int {
        std::time_t now = std::time(nullptr);
        std::tm local_tm {};
        std::tm utc_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &now);
        gmtime_s(&utc_tm, &now);
#else
        local_tm = *std::localtime(&now);
        utc_tm = *std::gmtime(&now);
#endif
        std::time_t local_time = std::mktime(&local_tm);
        std::time_t utc_as_local = std::mktime(&utc_tm);
        double offset_seconds = std::difftime(local_time, utc_as_local);
        return static_cast<int>(std::llround(offset_seconds / 60.0));
    };

    int host_offset_minutes = compute_host_offset_minutes();
    auto adjusted_by_host = raw_time - std::chrono::minutes(host_offset_minutes);
    
    auto now = std::chrono::system_clock::now();
    auto raw_diff = (raw_time > now) ? (raw_time - now) : (now - raw_time);
    auto mount_diff = (adjusted_by_mount > now) ? (adjusted_by_mount - now) : (now - adjusted_by_mount);
    auto host_diff = (adjusted_by_host > now) ? (adjusted_by_host - now) : (now - adjusted_by_host);

    auto best_time = raw_time;
    auto best_diff = raw_diff;
    if (mount_diff + std::chrono::minutes(1) < best_diff) {
        best_time = adjusted_by_mount;
        best_diff = mount_diff;
    }
    if (host_diff + std::chrono::minutes(1) < best_diff) {
        best_time = adjusted_by_host;
        best_diff = host_diff;
    }
    return best_time;
}

void iOptronProtocolWrapper::set_timezone_offset(int offset_minutes) {
    if (offset_minutes < -720 || offset_minutes > 780) {
        throw AlpacaException("Timezone offset must be -720 to +780 minutes");
    }
    
    std::ostringstream cmd;
    cmd << ":SG";
    if (offset_minutes >= 0) {
        cmd << "+";
    } else {
        cmd << "-";
        offset_minutes = -offset_minutes;
    }
    cmd << std::setfill('0') << std::setw(3) << offset_minutes << "#";
    // Some mounts do not respond to :SG; fire-and-forget to avoid timeouts.
    send_command_blind(cmd.str());
}

void iOptronProtocolWrapper::set_dst_observed(bool observed) {
    // Some mounts do not respond to :SDS; fire-and-forget to avoid timeouts.
    send_command_blind(observed ? ":SDS1#" : ":SDS0#");
}

// Tracking rate commands
void iOptronProtocolWrapper::set_tracking_rate(int rate) {
    if (rate < 0 || rate > 4) {
        throw AlpacaException("Tracking rate must be 0-4");
    }
    std::ostringstream cmd;
    cmd << ":RT" << rate << "#";
    // Some mounts do not respond to :RT; fire-and-forget to avoid timeouts.
    send_command_blind(cmd.str());
}

double iOptronProtocolWrapper::get_custom_tracking_rate() {
    std::string response = send_command(":GTR");
    if (response.length() < 5) {
        throw AlpacaException("Invalid custom tracking rate response");
    }
    
    // Response is nnnnn# representing n.nnnn × sidereal
    // e.g., "10000" = 1.0000, "12000" = 1.2000
    int value = static_cast<int>(parse_int64_field(response, ":GTR rate"));
    return value / 10000.0;
}

void iOptronProtocolWrapper::set_custom_tracking_rate(double rate_multiplier) {
    if (rate_multiplier < 0.1000 || rate_multiplier > 1.9000) {
        throw AlpacaException("Custom tracking rate must be 0.1000 to 1.9000");
    }
    
    // Format as nnnnn (e.g., 1.0000 = 10000)
    int value = static_cast<int>(std::round(rate_multiplier * 10000.0));
    std::ostringstream cmd;
    cmd << ":RR" << std::setfill('0') << std::setw(5) << value << "#";
    // Some mounts do not respond to :RR; fire-and-forget to avoid timeouts.
    send_command_blind(cmd.str());
}

// Guiding rate commands
std::pair<double, double> iOptronProtocolWrapper::get_guide_rates() {
    std::string response = send_command(":AG");
    if (response.length() < 4) {
        throw AlpacaException("Invalid guide rate response");
    }
    
    // Response is nnnn#: first 2 digits = RA rate (0.nn), last 2 = Dec rate (0.nn)
    std::string ra_str = response.substr(0, 2);
    std::string dec_str = response.substr(2, 2);

    double ra_rate = static_cast<double>(parse_int64_field(ra_str, ":AG RA rate")) / 100.0;
    double dec_rate = static_cast<double>(parse_int64_field(dec_str, ":AG Dec rate")) / 100.0;

    // Suppress ABI change warning for std::pair return (C++14 vs C++17)
    #if defined(__GNUC__) && !defined(_MSC_VER)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpsabi"
    #endif
    return std::make_pair(ra_rate, dec_rate);
    #if defined(__GNUC__) && !defined(_MSC_VER)
    #pragma GCC diagnostic pop
    #endif
}

void iOptronProtocolWrapper::set_guide_rates(double ra_rate, double dec_rate) {
    if (ra_rate < 0.01 || ra_rate > 0.90) {
        throw AlpacaException("RA guide rate must be 0.01 to 0.90");
    }
    if (dec_rate < 0.10 || dec_rate > 0.99) {
        throw AlpacaException("Dec guide rate must be 0.10 to 0.99");
    }
    
    // Format as nnnn: RA (0.nn) + Dec (0.nn)
    int ra_value = static_cast<int>(std::round(ra_rate * 100.0));
    int dec_value = static_cast<int>(std::round(dec_rate * 100.0));
    
    std::ostringstream cmd;
    cmd << ":RG" << std::setfill('0') << std::setw(2) << ra_value
        << std::setfill('0') << std::setw(2) << dec_value << "#";
    // Some mounts do not respond to :RG; fire-and-forget to avoid timeouts.
    send_command_blind(cmd.str());
}

} // namespace alpacacore::vendor::ioptron

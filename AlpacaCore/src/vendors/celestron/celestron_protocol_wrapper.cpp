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
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <filesystem>
#endif

namespace alpacacore::vendor::celestron {

namespace {

// Probe a serial port for a NexStar hand controller using the echo command (Kx).
// Returns HC firmware version string (e.g. "5.35") on success, empty on failure.
std::string probe_nexstar_port(const std::string& port_path) {
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

    // NexStar spec: 9600 baud, 8N1, no flow control
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
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
    tty.c_cc[VTIME] = 20; // 2-second per-character timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return "";
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return "";
    }
    tcflush(fd, TCIOFLUSH);

    // Probe with NexStar echo command: "K" + chr(0x42) -> should return chr(0x42) + "#"
    const char echo_cmd[] = {'K', 0x42};
    if (!util::write_all(fd, echo_cmd, 2)) {
        close(fd);
        return "";
    }

    // Read response: expect chr(0x42) + '#'
    char resp[4] = {};
    int total = 0;
    auto start = std::chrono::steady_clock::now();
    while (total < 2) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > 3) {
            break;
        }
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            resp[total++] = ch;
            if (ch == '#') break;
        } else if (r == 0) {
            continue;
        } else {
            break;
        }
    }

    if (total < 2 || resp[0] != 0x42 || resp[1] != '#') {
        close(fd);
        return "";
    }

    // Echo confirmed — now query firmware version with "V" command
    tcflush(fd, TCIOFLUSH);
    const char ver_cmd[] = {'V'};
    if (!util::write_all(fd, ver_cmd, 1)) {
        close(fd);
        return "";
    }

    char ver_resp[4] = {};
    total = 0;
    start = std::chrono::steady_clock::now();
    while (total < 3) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > 3) {
            break;
        }
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            ver_resp[total++] = ch;
            if (ch == '#') break;
        } else if (r == 0) {
            continue;
        } else {
            break;
        }
    }

    close(fd);

    if (total >= 3 && ver_resp[2] == '#') {
        int major = static_cast<unsigned char>(ver_resp[0]);
        int minor = static_cast<unsigned char>(ver_resp[1]);
        return std::to_string(major) + "." + std::to_string(minor);
    }

    // Got echo but not version — still a NexStar device
    return "unknown";
#else
    (void)port_path;
    return "";
#endif
}

} // anonymous namespace

std::vector<CelestronPortInfo> enumerate_celestron_ports() {
    std::vector<CelestronPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        // Fallback: probe all /dev/ttyUSB* ports
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (std::filesystem::exists(port)) {
                ALPACA_LOG_INFO("Celestron", "Probing " + port + "...");
                std::string fw = probe_nexstar_port(port);
                if (!fw.empty()) {
                    ALPACA_LOG_INFO("Celestron", "Found NexStar mount on " + port +
                                    " (HC firmware " + fw + ")");
                    results.push_back({port, "", fw});
                }
            }
        }
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(serial_by_id)) {
        if (!entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();

        // Celestron NexStar mounts use Prolific PL2303 USB-serial adapters.
        // Also accept FTDI and CP210x which are used by some third-party cables.
        bool is_candidate = (name.find("Prolific") != std::string::npos) ||
                            (name.find("PL2303") != std::string::npos) ||
                            (name.find("067b") != std::string::npos) ||
                            (name.find("FTDI") != std::string::npos) ||
                            (name.find("CP210") != std::string::npos) ||
                            (name.find("USB_Serial") != std::string::npos) ||
                            (name.find("USB-Serial") != std::string::npos);
        if (!is_candidate) continue;

        std::string resolved = std::filesystem::canonical(entry.path()).string();
        ALPACA_LOG_INFO("Celestron", "Probing " + resolved + " (" + name + ")...");

        std::string fw = probe_nexstar_port(resolved);
        if (!fw.empty()) {
            ALPACA_LOG_INFO("Celestron", "Found NexStar mount on " + resolved +
                            " (HC firmware " + fw + ")");
            results.push_back({resolved, name, fw});
        }
    }
#endif

    return results;
}

namespace {

int parse_mode_byte(const std::string& response) {
    if (response.empty()) {
        return 0;
    }
    unsigned char ch = static_cast<unsigned char>(response[0]);
    if (ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9')) {
        return static_cast<int>(ch - static_cast<unsigned char>('0'));
    }
    return static_cast<int>(ch);
}

std::string build_hex(uint32_t value, std::size_t width) {
    std::ostringstream oss;
    oss << std::uppercase << std::setw(static_cast<int>(width)) << std::setfill('0') << std::hex << value;
    return oss.str();
}

std::string trailing_hex(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && !std::isxdigit(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && std::isxdigit(static_cast<unsigned char>(value[start - 1]))) {
        --start;
    }
    return value.substr(start, end - start);
}

std::string leading_hex(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && !std::isxdigit(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = start;
    while (end < value.size() && std::isxdigit(static_cast<unsigned char>(value[end]))) {
        ++end;
    }
    return value.substr(start, end - start);
}

std::optional<std::pair<uint32_t, uint32_t>> parse_axis_pair_response(const std::string& response,
                                                                       bool precise) {
    const std::size_t expected_digits = precise ? 6 : 4;
    auto comma = response.find(',');
    if (comma == std::string::npos) {
        return std::nullopt;
    }

    std::string left = trailing_hex(response.substr(0, comma));
    std::string right = leading_hex(response.substr(comma + 1));
    if (left.size() < expected_digits || right.size() < expected_digits) {
        return std::nullopt;
    }

    left.resize(expected_digits);
    right.resize(expected_digits);

    uint32_t first_raw = 0;
    uint32_t second_raw = 0;
    std::istringstream first_iss(left);
    std::istringstream second_iss(right);
    first_iss >> std::hex >> first_raw;
    second_iss >> std::hex >> second_raw;
    if (first_iss.fail() || second_iss.fail()) {
        return std::nullopt;
    }
    return std::make_pair(first_raw, second_raw);
}

// NexStar model ID to human-readable name mapping
std::string model_id_to_name(int model_id) {
    switch (model_id) {
        case 1:  return "NexStar GPS";
        case 3:  return "NexStar i-Series";
        case 4:  return "NexStar i-Series SE";
        case 5:  return "CGE";
        case 6:  return "Advanced GT";
        case 7:  return "SLT";
        case 9:  return "CPC";
        case 10: return "NexStar GT";
        case 11: return "NexStar 4/5 SE";
        case 12: return "NexStar 6/8 SE";
        case 13: return "CGE Pro";
        case 14: return "CGEM DX";
        case 15: return "LCM";
        case 16: return "Sky Prodigy";
        case 17: return "CPC Deluxe";
        case 18: return "GT 16";
        case 19: return "StarSeeker";
        case 20: return "Advanced VX";
        case 21: return "Cosmos";
        case 22: return "NexStar Evolution";
        case 23: return "CGX";
        case 24: return "CGX-L";
        case 25: return "Astro Fi";
        case 26: return "SkyWatcher";
        default: return "Mount";
    }
}

} // namespace

class CelestronProtocolWrapper::Impl {
public:
    Impl() : connected_(false), connection_type_(ConnectionType::Serial) {
        serial_fd_ = -1;
        socket_fd_ = -1;
    }

    ~Impl() {
        disconnect();
    }

    bool connect(const ConnectionInfo& info) {
        if (info.type == ConnectionType::Serial && info.port_path.empty()) {
            ALPACA_LOG_ERROR("Celestron", "Serial connection requested but port_path is empty");
            return false;
        }
        if (info.type == ConnectionType::Network && info.host.empty()) {
            ALPACA_LOG_ERROR("Celestron", "Network connection requested but host is empty");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_) {
            // The protocol wrapper is a per-vendor singleton: a second device
            // connecting through it would silently steal/tear down the first
            // device's connection. Refuse instead.
            throw AlpacaException(
                "Only one Celestron mount per bridge: the shared Celestron "
                "protocol wrapper is already connected");
        }

        connection_type_ = info.type;
        connection_info_ = info;

        bool success = false;
        if (info.type == ConnectionType::Serial) {
            success = connect_serial(info.port_path, info.baud_rate);
        } else {
            success = connect_network(info.host, info.tcp_port);
        }

        connected_ = success;
        return success;
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
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    std::string send_command(const std::string& command,
                             bool require_hash_terminator,
                             int timeout_ms_override,
                             int binary_bytes = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to Celestron mount");
        }

        if (!write_data(command)) {
            throw AlpacaException("Failed to send command to Celestron mount");
        }

        int timeout_ms = timeout_ms_override;
        if (timeout_ms <= 0) {
            timeout_ms = connection_info_.response_timeout_ms;
        }
        return read_response(require_hash_terminator, timeout_ms, binary_bytes);
    }

    void send_command_blind(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to Celestron mount");
        }
        if (!write_data(command)) {
            throw AlpacaException("Failed to send command to Celestron mount");
        }
        // Some firmware responds to binary motion commands while others do not.
        // Drain any immediate bytes so they don't corrupt the next request/response cycle.
        try {
            (void)read_response(false, 40);
        } catch (...) {
            // Ignore optional response drain errors for fire-and-forget commands.
        }
    }

    std::string get_handset_firmware_version() {
        // NexStar "V" command returns chr(major) & chr(minor) & "#"
        std::string response = send_command("V", true, 0, 2);
        if (response.size() < 2) {
            return "0.0";
        }
        int major = static_cast<unsigned char>(response[0]);
        int minor = static_cast<unsigned char>(response[1]);
        return std::to_string(major) + "." + std::to_string(minor);
    }

    int get_model_id() {
        // NexStar "m" command returns chr(model) & "#"
        std::string response = send_command("m", true, 0, 1);
        if (response.empty()) {
            return -1;
        }
        return static_cast<unsigned char>(response[0]);
    }

    std::string get_model_name() {
        int id = get_model_id();
        return model_id_to_name(id);
    }

    bool is_aligned() {
        // NexStar "J" command: chr(1) if aligned, chr(0) if not
        std::string response = send_command("J", true, 0, 1);
        if (response.empty()) {
            ALPACA_LOG_WARN("Celestron", "is_aligned (J): empty response → false");
            return false;
        }
        unsigned char ch = static_cast<unsigned char>(response[0]);
        // Aligned is chr(1) on most firmware, ASCII '1' on some. Anything else
        // (chr(0), ASCII '0', garbage) means NOT aligned — treating ASCII '0'
        // (0x30) as "non-zero → aligned" would bypass the slew-safety gate.
        bool aligned = (ch == 1) || (ch == '1');
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "is_aligned (J): response_byte=0x%02X ascii='%c' → %s",
                          ch, (ch >= 32 && ch < 127) ? ch : '?',
                          aligned ? "true" : "false");
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        return aligned;
    }

    bool is_goto_in_progress() {
        // NexStar "L" command: ASCII "0" or "1" & "#"
        std::string response = send_command("L", true, 0);
        if (response.empty()) {
            ALPACA_LOG_WARN("Celestron", "is_goto_in_progress (L): empty response → false");
            return false;
        }
        bool in_progress = response[0] == '1';
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "is_goto_in_progress (L): response_byte=0x%02X ascii='%c' → %s",
                          static_cast<unsigned char>(response[0]),
                          (response[0] >= 32 && response[0] < 127) ? response[0] : '?',
                          in_progress ? "true" : "false");
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        return in_progress;
    }

    void cancel_goto() {
        // NexStar "M" command: responds "#"
        ALPACA_LOG_WARN("Celestron", "cancel_goto (M) sent");
        (void)send_command("M", true, 0);
    }

    void seek_index(int axis) {
        // MC_SEEK_INDEX (command 0x19): tells motor controller to seek the index mark.
        // Pass-through: P chr(1) chr(dev) chr(0x19) chr(0) chr(0) chr(0) chr(0)
        // Supported on mounts with index marks (CGX, CGX-L, CGE Pro, NexStar+ HC).
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid Celestron axis index for seek_index");
        }
        const int dev = (axis == 0) ? 16 : 17;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x19));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        (void)send_command(cmd, true, 0);
    }

    bool is_at_index(int axis) {
        // MC_AT_INDEX (command 0x18): checks if axis is at the index position.
        // Pass-through: P chr(1) chr(dev) chr(0x18) chr(0) chr(0) chr(0) chr(1)
        // Response: chr(x) & "#" where x=0 means not at index, x!=0 means at index.
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid Celestron axis index for is_at_index");
        }
        const int dev = (axis == 0) ? 16 : 17;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x18));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            return false;
        }
        return static_cast<unsigned char>(response[0]) != 0;
    }

    void level_start(int axis) {
        // MC_LEVEL_START (command 0x0B): initiates movement to the hardware
        // home/index switch position.  Works on both RA and DEC axes for mounts
        // with physical home switches (CGX, CGX-L, CGE Pro).
        // Pass-through: P chr(1) chr(dev) chr(0x0B) chr(0) chr(0) chr(0) chr(0)
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid Celestron axis index for level_start");
        }
        const int dev = (axis == 0) ? 16 : 17;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x0B));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        // Must wait for '#' response before sending the next command,
        // otherwise leftover bytes corrupt the DEC command.
        (void)send_command(cmd, true, 0);
    }

    bool is_level_done(int axis) {
        // MC_LEVEL_DONE (command 0x12): checks if the axis has reached its
        // hardware home/index switch position.
        // Pass-through: P chr(1) chr(dev) chr(0x12) chr(0) chr(0) chr(0) chr(1)
        // Response: chr(0x00)="#" while moving, chr(0xFF)="#" when done.
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid Celestron axis index for is_level_done");
        }
        const int dev = (axis == 0) ? 16 : 17;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x12));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            return false;
        }
        return static_cast<unsigned char>(response[0]) != 0;
    }

    std::string get_device_firmware(int device_address) {
        // GET_VER (0xFE) pass-through to any device on the AUX bus.
        // P chr(1) chr(dev) chr(0xFE) chr(0) chr(0) chr(0) chr(2)
        // Response: chr(major) chr(minor) '#' — or timeout/garbage if device absent.
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(device_address));
        cmd.push_back(static_cast<char>(0xFE));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(2));
        try {
            std::string response = send_command(cmd, true, 1000, 2);
            if (response.size() >= 2) {
                int major = static_cast<unsigned char>(response[0]);
                int minor = static_cast<unsigned char>(response[1]);
                return std::to_string(major) + "." + std::to_string(minor);
            }
        } catch (...) {
            // Device not present or not responding.
        }
        return "";
    }

    std::vector<CelestronProtocolWrapper::BusDevice> probe_bus() {
        struct ProbeEntry {
            int address;
            const char* name;
        };
        static const ProbeEntry entries[] = {
            {0x10, "RA Motor Controller"},
            {0x11, "DEC Motor Controller"},
            {0x12, "Focuser"},
            {0x04, "Hand Controller"},
            {0x0D, "NexStar+ Hand Controller"},
            {0xB0, "GPS Module"},
            {0xB2, "Real-Time Clock"},
            {0xB5, "WiFi Module"},
            {0xB6, "Battery/Power"},
            {0x17, "Dew Heater"},
            {0xBF, "Light Controller"},
            {0x30, "RA Switch"},
            {0x31, "DEC Switch"},
            {0x32, "DEC Autoguider Port"},
        };

        std::vector<CelestronProtocolWrapper::BusDevice> devices;
        for (const auto& entry : entries) {
            std::string fw = get_device_firmware(entry.address);
            if (!fw.empty()) {
                CelestronProtocolWrapper::BusDevice dev;
                dev.address = entry.address;
                dev.name = entry.name;
                dev.firmware_version = fw;
                devices.push_back(std::move(dev));
            }
        }
        return devices;
    }

    void pulse_guide_axis(int axis, int velocity, int duration_cs) {
        // MTR_AUX_GUIDE (0x26): P chr(3) chr(dev) chr(0x26) chr(velocity) chr(duration) chr(0) chr(0)
        // velocity: signed char, percentage of sidereal rate (-100 to +100)
        // duration_cs: unsigned char, centiseconds (max 255 = 2550ms)
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for pulse_guide_axis");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        int clamped_vel = std::clamp(velocity, -100, 100);
        int clamped_dur = std::clamp(duration_cs, 0, 255);

        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(3));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x26));
        cmd.push_back(static_cast<char>(static_cast<signed char>(clamped_vel)));
        cmd.push_back(static_cast<char>(static_cast<unsigned char>(clamped_dur)));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        // Must wait for '#' response to avoid corrupting subsequent commands.
        (void)send_command(cmd, true, 0);
    }

    bool is_aux_guide_active(int axis) {
        // MTR_IS_AUX_GUIDE_ACTIVE (0x27): P chr(1) chr(dev) chr(0x27) chr(0) chr(0) chr(0) chr(1)
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for is_aux_guide_active");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x27));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            return false;
        }
        return static_cast<unsigned char>(response[0]) != 0;
    }

    char get_pier_side() {
        std::string response = send_command("p", true, 0, 1);
        if (response.empty()) {
            return '\0';
        }
        return response[0];
    }

    void set_autoguide_rate(int axis, double percent) {
        // MC_SET_AUTOGUIDE_RATE (0x46): P chr(2) chr(dev) chr(0x46) chr(val) chr(0) chr(0) chr(0)
        // val = desired_percent * 256 / 100
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for set_autoguide_rate");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        double clamped = std::clamp(percent, 0.0, 100.0);
        int val = static_cast<int>(std::round(clamped * 256.0 / 100.0));
        if (val > 255) val = 255;

        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "set_autoguide_rate axis=%d dev=0x%02X percent=%.4f val=%d "
                          "bytes=[50 02 %02X 46 %02X 00 00 00]",
                          axis, dev, clamped, val, dev, val);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }

        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(2));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x46));
        cmd.push_back(static_cast<char>(val));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        (void)send_command(cmd, true, 0);
    }

    double get_autoguide_rate(int axis) {
        // MC_GET_AUTOGUIDE_RATE (0x47): P chr(1) chr(dev) chr(0x47) chr(0) chr(0) chr(0) chr(1)
        // Response: chr(val) '#', percentage = 100.0 * val / 256.0
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for get_autoguide_rate");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x47));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            ALPACA_LOG_WARN("Celestron", "get_autoguide_rate: empty response → 0.0");
            return 0.0;
        }
        int val = static_cast<unsigned char>(response[0]);
        double percent = 100.0 * static_cast<double>(val) / 256.0;
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "get_autoguide_rate axis=%d dev=0x%02X response_byte=0x%02X → %.4f%%",
                          axis, dev, val, percent);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        return percent;
    }

    void pec_seek_index() {
        // MC_SEEK_INDEX on RA axis only — same as seek_index(0) but semantically for PEC.
        seek_index(0);
    }

    bool pec_at_index() {
        return is_at_index(0);
    }

    void pec_record_start() {
        // MC_PEC_RECORD_START (0x0C): P chr(1) chr(0x10) chr(0x0C) chr(0) chr(0) chr(0) chr(0)
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(0x10));
        cmd.push_back(static_cast<char>(0x0C));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        (void)send_command(cmd, true, 0);
    }

    void pec_record_stop() {
        // MC_PEC_RECORD_STOP (0x16): P chr(1) chr(0x10) chr(0x16) chr(0) chr(0) chr(0) chr(0)
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(0x10));
        cmd.push_back(static_cast<char>(0x16));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        (void)send_command(cmd, true, 0);
    }

    bool pec_record_done() {
        // MC_PEC_RECORD_DONE (0x15): P chr(1) chr(0x10) chr(0x15) chr(0) chr(0) chr(0) chr(1)
        // Response: chr(x) '#' — 0xFF = done, 0x00 = still recording.
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(0x10));
        cmd.push_back(static_cast<char>(0x15));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            return false;
        }
        return static_cast<unsigned char>(response[0]) != 0;
    }

    void pec_playback(bool enable) {
        // MC_PEC_PLAYBACK (0x0D): P chr(2) chr(0x10) chr(0x0D) chr(flag) chr(0) chr(0) chr(0)
        // flag: 0x01 = start playback, 0x00 = stop playback.
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(2));
        cmd.push_back(static_cast<char>(0x10));
        cmd.push_back(static_cast<char>(0x0D));
        cmd.push_back(static_cast<char>(enable ? 0x01 : 0x00));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        (void)send_command(cmd, true, 0);
    }

    int pec_get_bin() {
        // MTR_PECBIN (0x0E): P chr(1) chr(0x10) chr(0x0E) chr(0) chr(0) chr(0) chr(1)
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(0x10));
        cmd.push_back(static_cast<char>(0x0E));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            return -1;
        }
        return static_cast<unsigned char>(response[0]);
    }

    bool is_slew_done(int axis) {
        // MC_SLEW_DONE (0x13): P chr(1) chr(dev) chr(0x13) chr(0) chr(0) chr(0) chr(1)
        // Response: chr(0xFF) = done, chr(0x00) = still slewing.
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for is_slew_done");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x13));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(1));
        std::string response = send_command(cmd, true, 0, 1);
        if (response.empty()) {
            return true;
        }
        return static_cast<unsigned char>(response[0]) != 0;
    }

    uint32_t get_mc_position(int axis) {
        // MC_GET_POSITION (0x01): P chr(1) chr(dev) chr(0x01) chr(0) chr(0) chr(0) chr(3)
        // Response: 3 bytes = 24-bit encoder position as fraction of full rotation.
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for get_mc_position");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(1));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x01));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(0));
        cmd.push_back(static_cast<char>(3));
        std::string response = send_command(cmd, true, 0, 3);
        if (response.size() < 3) {
            throw AlpacaException("Invalid MC_GET_POSITION response");
        }
        uint32_t result = (static_cast<uint32_t>(static_cast<unsigned char>(response[0])) << 16) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(response[1])) << 8) |
                          static_cast<uint32_t>(static_cast<unsigned char>(response[2]));
        {
            char hex_buf[64];
            std::snprintf(hex_buf, sizeof(hex_buf),
                          "MC_GET_POSITION axis=%d raw=0x%06X bytes=[0x%02X 0x%02X 0x%02X]",
                          axis, result,
                          static_cast<unsigned char>(response[0]),
                          static_cast<unsigned char>(response[1]),
                          static_cast<unsigned char>(response[2]));
            ALPACA_LOG_WARN("Celestron", std::string(hex_buf));
        }
        return result;
    }

    void mc_goto_fast(int axis, uint32_t position) {
        // MC_GOTO_FAST (0x02): P chr(3) chr(dev) chr(0x02) posH posM posL chr(0)
        // Pass-through GOTO at max rate (rate 9) — bypasses HC.
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for mc_goto_fast");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(3));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x02));
        cmd.push_back(static_cast<char>((position >> 16) & 0xFF));
        cmd.push_back(static_cast<char>((position >> 8) & 0xFF));
        cmd.push_back(static_cast<char>(position & 0xFF));
        cmd.push_back(static_cast<char>(0));
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "mc_goto_fast axis=%d dev=0x%02X target=0x%06X",
                          axis, dev, position);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        (void)send_command(cmd, true, 0);
    }

    void mc_set_position(int axis, uint32_t position) {
        // MC_SET_POSITION (0x04): P chr(3) chr(dev) chr(0x04) posH posM posL chr(0)
        // Sets the position counter without moving — pass-through equivalent of Sync.
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis for mc_set_position");
        }
        const int dev = (axis == 0) ? 0x10 : 0x11;
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "mc_set_position axis=%d dev=0x%02X target=0x%06X "
                          "bytes=[50 03 %02X 04 %02X %02X %02X 00]",
                          axis, dev, position, dev,
                          static_cast<unsigned>((position >> 16) & 0xFF),
                          static_cast<unsigned>((position >> 8) & 0xFF),
                          static_cast<unsigned>(position & 0xFF));
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        std::string cmd;
        cmd.reserve(8);
        cmd.push_back('P');
        cmd.push_back(static_cast<char>(3));
        cmd.push_back(static_cast<char>(dev));
        cmd.push_back(static_cast<char>(0x04));
        cmd.push_back(static_cast<char>((position >> 16) & 0xFF));
        cmd.push_back(static_cast<char>((position >> 8) & 0xFF));
        cmd.push_back(static_cast<char>(position & 0xFF));
        cmd.push_back(static_cast<char>(0));
        (void)send_command(cmd, true, 0);
    }

    int get_tracking_mode() {
        // NexStar "t" command: chr(mode) & "#"
        // 0=Off, 1=Alt/Az, 2=EQ North, 3=EQ South
        std::string response = send_command("t", true, 0, 1);
        int mode = parse_mode_byte(response);
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "get_tracking_mode (t): response_byte=0x%02X → mode=%d",
                          response.empty() ? 0 : static_cast<unsigned char>(response[0]),
                          mode);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        return mode;
    }

    void set_tracking_mode(int mode) {
        // NexStar "T" & chr(mode) command
        if (mode < 0) {
            mode = 0;
        } else if (mode > 3) {
            mode = 3;
        }
        {
            const char* name = (mode == 0) ? "Off" :
                              (mode == 1) ? "AltAz" :
                              (mode == 2) ? "EQ-North" :
                              (mode == 3) ? "EQ-South" : "Unknown";
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "set_tracking_mode (T): mode=%d (%s) bytes=[54 %02X]",
                          mode, name, mode);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        std::string cmd = "T";
        cmd.push_back(static_cast<char>(mode));
        (void)send_command(cmd, true, 0);
    }

    void move_axis_fixed_rate(int axis, int rate) {
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid Celestron axis index");
        }

        // NexStar pass-through: device 16 = AZM/RA motor, device 17 = ALT/DEC motor
        const int dev = (axis == 0) ? 16 : 17;
        const int magnitude = std::clamp(std::abs(rate), 0, 9);

        auto send_fixed = [&](int direction_code, int slew_rate) {
            std::string cmd;
            cmd.reserve(8);
            cmd.push_back('P');
            cmd.push_back(static_cast<char>(2));
            cmd.push_back(static_cast<char>(dev));
            cmd.push_back(static_cast<char>(direction_code));
            cmd.push_back(static_cast<char>(slew_rate));
            cmd.push_back(static_cast<char>(0));
            cmd.push_back(static_cast<char>(0));
            cmd.push_back(static_cast<char>(0));
            (void)send_command(cmd, true, 0);
        };

        if (magnitude == 0) {
            // Issue both stop directions for robustness.
            send_fixed(36, 0);
            send_fixed(37, 0);
            return;
        }

        const int direction_code = (rate > 0) ? 36 : 37;
        send_fixed(direction_code, magnitude);
    }

    void move_axis_variable_rate(int axis, double rate_deg_per_sec) {
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid Celestron axis index");
        }

        const int dev = (axis == 0) ? 16 : 17;
        const double abs_deg_per_sec = std::abs(rate_deg_per_sec);

        auto send_variable = [&](int direction_code, uint16_t scaled_rate) {
            std::string cmd;
            cmd.reserve(8);
            cmd.push_back('P');
            cmd.push_back(static_cast<char>(3));
            cmd.push_back(static_cast<char>(dev));
            cmd.push_back(static_cast<char>(direction_code));
            cmd.push_back(static_cast<char>((scaled_rate >> 8) & 0xFF));
            cmd.push_back(static_cast<char>(scaled_rate & 0xFF));
            cmd.push_back(static_cast<char>(0));
            cmd.push_back(static_cast<char>(0));
            std::string resp;
            try {
                resp = send_command(cmd, true, 0);
            } catch (const std::exception& e) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "move_axis_variable_rate FRAME axis=%d dev=0x%02X dir=%d scaled=%u "
                              "bytes=[50 03 %02X %02X %02X %02X 00 00] send FAILED: %s",
                              axis, dev, direction_code, scaled_rate,
                              static_cast<unsigned>(dev),
                              static_cast<unsigned>(direction_code),
                              static_cast<unsigned>((scaled_rate >> 8) & 0xFF),
                              static_cast<unsigned>(scaled_rate & 0xFF),
                              e.what());
                ALPACA_LOG_WARN("Celestron", std::string(buf));
                throw;
            }
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "move_axis_variable_rate FRAME axis=%d dev=0x%02X dir=%d scaled=%u "
                          "bytes=[50 03 %02X %02X %02X %02X 00 00] respLen=%zu",
                          axis, dev, direction_code, scaled_rate,
                          static_cast<unsigned>(dev),
                          static_cast<unsigned>(direction_code),
                          static_cast<unsigned>((scaled_rate >> 8) & 0xFF),
                          static_cast<unsigned>(scaled_rate & 0xFF),
                          resp.size());
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        };

        if (abs_deg_per_sec < 1e-6) {
            send_variable(6, 0);
            send_variable(7, 0);
            // Also send fixed-mode stop to cover firmware differences.
            move_axis_fixed_rate(axis, 0);
            return;
        }

        // NexStar spec: multiply desired rate (arcsec/sec) by 4, split into high/low bytes
        const double rate_arcsec_per_sec = abs_deg_per_sec * 3600.0;
        double scaled = std::round(rate_arcsec_per_sec * 4.0);
        if (scaled < 1.0) {
            scaled = 1.0;
        }
        if (scaled > 65535.0) {
            scaled = 65535.0;
        }
        const auto scaled_rate = static_cast<uint16_t>(scaled);
        const int direction_code = (rate_deg_per_sec > 0.0) ? 6 : 7;
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "move_axis_variable_rate REQUEST axis=%d rate_deg_per_sec=%.9f "
                          "arcsec_per_sec=%.4f scaled=%u dir=%d",
                          axis, rate_deg_per_sec, rate_arcsec_per_sec,
                          scaled_rate, direction_code);
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        send_variable(direction_code, scaled_rate);
    }

    std::pair<uint32_t, uint32_t> get_ra_dec_raw(bool precise) {
        // NexStar: "E" for standard, "e" for precise
        const char* cmd = precise ? "e" : "E";
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string response = send_command(cmd, true, 0);
            if (auto parsed = parse_axis_pair_response(response, precise); parsed.has_value()) {
                char diag[128];
                std::snprintf(diag, sizeof(diag),
                              "get_ra_dec_raw('%s') response=\"%s\" → RA=0x%06X DEC=0x%06X",
                              cmd, response.c_str(), parsed->first, parsed->second);
                ALPACA_LOG_WARN("Celestron", std::string(diag));
                return parsed.value();
            }
            ALPACA_LOG_WARN("Celestron", "get_ra_dec_raw('" + std::string(cmd) +
                           "') parse failed, response=\"" + response + "\" len=" +
                           std::to_string(response.size()) + " attempt=" + std::to_string(attempt));
        }
        throw AlpacaException("Invalid Celestron RA/Dec response");
    }

    std::pair<uint32_t, uint32_t> get_alt_az_raw(bool precise) {
        // NexStar: "Z" for standard, "z" for precise
        const char* cmd = precise ? "z" : "Z";
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string response = send_command(cmd, true, 0);
            if (auto parsed = parse_axis_pair_response(response, precise); parsed.has_value()) {
                return parsed.value();
            }
        }
        throw AlpacaException("Invalid Celestron Alt/Az response");
    }

    void goto_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
        // NexStar: "R" for standard, "r" for precise GOTO RA/DEC
        std::string ra_hex = build_hex(ra_raw, precise ? 6 : 4);
        std::string dec_hex = build_hex(dec_raw, precise ? 6 : 4);
        if (precise) {
            ra_hex += "00";
            dec_hex += "00";
        }
        std::string cmd = std::string(precise ? "r" : "R") + ra_hex + "," + dec_hex;
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "goto_ra_dec_raw target ra_raw=0x%06X dec_raw=0x%06X precise=%d cmd=\"%s\"",
                          ra_raw, dec_raw, precise ? 1 : 0, cmd.c_str());
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        (void)send_command(cmd, true, 0);
    }

    void goto_alt_az_raw(uint32_t az_raw, uint32_t alt_raw, bool precise) {
        // NexStar: "B" for standard, "b" for precise GOTO AZM-ALT
        std::string az_hex = build_hex(az_raw, precise ? 6 : 4);
        std::string alt_hex = build_hex(alt_raw, precise ? 6 : 4);
        if (precise) {
            az_hex += "00";
            alt_hex += "00";
        }
        std::string cmd = std::string(precise ? "b" : "B") + az_hex + "," + alt_hex;
        (void)send_command(cmd, true, 0);
    }

    void sync_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
        // NexStar: "S" for standard, "s" for precise Sync RA/DEC (v4.10+)
        std::string ra_hex = build_hex(ra_raw, precise ? 6 : 4);
        std::string dec_hex = build_hex(dec_raw, precise ? 6 : 4);
        if (precise) {
            ra_hex += "00";
            dec_hex += "00";
        }
        std::string cmd = std::string(precise ? "s" : "S") + ra_hex + "," + dec_hex;
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "sync_ra_dec_raw ra_raw=0x%06X dec_raw=0x%06X precise=%d cmd=\"%s\"",
                          ra_raw, dec_raw, precise ? 1 : 0, cmd.c_str());
            ALPACA_LOG_WARN("Celestron", std::string(buf));
        }
        (void)send_command(cmd, true, 0);
    }

    LocationInfo get_location() {
        // NexStar "w" (lowercase) to get location: 8 binary bytes + "#"
        // Format: lat_deg, lat_min, lat_sec, lat_sign(0=N,1=S),
        //         lon_deg, lon_min, lon_sec, lon_sign(0=E,1=W)
        std::string response = send_command("w", true, 0, 8);
        if (response.size() < 8) {
            throw AlpacaException("Invalid Celestron location response");
        }
        auto to_u8 = [&](std::size_t index) {
            return static_cast<unsigned char>(response[index]);
        };
        int lat_deg = to_u8(0);
        int lat_min = to_u8(1);
        int lat_sec = to_u8(2);
        int lat_sign = to_u8(3);
        int lon_deg = to_u8(4);
        int lon_min = to_u8(5);
        int lon_sec = to_u8(6);
        int lon_sign = to_u8(7);

        double lat = static_cast<double>(lat_deg) + lat_min / 60.0 + lat_sec / 3600.0;
        if (lat_sign != 0) {
            lat = -lat;
        }
        // NexStar convention: 0=East, 1=West. Alpaca convention: East is positive.
        double lon = static_cast<double>(lon_deg) + lon_min / 60.0 + lon_sec / 3600.0;
        if (lon_sign != 0) {
            lon = -lon;
        }
        LocationInfo info;
        info.latitude_degrees = lat;
        info.longitude_degrees = lon;
        return info;
    }

    void set_location(const LocationInfo& info) {
        // NexStar "W" (uppercase) to set location
        double lat = info.latitude_degrees;
        double lon = info.longitude_degrees;

        int lat_sign = lat < 0.0 ? 1 : 0;
        int lon_sign = lon < 0.0 ? 1 : 0;
        lat = std::abs(lat);
        lon = std::abs(lon);

        int lat_deg = static_cast<int>(lat);
        int lat_min = static_cast<int>((lat - lat_deg) * 60.0);
        int lat_sec = static_cast<int>((lat - lat_deg - lat_min / 60.0) * 3600.0);
        int lon_deg = static_cast<int>(lon);
        int lon_min = static_cast<int>((lon - lon_deg) * 60.0);
        int lon_sec = static_cast<int>((lon - lon_deg - lon_min / 60.0) * 3600.0);

        std::string cmd;
        cmd.reserve(1 + 8);
        cmd.push_back('W');
        cmd.push_back(static_cast<char>(lat_deg));
        cmd.push_back(static_cast<char>(lat_min));
        cmd.push_back(static_cast<char>(lat_sec));
        cmd.push_back(static_cast<char>(lat_sign));
        cmd.push_back(static_cast<char>(lon_deg));
        cmd.push_back(static_cast<char>(lon_min));
        cmd.push_back(static_cast<char>(lon_sec));
        cmd.push_back(static_cast<char>(lon_sign));
        (void)send_command(cmd, true, 0);
    }

    TimeInfo get_time() {
        // NexStar "h" to get time: 8 binary bytes + "#"
        std::string response = send_command("h", true, 0, 8);
        if (response.size() < 8) {
            throw AlpacaException("Invalid Celestron time response");
        }
        auto to_u8 = [&](std::size_t index) {
            return static_cast<unsigned char>(response[index]);
        };
        TimeInfo info;
        info.hour = to_u8(0);
        info.minute = to_u8(1);
        info.second = to_u8(2);
        info.month = to_u8(3);
        info.day = to_u8(4);
        info.year = to_u8(5);
        int tz_raw = to_u8(6);
        info.dst_enabled = to_u8(7) != 0;
        // NexStar spec: if zone is negative, use 256-zone
        if (tz_raw > 127) {
            tz_raw = tz_raw - 256;
        }
        info.timezone_offset_minutes = tz_raw * 60;
        return info;
    }

    void set_time(const TimeInfo& info) {
        // NexStar "H" to set time
        int tz_hours = info.timezone_offset_minutes / 60;
        int tz_byte = tz_hours;
        if (tz_byte < 0) {
            tz_byte = 256 + tz_byte;
        }

        std::string cmd;
        cmd.reserve(1 + 8);
        cmd.push_back('H');
        cmd.push_back(static_cast<char>(info.hour));
        cmd.push_back(static_cast<char>(info.minute));
        cmd.push_back(static_cast<char>(info.second));
        cmd.push_back(static_cast<char>(info.month));
        cmd.push_back(static_cast<char>(info.day));
        cmd.push_back(static_cast<char>(info.year));
        cmd.push_back(static_cast<char>(tz_byte));
        cmd.push_back(static_cast<char>(info.dst_enabled ? 1 : 0));
        (void)send_command(cmd, true, 0);
    }

private:
    bool connect_serial(const std::string& port_path, int baud_rate) {
        serial_fd_ = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            return false;
        }
        termios tty{};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        // NexStar spec: 9600 bits/sec, no parity, one stop bit
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

        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= CREAD | CLOCAL;
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_oflag &= ~OPOST;
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;

        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        // The fd was opened O_NONBLOCK; clear it so reads honour VMIN/VTIME and
        // write_all()/read don't spuriously fail with EAGAIN (matches the probe
        // path and the other serial wrappers).
        if (!util::clear_nonblocking(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        return true;
    }

    bool connect_network(const std::string& host, int port) {
        constexpr int kConnectTimeoutMs = 7000;
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            // getaddrinfo is the reentrant replacement for gethostbyname.
            addrinfo hints{};
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
        }
        // Non-blocking connect with a bounded poll timeout: this runs under the
        // driver mutex, and a bare blocking connect() to an unreachable host
        // would stall every GET (and disconnect) for the full OS TCP timeout
        // (~127 s). Same pattern as the iOptron network prober.
        if (!util::set_nonblocking(socket_fd_)) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        int rc = ::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
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
    }

    void disconnect_serial() {
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
    }

    void disconnect_network() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    bool write_data(const std::string& data) {
        if (connection_type_ == ConnectionType::Serial) {
            return write_serial(data);
        }
        return write_network(data);
    }

    bool write_serial(const std::string& data) { return util::write_all(serial_fd_, data.c_str(), data.length()); }

    bool write_network(const std::string& data) {
        // Loop over short sends; MSG_NOSIGNAL so a dropped peer can't SIGPIPE
        // the server.
        return util::send_all(socket_fd_, data.c_str(), data.length(), MSG_NOSIGNAL);
    }

    std::string read_response(bool require_hash_terminator, int timeout_ms,
                              int binary_bytes = 0) {
        std::string response;
        auto start = std::chrono::steady_clock::now();
        auto last_char_time = start;
        const int idle_break_ms = 100;
        // When binary_bytes > 0, read exactly that many raw bytes first
        // (no CR/LF filtering, no '#' detection) then drain the '#' terminator.
        // This prevents binary values like 0x23 ('#') or 0x0A/0x0D from being
        // misinterpreted as protocol framing characters.
        int remaining_binary = binary_bytes;

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
                if (remaining_binary > 0) {
                    // Binary phase: accept every byte as data
                    response.push_back(ch);
                    --remaining_binary;
                    if (remaining_binary == 0 && require_hash_terminator) {
                        // All binary bytes received; now drain the '#' terminator
                        drain_hash_terminator(timeout_ms, start);
                    }
                    if (remaining_binary == 0) {
                        break;
                    }
                    continue;
                }
                if (ch == '#') {
                    break;
                }
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                response.push_back(ch);
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            if (elapsed.count() > timeout_ms) {
                if (!require_hash_terminator) {
                    return response;
                }
                throw AlpacaException("Timeout waiting for Celestron response");
            }
            if (!require_hash_terminator && !got_char && !response.empty()) {
                auto idle_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_char_time);
                if (idle_elapsed.count() > idle_break_ms) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return response;
    }

    void drain_hash_terminator(int timeout_ms,
                               std::chrono::steady_clock::time_point start) {
        // Read and discard the '#' terminator after binary payload.
        while (true) {
            char ch;
            bool got_char = false;
            if (connection_type_ == ConnectionType::Serial) {
                got_char = read_serial_char(ch);
            } else {
                got_char = read_network_char(ch);
            }
            if (got_char) {
                // Accept '#' or anything else — just drain one byte.
                return;
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            if (elapsed.count() > timeout_ms) {
                // Terminator didn't arrive; the payload is still valid.
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool read_serial_char(char& ch) {
        ssize_t bytes_read = read(serial_fd_, &ch, 1);
        return bytes_read == 1;
    }

    bool read_network_char(char& ch) {
        ssize_t bytes_received = recv(socket_fd_, &ch, 1, 0);
        return bytes_received == 1;
    }

    void configure_network_timeouts() {
        constexpr int kSocketTimeoutMs = 200;
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = kSocketTimeoutMs * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

private:
    mutable std::mutex mutex_;
    bool connected_;
    ConnectionType connection_type_;
    ConnectionInfo connection_info_;

    int serial_fd_;
    int socket_fd_;
};

CelestronProtocolWrapper::CelestronProtocolWrapper() : pimpl_(std::make_unique<Impl>()) {}
CelestronProtocolWrapper::~CelestronProtocolWrapper() = default;

CelestronProtocolWrapper& CelestronProtocolWrapper::instance() {
    static CelestronProtocolWrapper wrapper;
    return wrapper;
}

bool CelestronProtocolWrapper::connect(const ConnectionInfo& info) {
    return pimpl_->connect(info);
}

void CelestronProtocolWrapper::disconnect() {
    pimpl_->disconnect();
}

bool CelestronProtocolWrapper::is_connected() const {
    return pimpl_->is_connected();
}

std::string CelestronProtocolWrapper::send_command(const std::string& command,
                                                   bool require_hash_terminator,
                                                   int timeout_ms_override) {
    return pimpl_->send_command(command, require_hash_terminator, timeout_ms_override);
}

std::string CelestronProtocolWrapper::send_raw_command(const std::string& bytes,
                                                       bool require_hash_terminator,
                                                       int timeout_ms_override) {
    return pimpl_->send_command(bytes, require_hash_terminator, timeout_ms_override);
}

void CelestronProtocolWrapper::send_command_blind(const std::string& command) {
    pimpl_->send_command_blind(command);
}

std::string CelestronProtocolWrapper::get_handset_firmware_version() {
    return pimpl_->get_handset_firmware_version();
}

int CelestronProtocolWrapper::get_model_id() {
    return pimpl_->get_model_id();
}

std::string CelestronProtocolWrapper::get_model_name() {
    return pimpl_->get_model_name();
}

bool CelestronProtocolWrapper::is_aligned() {
    return pimpl_->is_aligned();
}

bool CelestronProtocolWrapper::is_goto_in_progress() {
    return pimpl_->is_goto_in_progress();
}

void CelestronProtocolWrapper::cancel_goto() {
    pimpl_->cancel_goto();
}

void CelestronProtocolWrapper::seek_index(int axis) {
    pimpl_->seek_index(axis);
}

bool CelestronProtocolWrapper::is_at_index(int axis) {
    return pimpl_->is_at_index(axis);
}

void CelestronProtocolWrapper::level_start(int axis) {
    pimpl_->level_start(axis);
}

bool CelestronProtocolWrapper::is_level_done(int axis) {
    return pimpl_->is_level_done(axis);
}

std::string CelestronProtocolWrapper::get_device_firmware(int device_address) {
    return pimpl_->get_device_firmware(device_address);
}

std::vector<CelestronProtocolWrapper::BusDevice> CelestronProtocolWrapper::probe_bus() {
    return pimpl_->probe_bus();
}

void CelestronProtocolWrapper::pulse_guide_axis(int axis, int velocity, int duration_cs) {
    pimpl_->pulse_guide_axis(axis, velocity, duration_cs);
}

bool CelestronProtocolWrapper::is_aux_guide_active(int axis) {
    return pimpl_->is_aux_guide_active(axis);
}

char CelestronProtocolWrapper::get_pier_side() {
    return pimpl_->get_pier_side();
}

void CelestronProtocolWrapper::set_autoguide_rate(int axis, double percent) {
    pimpl_->set_autoguide_rate(axis, percent);
}

double CelestronProtocolWrapper::get_autoguide_rate(int axis) {
    return pimpl_->get_autoguide_rate(axis);
}

void CelestronProtocolWrapper::pec_seek_index() {
    pimpl_->pec_seek_index();
}

bool CelestronProtocolWrapper::pec_at_index() {
    return pimpl_->pec_at_index();
}

void CelestronProtocolWrapper::pec_record_start() {
    pimpl_->pec_record_start();
}

void CelestronProtocolWrapper::pec_record_stop() {
    pimpl_->pec_record_stop();
}

bool CelestronProtocolWrapper::pec_record_done() {
    return pimpl_->pec_record_done();
}

void CelestronProtocolWrapper::pec_playback(bool enable) {
    pimpl_->pec_playback(enable);
}

int CelestronProtocolWrapper::pec_get_bin() {
    return pimpl_->pec_get_bin();
}

bool CelestronProtocolWrapper::is_slew_done(int axis) {
    return pimpl_->is_slew_done(axis);
}

uint32_t CelestronProtocolWrapper::get_mc_position(int axis) {
    return pimpl_->get_mc_position(axis);
}

void CelestronProtocolWrapper::mc_goto_fast(int axis, uint32_t position) {
    pimpl_->mc_goto_fast(axis, position);
}

void CelestronProtocolWrapper::mc_set_position(int axis, uint32_t position) {
    pimpl_->mc_set_position(axis, position);
}

int CelestronProtocolWrapper::get_tracking_mode() {
    return pimpl_->get_tracking_mode();
}

void CelestronProtocolWrapper::set_tracking_mode(int mode) {
    pimpl_->set_tracking_mode(mode);
}

void CelestronProtocolWrapper::move_axis_fixed_rate(int axis, int rate) {
    pimpl_->move_axis_fixed_rate(axis, rate);
}

void CelestronProtocolWrapper::move_axis_variable_rate(int axis, double rate_deg_per_sec) {
    pimpl_->move_axis_variable_rate(axis, rate_deg_per_sec);
}

std::pair<uint32_t, uint32_t> CelestronProtocolWrapper::get_ra_dec_raw(bool precise) {
    return pimpl_->get_ra_dec_raw(precise);
}

std::pair<uint32_t, uint32_t> CelestronProtocolWrapper::get_alt_az_raw(bool precise) {
    return pimpl_->get_alt_az_raw(precise);
}

void CelestronProtocolWrapper::goto_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
    pimpl_->goto_ra_dec_raw(ra_raw, dec_raw, precise);
}

void CelestronProtocolWrapper::goto_alt_az_raw(uint32_t az_raw, uint32_t alt_raw, bool precise) {
    pimpl_->goto_alt_az_raw(az_raw, alt_raw, precise);
}

void CelestronProtocolWrapper::sync_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
    pimpl_->sync_ra_dec_raw(ra_raw, dec_raw, precise);
}

LocationInfo CelestronProtocolWrapper::get_location() {
    return pimpl_->get_location();
}

void CelestronProtocolWrapper::set_location(const LocationInfo& info) {
    pimpl_->set_location(info);
}

TimeInfo CelestronProtocolWrapper::get_time() {
    return pimpl_->get_time();
}

void CelestronProtocolWrapper::set_time(const TimeInfo& info) {
    pimpl_->set_time(info);
}

} // namespace alpacacore::vendor::celestron

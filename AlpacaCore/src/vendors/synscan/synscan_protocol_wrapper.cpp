// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
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

namespace alpacacore::vendor::synscan {

namespace {

// Probe a serial port for a SynScan hand controller using the echo command (Kx).
// Returns HC firmware version string (e.g. "04.42.00") on success, empty on failure.
// SynScan V command returns 6 hex-ASCII digits (e.g. "042A00#"), unlike NexStar binary format.
std::string probe_synscan_port(const std::string& port_path) {
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

    const char echo_cmd[] = {'K', 0x42};
    if (!util::write_all(fd, echo_cmd, 2)) {
        close(fd);
        return "";
    }

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

    // Echo confirmed — query firmware version with "V" command.
    // SynScan returns 6 hex-ASCII digits + '#' (e.g. "042A00#" for 4.42.00).
    tcflush(fd, TCIOFLUSH);
    const char ver_cmd[] = {'V'};
    if (!util::write_all(fd, ver_cmd, 1)) {
        close(fd);
        return "";
    }

    char ver_resp[8] = {};
    total = 0;
    start = std::chrono::steady_clock::now();
    while (total < 7) {
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

    // Distinguish SynScan from NexStar by response length.
    // SynScan V: 6 hex-ASCII chars + '#' (7 bytes total).
    // NexStar V: 2 binary bytes + '#' (3 bytes total).
    bool is_synscan = (total == 7 && ver_resp[6] == '#');

    close(fd);

    if (is_synscan) {
        // Parse "XXYYZZ" as version XX.YY.ZZ (each pair is hex)
        std::string hex_str(ver_resp, 6);
        bool all_hex = true;
        for (char c : hex_str) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                all_hex = false;
                break;
            }
        }
        if (!all_hex) {
            return "unknown";
        }

        auto parse_pair = [](const char* p) -> int {
            int val = 0;
            std::istringstream iss(std::string(p, 2));
            iss >> std::hex >> val;
            return val;
        };
        int major = parse_pair(ver_resp);
        int minor = parse_pair(ver_resp + 2);
        int patch = parse_pair(ver_resp + 4);

        std::ostringstream oss;
        oss << std::setw(2) << std::setfill('0') << major << "."
            << std::setw(2) << std::setfill('0') << minor << "."
            << std::setw(2) << std::setfill('0') << patch;
        return oss.str();
    }

    if (total >= 2) {
        return "unknown";
    }

    return "";
#else
    (void)port_path;
    return "";
#endif
}

} // anonymous namespace

std::vector<SynScanPortInfo> enumerate_synscan_ports() {
    std::vector<SynScanPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (std::filesystem::exists(port)) {
                ALPACA_LOG_INFO("SynScan", "Probing " + port + "...");
                std::string fw = probe_synscan_port(port);
                if (!fw.empty()) {
                    ALPACA_LOG_INFO("SynScan", "Found SynScan mount on " + port +
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

        bool is_candidate = (name.find("Prolific") != std::string::npos) ||
                            (name.find("PL2303") != std::string::npos) ||
                            (name.find("067b") != std::string::npos) ||
                            (name.find("FTDI") != std::string::npos) ||
                            (name.find("CP210") != std::string::npos) ||
                            (name.find("USB_Serial") != std::string::npos) ||
                            (name.find("USB-Serial") != std::string::npos);
        if (!is_candidate) continue;

        std::string resolved = std::filesystem::canonical(entry.path()).string();
        ALPACA_LOG_INFO("SynScan", "Probing " + resolved + " (" + name + ")...");

        std::string fw = probe_synscan_port(resolved);
        if (!fw.empty()) {
            ALPACA_LOG_INFO("SynScan", "Found SynScan mount on " + resolved +
                            " (HC firmware " + fw + ")");
            results.push_back({resolved, name, fw});
        }
    }
#endif

    return results;
}

namespace {

int parse_hex_pair(std::string_view value, std::size_t offset) {
    if (offset + 2 > value.size()) {
        return 0;
    }
    int result = 0;
    std::istringstream iss(std::string(value.substr(offset, 2)));
    iss >> std::hex >> result;
    return result;
}

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

    left = left.substr(left.size() - expected_digits);
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

} // namespace

class SynScanProtocolWrapper::Impl {
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
            ALPACA_LOG_ERROR("SynScan", "Serial connection requested but port_path is empty");
            return false;
        }
        if (info.type == ConnectionType::Network && info.host.empty()) {
            ALPACA_LOG_ERROR("SynScan", "Network connection requested but host is empty");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_) {
            disconnect_locked();  // already holding mutex_ — must not re-lock
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
                             int timeout_ms_override) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }

        if (!write_data(command)) {
            throw AlpacaException("Failed to send command to mount");
        }

        int timeout_ms = timeout_ms_override;
        if (timeout_ms <= 0) {
            timeout_ms = connection_info_.response_timeout_ms;
        }
        return read_response(require_hash_terminator, timeout_ms);
    }

    void send_command_blind(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }
        if (!write_data(command)) {
            throw AlpacaException("Failed to send command to mount");
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
        std::string response = send_command("V", true, 0);
        if (response.size() < 6) {
            return "0.0.0";
        }
        int major = parse_hex_pair(response, 0);
        int minor = parse_hex_pair(response, 2);
        int patch = parse_hex_pair(response, 4);
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    int get_model_id() {
        std::string response = send_command("m", true, 0);
        if (response.empty()) {
            return -1;
        }
        return static_cast<unsigned char>(response[0]);
    }

    bool is_aligned() {
        std::string response = send_command("J", true, 0);
        if (response.empty()) {
            return false;
        }
        unsigned char ch = static_cast<unsigned char>(response[0]);
        if (ch == '1') {
            return true;
        }
        return ch != 0;
    }

    bool is_goto_in_progress() {
        std::string response = send_command("L", true, 0);
        if (response.empty()) {
            return false;
        }
        return response[0] == '1';
    }

    void cancel_goto() {
        // Some handsets do not acknowledge cancel reliably; fire-and-forget avoids timeout stalls.
        send_command_blind("M");
    }

    char get_pointing_state() {
        std::string response = send_command("p", true, 0);
        if (response.empty()) {
            return 'U';
        }
        return response[0];
    }

    int get_tracking_mode() {
        std::string response = send_command("t", true, 0);
        return parse_mode_byte(response);
    }

    void set_tracking_mode(int mode) {
        if (mode < 0) {
            mode = 0;
        } else if (mode > 3) {
            mode = 3;
        }
        std::string cmd = "T";
        cmd.push_back(static_cast<char>(mode));
        (void)send_command(cmd, true, 0);
    }

    void move_axis_fixed_rate(int axis, int rate) {
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid SynScan axis index");
        }

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
            // Some handsets/firmware do not acknowledge this low-level slew command.
            // Fire-and-forget avoids false timeout failures while keeping motion responsive.
            send_command_blind(cmd);
        };

        if (magnitude == 0) {
            // Issue both stop directions for robustness across handset/firmware variants.
            send_fixed(36, 0);
            send_fixed(37, 0);
            return;
        }

        const int direction_code = (rate > 0) ? 36 : 37;
        send_fixed(direction_code, magnitude);
    }

    void move_axis_variable_rate(int axis, double rate_deg_per_sec) {
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid SynScan axis index");
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
            send_command_blind(cmd);
        };

        if (abs_deg_per_sec < 1e-6) {
            send_variable(6, 0);
            send_variable(7, 0);
            // Also send fixed-mode stop to cover handset firmware differences.
            move_axis_fixed_rate(axis, 0);
            return;
        }

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
        send_variable(direction_code, scaled_rate);
    }

    std::pair<uint32_t, uint32_t> get_ra_dec_raw(bool precise) {
        const char* cmd = precise ? "e" : "E";
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string response = send_command(cmd, true, 0);
            if (auto parsed = parse_axis_pair_response(response, precise); parsed.has_value()) {
                return parsed.value();
            }
        }
        throw AlpacaException("Invalid SynScan RA/Dec response");
    }

    std::pair<uint32_t, uint32_t> get_alt_az_raw(bool precise) {
        const char* cmd = precise ? "z" : "Z";
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string response = send_command(cmd, true, 0);
            if (auto parsed = parse_axis_pair_response(response, precise); parsed.has_value()) {
                return parsed.value();
            }
        }
        throw AlpacaException("Invalid SynScan Alt/Az response");
    }

    void goto_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
        std::string ra_hex = build_hex(ra_raw, precise ? 6 : 4);
        std::string dec_hex = build_hex(dec_raw, precise ? 6 : 4);
        if (precise) {
            ra_hex += "00";
            dec_hex += "00";
        }
        std::string cmd = std::string(precise ? "r" : "R") + ra_hex + "," + dec_hex;
        (void)send_command(cmd, true, 0);
    }

    void goto_alt_az_raw(uint32_t az_raw, uint32_t alt_raw, bool precise) {
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
        std::string ra_hex = build_hex(ra_raw, precise ? 6 : 4);
        std::string dec_hex = build_hex(dec_raw, precise ? 6 : 4);
        if (precise) {
            ra_hex += "00";
            dec_hex += "00";
        }
        std::string cmd = std::string(precise ? "s" : "S") + ra_hex + "," + dec_hex;
        (void)send_command(cmd, true, 0);
    }

    LocationInfo get_location() {
        std::string response = send_command("w", true, 0);
        if (response.size() < 8) {
            throw AlpacaException("Invalid SynScan location response");
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
        std::string response = send_command("h", true, 0);
        if (response.size() < 8) {
            throw AlpacaException("Invalid SynScan time response");
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
        if (tz_raw > 127) {
            tz_raw = tz_raw - 256;
        }
        info.timezone_offset_minutes = tz_raw * 60;
        return info;
    }

    void set_time(const TimeInfo& info) {
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
#ifdef _WIN32
        std::string full_port = port_path;
        if (full_port.rfind("\\\\.\\", 0) != 0) {
            full_port = "\\\\.\\" + full_port;
        }
        serial_handle_ = CreateFileA(
            full_port.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (serial_handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        dcb.BaudRate = static_cast<DWORD>(baud_rate);
        dcb.ByteSize = 8;
        dcb.StopBits = ONESTOPBIT;
        dcb.Parity = NOPARITY;
        if (!SetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(serial_handle_, &timeouts);
        return true;
#else
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
#endif
    }

    bool connect_network(const std::string& host, int port) {
#ifdef _WIN32
        socket_handle_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_handle_ == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        addr.sin_addr.s_addr = inet_addr(host.c_str());
        if (addr.sin_addr.s_addr == INADDR_NONE) {
            hostent* host_entry = gethostbyname(host.c_str());
            if (!host_entry) {
                closesocket(socket_handle_);
                socket_handle_ = INVALID_SOCKET;
                return false;
            }
            addr.sin_addr = *reinterpret_cast<in_addr*>(host_entry->h_addr);
        }
        if (::connect(socket_handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
            return false;
        }
        configure_network_timeouts();
        return true;
#else
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            hostent* host_entry = gethostbyname(host.c_str());
            if (!host_entry) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = *reinterpret_cast<in_addr*>(host_entry->h_addr);
        }
        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        configure_network_timeouts();
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
        }
        return write_network(data);
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
        // Loop over short sends so partial bytes can't corrupt the next
        // command's framing (mirrors the POSIX send_all path below).
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
        // Loop over short sends; MSG_NOSIGNAL so a dropped peer can't SIGPIPE
        // the server.
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
                throw AlpacaException("Timeout waiting for SynScan response");
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
#ifdef _WIN32
        DWORD timeout = kSocketTimeoutMs;
        setsockopt(socket_handle_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_handle_, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = kSocketTimeoutMs * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
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

SynScanProtocolWrapper::SynScanProtocolWrapper() : pimpl_(std::make_unique<Impl>()) {}
SynScanProtocolWrapper::~SynScanProtocolWrapper() = default;

SynScanProtocolWrapper& SynScanProtocolWrapper::instance() {
    static SynScanProtocolWrapper wrapper;
    return wrapper;
}

bool SynScanProtocolWrapper::connect(const ConnectionInfo& info) {
    return pimpl_->connect(info);
}

void SynScanProtocolWrapper::disconnect() {
    pimpl_->disconnect();
}

bool SynScanProtocolWrapper::is_connected() const {
    return pimpl_->is_connected();
}

std::string SynScanProtocolWrapper::send_command(const std::string& command,
                                                 bool require_hash_terminator,
                                                 int timeout_ms_override) {
    return pimpl_->send_command(command, require_hash_terminator, timeout_ms_override);
}

std::string SynScanProtocolWrapper::send_raw_command(const std::string& bytes,
                                                     bool require_hash_terminator,
                                                     int timeout_ms_override) {
    return pimpl_->send_command(bytes, require_hash_terminator, timeout_ms_override);
}

void SynScanProtocolWrapper::send_command_blind(const std::string& command) {
    pimpl_->send_command_blind(command);
}

std::string SynScanProtocolWrapper::get_handset_firmware_version() {
    return pimpl_->get_handset_firmware_version();
}

int SynScanProtocolWrapper::get_model_id() {
    return pimpl_->get_model_id();
}

bool SynScanProtocolWrapper::is_aligned() {
    return pimpl_->is_aligned();
}

bool SynScanProtocolWrapper::is_goto_in_progress() {
    return pimpl_->is_goto_in_progress();
}

void SynScanProtocolWrapper::cancel_goto() {
    pimpl_->cancel_goto();
}

char SynScanProtocolWrapper::get_pointing_state() {
    return pimpl_->get_pointing_state();
}

int SynScanProtocolWrapper::get_tracking_mode() {
    return pimpl_->get_tracking_mode();
}

void SynScanProtocolWrapper::set_tracking_mode(int mode) {
    pimpl_->set_tracking_mode(mode);
}

void SynScanProtocolWrapper::move_axis_fixed_rate(int axis, int rate) {
    pimpl_->move_axis_fixed_rate(axis, rate);
}

void SynScanProtocolWrapper::move_axis_variable_rate(int axis, double rate_deg_per_sec) {
    pimpl_->move_axis_variable_rate(axis, rate_deg_per_sec);
}

std::pair<uint32_t, uint32_t> SynScanProtocolWrapper::get_ra_dec_raw(bool precise) {
    return pimpl_->get_ra_dec_raw(precise);
}

std::pair<uint32_t, uint32_t> SynScanProtocolWrapper::get_alt_az_raw(bool precise) {
    return pimpl_->get_alt_az_raw(precise);
}

void SynScanProtocolWrapper::goto_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
    pimpl_->goto_ra_dec_raw(ra_raw, dec_raw, precise);
}

void SynScanProtocolWrapper::goto_alt_az_raw(uint32_t az_raw, uint32_t alt_raw, bool precise) {
    pimpl_->goto_alt_az_raw(az_raw, alt_raw, precise);
}

void SynScanProtocolWrapper::sync_ra_dec_raw(uint32_t ra_raw, uint32_t dec_raw, bool precise) {
    pimpl_->sync_ra_dec_raw(ra_raw, dec_raw, precise);
}

LocationInfo SynScanProtocolWrapper::get_location() {
    return pimpl_->get_location();
}

void SynScanProtocolWrapper::set_location(const LocationInfo& info) {
    pimpl_->set_location(info);
}

TimeInfo SynScanProtocolWrapper::get_time() {
    return pimpl_->get_time();
}

void SynScanProtocolWrapper::set_time(const TimeInfo& info) {
    pimpl_->set_time(info);
}

} // namespace alpacacore::vendor::synscan

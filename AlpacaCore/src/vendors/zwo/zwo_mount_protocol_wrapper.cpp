// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>
#include <optional>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace alpacacore::vendor::zwo {

namespace {

constexpr auto kReadPollDelay = std::chrono::milliseconds(10);
constexpr int kIdleBreakMs = 100;

int map_mount_error_to_alpaca(int mount_error_code) {
    switch (mount_error_code) {
    case 2:
    case 5:
    case 6:
    case 8:
        return AlpacaError::InvalidValue;
    case 7:
        return AlpacaError::InvalidOperation;
    case 3:
    case 4:
    case 9:
    case 10:
    case 11:
    case 12:
        return AlpacaError::InvalidOperation;
    default:
        return AlpacaError::DriverException;
    }
}

const char* describe_mount_error(int mount_error_code) {
    switch (mount_error_code) {
    case 1: return "parameter beyond range";
    case 2: return "parameter format error";
    case 3: return "mount busy (homing/slewing/goto)";
    case 4: return "equipment moving";
    case 5: return "target under horizon";
    case 6: return "target under height limitation";
    case 7: return "time and position not synchronized";
    case 8: return "meridian limit reached while tracking";
    case 9: return "sync coordinates are on opposite sides of meridian";
    case 10: return "mount is parked";
    case 11: return "GOTO limit reached";
    case 12: return "altitude limit reached";
    default: return "unknown mount error";
    }
}

std::optional<int> extract_mount_error_code(const std::string& response) {
    if (response.empty()) {
        return std::nullopt;
    }
    if (response[0] != 'e' && response[0] != 'E') {
        return std::nullopt;
    }

    // Some successful responses are single letters (e.g. :Gm -> "E", "W", "N").
    // Treat only e/E followed by one or more digits as a mount error token.
    if (response.size() < 2 || !std::isdigit(static_cast<unsigned char>(response[1]))) {
        return std::nullopt;
    }

    std::string digits;
    for (std::size_t i = 1; i < response.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(response[i]);
        if (std::isdigit(ch)) {
            digits.push_back(static_cast<char>(ch));
        } else {
            break;
        }
    }

    return std::stoi(digits);
}

void throw_if_mount_error(const std::string& response, const std::string& context) {
    const auto mount_error = extract_mount_error_code(response);
    if (!mount_error.has_value()) {
        return;
    }
    throw AlpacaException(
        context + " failed with mount error e" + std::to_string(mount_error.value()) +
            " (" + describe_mount_error(mount_error.value()) + ")",
        map_mount_error_to_alpaca(mount_error.value()));
}

std::string trim_copy(std::string value) {
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(),
        value.end());
    return value;
}

bool parse_hms(const std::string& value, double& hours) {
    static const std::regex kPattern(R"(^\s*(\d{1,3})[:](\d{1,2})[:](\d{1,2})\s*$)");
    std::smatch match;
    if (!std::regex_match(value, match, kPattern)) {
        return false;
    }

    const int h = std::stoi(match[1].str());
    const int m = std::stoi(match[2].str());
    const int s = std::stoi(match[3].str());
    if (m > 59 || s > 59) {
        return false;
    }

    hours = static_cast<double>(h) + static_cast<double>(m) / 60.0 + static_cast<double>(s) / 3600.0;
    return true;
}

bool parse_angle_triplet(const std::string& value, bool require_sign, double& degrees_out) {
    if (value.empty()) {
        return false;
    }

    std::size_t pos = 0;
    bool negative = false;
    if (value[pos] == '+' || value[pos] == '-') {
        negative = value[pos] == '-';
        ++pos;
    } else if (require_sign) {
        return false;
    }

    std::vector<int> parts;
    int current = 0;
    bool in_number = false;
    for (; pos < value.size(); ++pos) {
        const unsigned char ch = static_cast<unsigned char>(value[pos]);
        if (std::isdigit(ch)) {
            current = (current * 10) + (ch - static_cast<unsigned char>('0'));
            in_number = true;
            continue;
        }
        if (in_number) {
            parts.push_back(current);
            current = 0;
            in_number = false;
        }
    }
    if (in_number) {
        parts.push_back(current);
    }

    if (parts.size() < 3) {
        return false;
    }
    if (parts[1] > 59 || parts[2] > 59) {
        return false;
    }

    double degrees = static_cast<double>(parts[0]) +
                     static_cast<double>(parts[1]) / 60.0 +
                     static_cast<double>(parts[2]) / 3600.0;
    if (negative) {
        degrees = -degrees;
    }

    degrees_out = degrees;
    return true;
}

std::string format_hms(double hours) {
    if (!std::isfinite(hours)) {
        return "00:00:00";
    }
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }

    int total_seconds = static_cast<int>(std::round(wrapped * 3600.0));
    total_seconds %= (24 * 3600);
    if (total_seconds < 0) {
        total_seconds += 24 * 3600;
    }

    const int h = total_seconds / 3600;
    const int m = (total_seconds % 3600) / 60;
    const int s = total_seconds % 60;

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << h << ':'
        << std::setfill('0') << std::setw(2) << m << ':'
        << std::setfill('0') << std::setw(2) << s;
    return oss.str();
}

std::string format_angle_colon(double degrees, int width_degrees) {
    if (!std::isfinite(degrees)) {
        degrees = 0.0;
    }

    const char sign = degrees < 0.0 ? '-' : '+';
    const double abs_deg = std::abs(degrees);
    int total_seconds = static_cast<int>(std::round(abs_deg * 3600.0));
    const int d = total_seconds / 3600;
    const int m = (total_seconds % 3600) / 60;
    const int s = total_seconds % 60;

    std::ostringstream oss;
    oss << sign
        << std::setfill('0') << std::setw(width_degrees) << d << ':'
        << std::setfill('0') << std::setw(2) << m << ':'
        << std::setfill('0') << std::setw(2) << s;
    return oss.str();
}

std::string format_angle_star_colon(double degrees, int width_degrees) {
    if (!std::isfinite(degrees)) {
        degrees = 0.0;
    }

    const char sign = degrees < 0.0 ? '-' : '+';
    const double abs_deg = std::abs(degrees);
    int total_seconds = static_cast<int>(std::round(abs_deg * 3600.0));
    const int d = total_seconds / 3600;
    const int m = (total_seconds % 3600) / 60;
    const int s = total_seconds % 60;

    std::ostringstream oss;
    oss << sign
        << std::setfill('0') << std::setw(width_degrees) << d << '*'
        << std::setfill('0') << std::setw(2) << m << ':'
        << std::setfill('0') << std::setw(2) << s;
    return oss.str();
}

std::string format_angle_star(double degrees, int width_degrees) {
    if (!std::isfinite(degrees)) {
        degrees = 0.0;
    }

    const char sign = degrees < 0.0 ? '-' : '+';
    const double abs_deg = std::abs(degrees);
    int total_seconds = static_cast<int>(std::round(abs_deg * 3600.0));
    const int d = total_seconds / 3600;
    const int m = (total_seconds % 3600) / 60;
    const int s = total_seconds % 60;

    std::ostringstream oss;
    oss << sign
        << std::setfill('0') << std::setw(width_degrees) << d << '*'
        << std::setfill('0') << std::setw(2) << m << ':'
        << std::setfill('0') << std::setw(2) << s;
    return oss.str();
}

// Protocol uses inverted sign for UTC offsets (UTC+8 is encoded as -08:00).
int parse_protocol_timezone_to_utc_minutes(const std::string& value) {
    static const std::regex kPattern(R"(^\s*([+-])(\d{1,2})(?::(\d{1,2}))?\s*$)");
    std::smatch match;
    if (!std::regex_match(value, match, kPattern)) {
        return 0;
    }

    const int hour = std::stoi(match[2].str());
    const int minute = match[3].matched ? std::stoi(match[3].str()) : 0;
    int protocol_minutes = (hour * 60) + minute;
    if (match[1].str() == "-") {
        protocol_minutes = -protocol_minutes;
    }
    return -protocol_minutes;
}

std::string format_utc_minutes_to_protocol_timezone(int utc_offset_minutes) {
    int protocol_minutes = -utc_offset_minutes;
    const char sign = protocol_minutes < 0 ? '-' : '+';
    protocol_minutes = std::abs(protocol_minutes);

    const int hour = protocol_minutes / 60;
    const int minute = protocol_minutes % 60;

    std::ostringstream oss;
    oss << sign << std::setfill('0') << std::setw(2) << hour
        << ':' << std::setfill('0') << std::setw(2) << minute;
    return oss.str();
}

} // namespace

class ZWOMountProtocolWrapper::Impl {
public:
    Impl()
        : connected_(false)
        , connection_type_(ConnectionType::Serial) {
#ifdef _WIN32
        serial_handle_ = INVALID_HANDLE_VALUE;
        socket_handle_ = INVALID_SOCKET;
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#else
        serial_fd_ = -1;
        socket_fd_ = -1;
#endif
    }

    ~Impl() {
        disconnect();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool connect(const ConnectionInfo& info) {
        if (info.type == ConnectionType::Serial && info.port_path.empty()) {
            ALPACA_LOG_ERROR("ZWO", "Serial connection requested but port path is empty");
            return false;
        }
        if (info.type == ConnectionType::Network && info.host.empty()) {
            ALPACA_LOG_ERROR("ZWO", "Network connection requested but host is empty");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_) {
            disconnect_locked();
        }

        connection_type_ = info.type;
        connection_info_ = info;

        bool ok = false;
        if (info.type == ConnectionType::Serial) {
            ok = connect_serial(info.port_path, info.baud_rate);
        } else {
            ok = connect_network(info.host, info.tcp_port);
        }

        connected_ = ok;
        return ok;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
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
            throw AlpacaException("Not connected to ZWO mount", AlpacaError::NotConnected);
        }

        std::string full_command = command;
        if (full_command.empty()) {
            throw AlpacaException("Command cannot be empty", AlpacaError::InvalidValue);
        }
        if (full_command.front() != ':') {
            full_command.insert(full_command.begin(), ':');
        }
        if (full_command.back() != '#') {
            full_command.push_back('#');
        }

        if (!write_data(full_command)) {
            throw AlpacaException("Failed to send command to ZWO mount");
        }

        int timeout_ms = timeout_ms_override;
        if (timeout_ms <= 0) {
            timeout_ms = connection_info_.response_timeout_ms;
        }
        if (timeout_ms <= 0) {
            timeout_ms = 5000;
        }

        std::string response = read_response(require_hash_terminator, timeout_ms);
        ALPACA_LOG_TRACE("ZWO", "CMD " + full_command + " RESP " + response);
        return response;
    }

    void send_command_blind(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to ZWO mount", AlpacaError::NotConnected);
        }

        std::string full_command = command;
        if (full_command.empty()) {
            throw AlpacaException("Command cannot be empty", AlpacaError::InvalidValue);
        }
        if (full_command.front() != ':') {
            full_command.insert(full_command.begin(), ':');
        }
        if (full_command.back() != '#') {
            full_command.push_back('#');
        }

        if (!write_data(full_command)) {
            throw AlpacaException("Failed to send command to ZWO mount");
        }

        ALPACA_LOG_TRACE("ZWO", "CMD " + full_command + " (blind)");

        try {
            (void)read_response(false, 40);
        } catch (const std::exception&) {
            // Ignore best-effort drain failures for fire-and-forget commands.
        }
    }

private:
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

    bool connect_serial(const std::string& port_path, int baud_rate) {
#ifdef _WIN32
        std::wstring wport_path(port_path.begin(), port_path.end());
        serial_handle_ = CreateFileW(
            wport_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

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

        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 100;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 100;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(serial_handle_, &timeouts);

        return true;
#else
        serial_fd_ = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            ALPACA_LOG_ERROR("ZWO", "Failed to open serial port " + port_path + ": errno=" +
                                            std::to_string(errno));
            return false;
        }

        termios tty {};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }

        speed_t speed = B9600;
        switch (baud_rate) {
        case 1200: speed = B1200; break;
        case 2400: speed = B2400; break;
        case 4800: speed = B4800; break;
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

        const int flags = fcntl(serial_fd_, F_GETFL);
        if (flags >= 0) {
            (void)fcntl(serial_fd_, F_SETFL, flags & ~O_NONBLOCK);
        }

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
#ifdef _WIN32
        if (port < 1 || port > static_cast<int>(std::numeric_limits<u_short>::max())) {
            return false;
        }
        socket_handle_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_handle_ == INVALID_SOCKET) {
            return false;
        }

        sockaddr_in addr {};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));

        addrinfo hints {};
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
            return false;
        }

        addr.sin_addr = (reinterpret_cast<sockaddr_in*>(result->ai_addr))->sin_addr;
        freeaddrinfo(result);

        if (::connect(socket_handle_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
            return false;
        }

        configure_network_timeouts();

        u_long mode = 0;
        ioctlsocket(socket_handle_, FIONBIO, &mode);
        return true;
#else
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }

        sockaddr_in addr {};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        hostent* host_entry = gethostbyname(host.c_str());
        if (host_entry == nullptr) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        addr.sin_addr = *reinterpret_cast<in_addr*>(host_entry->h_addr);

        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
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
        }
        return write_network(data);
    }

    bool write_serial(const std::string& data) {
#ifdef _WIN32
        const auto data_size = data.size();
        if (data_size > std::numeric_limits<DWORD>::max()) {
            return false;
        }
        const DWORD requested = static_cast<DWORD>(data_size);
        DWORD bytes_written = 0;
        return WriteFile(serial_handle_, data.c_str(), requested, &bytes_written, nullptr) &&
               bytes_written == requested;
#else
        const ssize_t bytes_written = write(serial_fd_, data.c_str(), data.size());
        return bytes_written == static_cast<ssize_t>(data.size());
#endif
    }

    bool write_network(const std::string& data) {
#ifdef _WIN32
        const auto data_size = data.size();
        if (data_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        const int requested = static_cast<int>(data_size);
        const int bytes_sent = send(socket_handle_, data.c_str(), requested, 0);
        return bytes_sent == requested;
#else
        const ssize_t bytes_sent = send(socket_fd_, data.c_str(), data.size(), 0);
        return bytes_sent == static_cast<ssize_t>(data.size());
#endif
    }

    std::string read_response(bool require_hash_terminator, int timeout_ms) {
        std::string response;
        const auto start = std::chrono::steady_clock::now();
        auto last_char_time = start;

        while (true) {
            char ch = '\0';
            bool got_char = false;
            if (connection_type_ == ConnectionType::Serial) {
                got_char = read_serial_char(ch);
            } else {
                got_char = read_network_char(ch);
            }

            if (got_char) {
                last_char_time = std::chrono::steady_clock::now();
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                if (ch == '#') {
                    break;
                }
                response.push_back(ch);
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            if (elapsed.count() > timeout_ms) {
                if (!require_hash_terminator) {
                    return response;
                }
                throw AlpacaException("Timeout waiting for ZWO mount response", AlpacaError::DriverException);
            }

            if (!require_hash_terminator && !got_char && !response.empty()) {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_char_time);
                if (idle.count() > kIdleBreakMs) {
                    break;
                }
            }

            std::this_thread::sleep_for(kReadPollDelay);
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
        const ssize_t bytes_read = read(serial_fd_, &ch, 1);
        return bytes_read == 1;
#endif
    }

    bool read_network_char(char& ch) {
#ifdef _WIN32
        const int bytes_received = recv(socket_handle_, &ch, 1, 0);
        return bytes_received == 1;
#else
        const ssize_t bytes_received = recv(socket_fd_, &ch, 1, 0);
        return bytes_received == 1;
#endif
    }

    void configure_network_timeouts() {
        constexpr int kSocketTimeoutMs = 200;
#ifdef _WIN32
        const DWORD timeout = kSocketTimeoutMs;
        setsockopt(socket_handle_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_handle_, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout {};
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

ZWOMountProtocolWrapper::ZWOMountProtocolWrapper()
    : pimpl_(std::make_unique<Impl>()) {}

ZWOMountProtocolWrapper::~ZWOMountProtocolWrapper() = default;

ZWOMountProtocolWrapper& ZWOMountProtocolWrapper::instance() {
    static ZWOMountProtocolWrapper wrapper;
    return wrapper;
}

bool ZWOMountProtocolWrapper::connect(const ConnectionInfo& info) {
    return pimpl_->connect(info);
}

void ZWOMountProtocolWrapper::disconnect() {
    pimpl_->disconnect();
}

bool ZWOMountProtocolWrapper::is_connected() const {
    return pimpl_->is_connected();
}

std::string ZWOMountProtocolWrapper::send_command(const std::string& command,
                                                  bool require_hash_terminator,
                                                  int timeout_ms_override) {
    return pimpl_->send_command(command, require_hash_terminator, timeout_ms_override);
}

void ZWOMountProtocolWrapper::send_command_blind(const std::string& command) {
    pimpl_->send_command_blind(command);
}

std::string ZWOMountProtocolWrapper::get_mount_info() {
    std::string response = send_command(":GVP");
    throw_if_mount_error(response, ":GVP");
    return trim_copy(std::move(response));
}

EquatorialCoordinates ZWOMountProtocolWrapper::get_current_equatorial() {
    EquatorialCoordinates out;

    const std::string ra_response = send_command(":GR");
    throw_if_mount_error(ra_response, ":GR");
    if (!parse_hms(ra_response, out.ra_hours)) {
        throw AlpacaException("Invalid RA response from ZWO mount", AlpacaError::DriverException);
    }

    const std::string dec_response = send_command(":GD");
    throw_if_mount_error(dec_response, ":GD");
    if (!parse_angle_triplet(dec_response, true, out.dec_degrees)) {
        throw AlpacaException("Invalid Dec response from ZWO mount", AlpacaError::DriverException);
    }

    return out;
}

HorizontalCoordinates ZWOMountProtocolWrapper::get_current_horizontal() {
    HorizontalCoordinates out;

    const std::string az_response = send_command(":GZ");
    throw_if_mount_error(az_response, ":GZ");
    if (!parse_angle_triplet(az_response, false, out.azimuth_degrees)) {
        throw AlpacaException("Invalid Azimuth response from ZWO mount", AlpacaError::DriverException);
    }

    const std::string alt_response = send_command(":GA");
    throw_if_mount_error(alt_response, ":GA");
    if (!parse_angle_triplet(alt_response, true, out.altitude_degrees)) {
        throw AlpacaException("Invalid Altitude response from ZWO mount", AlpacaError::DriverException);
    }

    return out;
}

EquatorialCoordinates ZWOMountProtocolWrapper::get_target_equatorial() {
    EquatorialCoordinates out;

    const std::string ra_response = send_command(":Gr");
    throw_if_mount_error(ra_response, ":Gr");
    if (!parse_hms(ra_response, out.ra_hours)) {
        throw AlpacaException("Invalid target RA response from ZWO mount", AlpacaError::DriverException);
    }

    const std::string dec_response = send_command(":Gd");
    throw_if_mount_error(dec_response, ":Gd");
    if (!parse_angle_triplet(dec_response, true, out.dec_degrees)) {
        throw AlpacaException("Invalid target Dec response from ZWO mount", AlpacaError::DriverException);
    }

    return out;
}

void ZWOMountProtocolWrapper::set_target_ra(double ra_hours) {
    if (!std::isfinite(ra_hours) || ra_hours < 0.0 || ra_hours >= 24.0) {
        throw AlpacaException("RA must be in [0,24) hours", AlpacaError::InvalidValue);
    }

    const std::string command = ":Sr" + format_hms(ra_hours);
    const std::string response = send_command(command, false);
    throw_if_mount_error(response, ":Sr");
    if (!response.empty() && response[0] != '1') {
        throw AlpacaException("ZWO mount rejected target RA", AlpacaError::InvalidValue);
    }
}

void ZWOMountProtocolWrapper::set_target_dec(double dec_degrees) {
    if (!std::isfinite(dec_degrees) || dec_degrees < -90.0 || dec_degrees > 90.0) {
        throw AlpacaException("Dec must be in [-90,+90] degrees", AlpacaError::InvalidValue);
    }

    const std::string command = ":Sd" + format_angle_colon(dec_degrees, 2);
    const std::string response = send_command(command, false);
    throw_if_mount_error(response, ":Sd");
    if (!response.empty() && response[0] != '1') {
        throw AlpacaException("ZWO mount rejected target Dec", AlpacaError::InvalidValue);
    }
}

void ZWOMountProtocolWrapper::sync_target_equatorial(double ra_hours, double dec_degrees) {
    if (!std::isfinite(ra_hours) || ra_hours < 0.0 || ra_hours >= 24.0) {
        throw AlpacaException("RA must be in [0,24) hours", AlpacaError::InvalidValue);
    }
    if (!std::isfinite(dec_degrees) || dec_degrees < -90.0 || dec_degrees > 90.0) {
        throw AlpacaException("Dec must be in [-90,+90] degrees", AlpacaError::InvalidValue);
    }

    const std::string command = ":SMMC" + format_hms(ra_hours) + "&" +
                                format_angle_star_colon(dec_degrees, 2);
    const std::string response = trim_copy(send_command(command, false));
    throw_if_mount_error(response, ":SMMC");
    if (response.empty() || response == "N/A") {
        return;
    }
    throw AlpacaException("ZWO mount rejected sync request", AlpacaError::InvalidOperation);
}

bool ZWOMountProtocolWrapper::goto_target() {
    const std::string response = trim_copy(send_command(":MS", false));
    throw_if_mount_error(response, ":MS");
    if (response.empty()) {
        // TODO: Confirm whether all mount firmware revisions always reply to :MS.
        return true;
    }
    if (response == "0" || response == "1") {
        return true;
    }
    if (response == "N/A") {
        return true;
    }
    return false;
}

void ZWOMountProtocolWrapper::sync_target() {
    const std::string response = trim_copy(send_command(":CM", false));
    throw_if_mount_error(response, ":CM");
    if (response.empty() || response == "N/A" || response == "1") {
        return;
    }
    throw AlpacaException("ZWO mount rejected sync request", AlpacaError::InvalidOperation);
}

void ZWOMountProtocolWrapper::abort_motion() {
    send_command_blind(":Q");
}

StatusInfo ZWOMountProtocolWrapper::get_status() {
    StatusInfo status;
    status.raw = trim_copy(send_command(":GU", false));
    throw_if_mount_error(status.raw, ":GU");

    status.no_tracking = status.raw.find('n') != std::string::npos;
    status.stop_or_tracking = status.raw.find('N') != std::string::npos;
    status.low_power = status.raw.find('L') != std::string::npos;
    status.at_home = status.raw.find('H') != std::string::npos;
    status.ra_stall = status.raw.find('S') != std::string::npos;
    status.dec_stall = status.raw.find('s') != std::string::npos;
    status.ra_guiding = status.raw.find('T') != std::string::npos;
    status.dec_guiding = status.raw.find('t') != std::string::npos;

    if (status.raw.find('G') != std::string::npos) {
        status.mode = MountMode::Equatorial;
    } else if (status.raw.find('Z') != std::string::npos) {
        status.mode = MountMode::AltAzimuth;
    }

    return status;
}

bool ZWOMountProtocolWrapper::get_tracking_enabled() {
    const std::string response = trim_copy(send_command(":GAT", false));
    if (const auto mount_error = extract_mount_error_code(response); mount_error.has_value()) {
        // :GAT reports abnormal tracking-off states as e+error.
        // Surface these as tracking=false for Alpaca property reads.
        ALPACA_LOG_WARN(
            "ZWO",
            ":GAT reported mount error e" + std::to_string(mount_error.value()) +
                " (" + describe_mount_error(mount_error.value()) + "); treating Tracking as false");
        return false;
    }
    if (response.empty()) {
        throw AlpacaException("Invalid tracking state response", AlpacaError::DriverException);
    }
    if (response[0] == '1') {
        return true;
    }
    if (response[0] == '0') {
        return false;
    }
    throw AlpacaException("Unknown tracking state response: " + response, AlpacaError::DriverException);
}

void ZWOMountProtocolWrapper::set_tracking_enabled(bool enabled) {
    const char* command = enabled ? ":Te" : ":Td";
    const std::string response = trim_copy(send_command(command, false));
    throw_if_mount_error(response, command);
    if (response.empty()) {
        // TODO: Confirm whether all firmware revisions return success digits for :Te/:Td.
        return;
    }
    if (response[0] == '1') {
        return;
    }

    // Some firmware revisions can return "0" when already in the requested state.
    if (response[0] == '0') {
        if (get_tracking_enabled() == enabled) {
            return;
        }
        if (!enabled) {
            ALPACA_LOG_WARN(
                "ZWO",
                ":Td returned 0 and tracking stayed enabled; treating as transient firmware refusal");
            return;
        }
    }

    throw AlpacaException(
        std::string("Failed to ") + (enabled ? "enable" : "disable") + " tracking",
        AlpacaError::InvalidOperation);
}

int ZWOMountProtocolWrapper::get_tracking_rate() {
    const std::string response = trim_copy(send_command(":GT", false));
    throw_if_mount_error(response, ":GT");
    if (response.empty() || !std::isdigit(static_cast<unsigned char>(response[0]))) {
        throw AlpacaException("Invalid tracking rate response", AlpacaError::DriverException);
    }

    const int raw_rate = response[0] - '0';
    if (raw_rate < 0 || raw_rate > 3) {
        return 0;
    }

    // TODO: Verify whether firmware reports 1=lunar/2=solar or 1=solar/2=lunar.
    if (raw_rate == 3) {
        return 2;
    }
    return raw_rate;
}

void ZWOMountProtocolWrapper::set_tracking_rate(int rate) {
    switch (rate) {
    case 0:
        send_command_blind(":TQ");
        return;
    case 1:
        send_command_blind(":TL");
        return;
    case 2:
        send_command_blind(":TS");
        return;
    default:
        throw AlpacaException("Tracking rate must be 0, 1, or 2", AlpacaError::InvalidValue);
    }
}

void ZWOMountProtocolWrapper::set_move_rate_sidereal_multiple(double multiple) {
    if (!std::isfinite(multiple) || multiple < 0.0 || multiple > 1440.0) {
        throw AlpacaException("Move rate multiplier must be in [0,1440]", AlpacaError::InvalidValue);
    }

    std::ostringstream rate;
    rate << std::fixed << std::setprecision(2) << std::setfill('0') << std::setw(7) << multiple;
    send_command_blind(":Rv" + rate.str());
}

void ZWOMountProtocolWrapper::start_move_east() {
    send_command_blind(":Me");
}

void ZWOMountProtocolWrapper::stop_move_east() {
    send_command_blind(":Qe");
}

void ZWOMountProtocolWrapper::start_move_west() {
    send_command_blind(":Mw");
}

void ZWOMountProtocolWrapper::stop_move_west() {
    send_command_blind(":Qw");
}

void ZWOMountProtocolWrapper::start_move_north() {
    send_command_blind(":Mn");
}

void ZWOMountProtocolWrapper::stop_move_north() {
    send_command_blind(":Qn");
}

void ZWOMountProtocolWrapper::start_move_south() {
    send_command_blind(":Ms");
}

void ZWOMountProtocolWrapper::stop_move_south() {
    send_command_blind(":Qs");
}

void ZWOMountProtocolWrapper::pulse_guide(int direction, int duration_ms) {
    if (duration_ms < 0) {
        throw AlpacaException("Pulse guide duration must be >= 0 ms", AlpacaError::InvalidValue);
    }

    const int effective_duration_ms = std::clamp(duration_ms, 0, 9999);
    if (effective_duration_ms != duration_ms) {
        ALPACA_LOG_WARN(
            "ZWO",
            "Pulse guide duration " + std::to_string(duration_ms) +
                "ms exceeds protocol maximum 9999ms; clamping to 9999ms");
    }

    char dir = '\0';
    switch (direction) {
    case 0: dir = 'n'; break;
    case 1: dir = 's'; break;
    case 2: dir = 'e'; break;
    case 3: dir = 'w'; break;
    default:
        throw AlpacaException("Pulse guide direction must be 0..3", AlpacaError::InvalidValue);
    }

    std::ostringstream cmd;
    cmd << ":Mg" << dir << std::setfill('0') << std::setw(4) << effective_duration_ms;
    send_command_blind(cmd.str());
}

double ZWOMountProtocolWrapper::get_guide_rate() {
    const std::string response = trim_copy(send_command(":Ggr", false));
    throw_if_mount_error(response, ":Ggr");
    if (response.empty()) {
        throw AlpacaException("Invalid guide rate response", AlpacaError::DriverException);
    }
    double value = std::stod(response);
    // Some firmware revisions return the 0.1-0.9 rate, others return a value scaled by 15.
    if (value > 1.0 && value <= 15.0) {
        value /= 15.0;
    }
    return value;
}

void ZWOMountProtocolWrapper::set_guide_rate(double guide_rate) {
    if (!std::isfinite(guide_rate) || guide_rate < 0.10 || guide_rate > 0.90) {
        throw AlpacaException("Guide rate must be in [0.10,0.90]", AlpacaError::InvalidValue);
    }

    auto send_rate = [this](double value, int precision) {
        std::ostringstream payload;
        payload << std::fixed << std::setprecision(precision) << value;
        send_command_blind(":Rg" + payload.str());
    };

    // Preferred format per v2.x docs: :Rg0.nn (0.10-0.90).
    send_rate(guide_rate, 2);

    // Some firmware expects the rate scaled by 15 (e.g., 0.1x -> 1.5).
    try {
        const double readback = get_guide_rate();
        if (std::abs(readback - guide_rate) <= 0.02) {
            return;
        }
    } catch (const std::exception&) {
    }

    const double scaled = guide_rate * 15.0;
    send_rate(scaled, 1);
}

SiteInfo ZWOMountProtocolWrapper::get_site_info() {
    const std::string response = trim_copy(send_command(":GMGE", false));
    throw_if_mount_error(response, ":GMGE");

    SiteInfo info;
    auto separator = response.find('&');
    if (separator == std::string::npos) {
        throw AlpacaException("Invalid site info response", AlpacaError::DriverException);
    }

    const std::string lat = response.substr(0, separator);
    const std::string lon = response.substr(separator + 1);

    if (!parse_angle_triplet(lat, true, info.latitude_degrees) ||
        !parse_angle_triplet(lon, true, info.longitude_degrees)) {
        throw AlpacaException("Unable to parse site coordinates", AlpacaError::DriverException);
    }

    return info;
}

void ZWOMountProtocolWrapper::set_site_info(const SiteInfo& info) {
    if (!std::isfinite(info.latitude_degrees) || info.latitude_degrees < -90.0 || info.latitude_degrees > 90.0) {
        throw AlpacaException("Latitude must be in [-90,+90]", AlpacaError::InvalidValue);
    }
    if (!std::isfinite(info.longitude_degrees) || info.longitude_degrees < -180.0 || info.longitude_degrees > 180.0) {
        throw AlpacaException("Longitude must be in [-180,+180]", AlpacaError::InvalidValue);
    }

    const std::string lat = format_angle_star(info.latitude_degrees, 2);
    const std::string lon = format_angle_star(info.longitude_degrees, 3);

    const std::string response = trim_copy(send_command(":SMGE" + lat + "&" + lon, false));
    throw_if_mount_error(response, ":SMGE");
    if (!response.empty() && response[0] != '1') {
        throw AlpacaException("Failed to set site coordinates", AlpacaError::InvalidOperation);
    }
}

TimeInfo ZWOMountProtocolWrapper::get_time_info() {
    const std::string response = trim_copy(send_command(":GMTI", false));
    throw_if_mount_error(response, ":GMTI");

    const std::size_t first_sep = response.find('&');
    if (first_sep == std::string::npos) {
        throw AlpacaException("Invalid time response from ZWO mount", AlpacaError::DriverException);
    }
    const std::size_t second_sep = response.find('&', first_sep + 1);
    if (second_sep == std::string::npos) {
        throw AlpacaException("Invalid time response from ZWO mount", AlpacaError::DriverException);
    }

    const std::string date = response.substr(0, first_sep);
    const std::string time = response.substr(first_sep + 1, second_sep - first_sep - 1);
    const std::string tz = response.substr(second_sep + 1);

    TimeInfo info;

    {
        std::smatch match;
        static const std::regex kDatePattern(R"(^\s*(\d{1,2})/(\d{1,2})/(\d{2})\s*$)");
        if (!std::regex_match(date, match, kDatePattern)) {
            throw AlpacaException("Invalid date response from ZWO mount", AlpacaError::DriverException);
        }
        info.month = std::stoi(match[1].str());
        info.day = std::stoi(match[2].str());
        const int yy = std::stoi(match[3].str());
        info.year = (yy <= 79) ? (2000 + yy) : (1900 + yy);
    }

    {
        std::smatch match;
        static const std::regex kTimePattern(R"(^\s*(\d{1,2}):(\d{1,2}):(\d{1,2})\s*$)");
        if (!std::regex_match(time, match, kTimePattern)) {
            throw AlpacaException("Invalid clock response from ZWO mount", AlpacaError::DriverException);
        }
        info.hour = std::stoi(match[1].str());
        info.minute = std::stoi(match[2].str());
        info.second = std::stoi(match[3].str());
    }

    info.timezone_offset_minutes = parse_protocol_timezone_to_utc_minutes(tz);
    return info;
}

void ZWOMountProtocolWrapper::set_time_info(const TimeInfo& info) {
    if (info.month < 1 || info.month > 12 ||
        info.day < 1 || info.day > 31 ||
        info.hour < 0 || info.hour > 23 ||
        info.minute < 0 || info.minute > 59 ||
        info.second < 0 || info.second > 59 ||
        info.timezone_offset_minutes < -14 * 60 || info.timezone_offset_minutes > 14 * 60) {
        throw AlpacaException("Invalid date/time values", AlpacaError::InvalidValue);
    }

    const int yy = info.year % 100;

    std::ostringstream command;
    command << ":SMTI"
            << std::setfill('0') << std::setw(2) << info.month << '/'
            << std::setfill('0') << std::setw(2) << info.day << '/'
            << std::setfill('0') << std::setw(2) << yy << '&'
            << std::setfill('0') << std::setw(2) << info.hour << ':'
            << std::setfill('0') << std::setw(2) << info.minute << ':'
            << std::setfill('0') << std::setw(2) << info.second << '&'
            << format_utc_minutes_to_protocol_timezone(info.timezone_offset_minutes);

    const std::string response = trim_copy(send_command(command.str(), false));
    throw_if_mount_error(response, ":SMTI");
    if (!response.empty() && response[0] != '1') {
        throw AlpacaException("Failed to set mount date/time", AlpacaError::InvalidOperation);
    }
}

double ZWOMountProtocolWrapper::get_sidereal_time_hours() {
    const std::string response = send_command(":GS");
    throw_if_mount_error(response, ":GS");

    double hours = 0.0;
    if (!parse_hms(response, hours)) {
        throw AlpacaException("Invalid sidereal time response", AlpacaError::DriverException);
    }
    return hours;
}

char ZWOMountProtocolWrapper::get_mount_direction() {
    const std::string response = trim_copy(send_command(":Gm", false));
    throw_if_mount_error(response, ":Gm");
    if (response.empty()) {
        return 'N';
    }
    return response[0];
}

void ZWOMountProtocolWrapper::go_home() {
    send_command_blind(":hC");
}

void ZWOMountProtocolWrapper::park() {
    send_command_blind(":hP");
}

bool ZWOMountProtocolWrapper::set_custom_park_here() {
    const std::string response = trim_copy(send_command(":Sp01", false));
    throw_if_mount_error(response, ":Sp01");
    return !response.empty() && response[0] == '1';
}

bool ZWOMountProtocolWrapper::unpark() {
    const std::string response = trim_copy(send_command(":Spu", false));
    throw_if_mount_error(response, ":Spu");
    return !response.empty() && response[0] == '1';
}

ParkStatus ZWOMountProtocolWrapper::get_park_status() {
    const std::string response = trim_copy(send_command(":Gps", false));
    throw_if_mount_error(response, ":Gps");
    if (response.empty()) {
        return ParkStatus::Unknown;
    }
    switch (response[0]) {
    case '0': return ParkStatus::NotParked;
    case '1': return ParkStatus::InProgress;
    case '2': return ParkStatus::Completed;
    case '3': return ParkStatus::Error;
    default: return ParkStatus::Unknown;
    }
}

bool ZWOMountProtocolWrapper::has_successful_homing() {
    const std::string response = trim_copy(send_command(":Gh", false));
    throw_if_mount_error(response, ":Gh");
    return !response.empty() && response[0] == '1';
}

} // namespace alpacacore::vendor::zwo

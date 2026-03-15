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

#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/units.h>
#include <mutex>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <cctype>
#include <cstring>
#include <ctime>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

namespace alpacacore::vendor::ioptron {

namespace {

std::string strip_status_prefix(std::string response) {
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
            ALPACA_LOG_INFO("iOptron", "Already connected, disconnecting first");
            disconnect();
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
        
        // Set to blocking mode
        ALPACA_LOG_INFO("iOptron", "Setting to blocking mode...");
        int flags = fcntl(serial_fd_, F_GETFL);
        if (flags < 0) {
            int fc_err = errno;
            ALPACA_LOG_ERROR("iOptron", "fcntl F_GETFL failed: " + std::string(std::strerror(fc_err)) + " (errno " + std::to_string(fc_err) + ")");
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }
        if (fcntl(serial_fd_, F_SETFL, flags & ~O_NONBLOCK) != 0) {
            int fc_err = errno;
            ALPACA_LOG_ERROR("iOptron", "fcntl F_SETFL failed: " + std::string(std::strerror(fc_err)) + " (errno " + std::to_string(fc_err) + ")");
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
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        
        sockaddr_in addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        // Resolve hostname
        struct hostent* host_entry = gethostbyname(host.c_str());
        if (host_entry == nullptr) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        
        addr.sin_addr = *((struct in_addr*)host_entry->h_addr);
        
        if (::connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
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
        const DWORD requested = static_cast<DWORD>(data_size);
        DWORD bytes_written = 0;
        return WriteFile(serial_handle_, data.c_str(), requested, &bytes_written, nullptr) &&
               bytes_written == requested;
#else
        ssize_t bytes_written = write(serial_fd_, data.c_str(), data.length());
        return bytes_written == static_cast<ssize_t>(data.length());
#endif
    }
    
    bool write_network(const std::string& data) {
#ifdef _WIN32
        const auto data_size = data.size();
        if (data_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        const int requested = static_cast<int>(data_size);
        int bytes_sent = send(socket_handle_, data.c_str(), requested, 0);
        return bytes_sent == requested;
#else
        ssize_t bytes_sent = send(socket_fd_, data.c_str(), data.length(), 0);
        return bytes_sent == static_cast<ssize_t>(data.length());
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
        return (negative ? -1 : 1) * std::stoll(str.substr(start));
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

// Mount information queries
MountInfo iOptronProtocolWrapper::get_mount_info() {
    // Legacy helper retained for potential future use, but the current
    // driver does not depend on mount-specific model information.
    // We issue a single :MountInfo query (no retries) and return the
    // raw response as the model_code; model_name and has_encoder are
    // left at their defaults.
    MountInfo info;
    try {
        std::string response = send_command(":MountInfo");
        info.model_code = std::move(response);
    } catch (const std::exception&) {
        // Swallow errors; callers should treat missing model info as non-fatal.
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
    int64_t ra_value = std::stoll(ra_str);
    
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
    int64_t az_value = std::stoll(az_str);
    
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
    int64_t lat_offset = std::stoll(lat_str);
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
    
    int64_t alt_value = std::stoll(alt_str);
    int64_t az_value = std::stoll(az_str);
    
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
    treatment.degrees_past = std::stoi(response.substr(1, 2));
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
    // :MS1 returns "1" (accepted) or "0" (rejected). Read the response to keep the stream aligned.
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
    
    std::string alt_cmd = ":SPH" + std::to_string(alt_value) + "#";
    std::string az_cmd = ":SPA" + std::to_string(az_value) + "#";
    
    send_command(alt_cmd, false);
    send_command(az_cmd, false);
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
    send_command(is_northern ? ":SHE1#" : ":SHE0#", false);
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
    int64_t ms_since_j2000 = std::stoll(time_digits);
    
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
    int value = std::stoi(response);
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
    
    double ra_rate = std::stod("0." + ra_str);
    double dec_rate = std::stod("0." + dec_str);
    
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

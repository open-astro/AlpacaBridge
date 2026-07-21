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
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::gemini {

namespace {
constexpr int kCommandDelayMs = 50;
constexpr int kReadCharDelayMs = 10;
constexpr int kMaxResponseLen = 32;
constexpr int kHandshakeRetries = 3;
constexpr int kHandshakeRetryDelayS = 1;
constexpr int kHandshakeReadTimeoutS = 2;  // Shorter timeout during handshake

// Extract the trailing run of decimal digits from a '#'-terminated response
// (e.g. "*V206#" -> 206, "*J50#" -> 50). Confirmed against real hardware:
// responses are "*" + echoed command letter + decimal payload + "#".
bool parse_trailing_int(const std::string& resp, int& out) {
    std::string digits;
    for (auto it = resp.rbegin(); it != resp.rend(); ++it) {
        if (*it == '#') continue;
        if (std::isdigit(static_cast<unsigned char>(*it))) {
            digits.insert(digits.begin(), *it);
        } else if (!digits.empty()) {
            break;
        }
    }
    if (digits.empty()) {
        return false;
    }
    out = std::atoi(digits.c_str());
    return true;
}

// >S# is confirmed (against real hardware) to reply "*S<d1><d2><d3>#" -- three
// single-digit flags, not one combined number. d1 is the light on/off flag
// (e.g. "*S111#" while lit, "*S011#" once turned off); d2/d3 stayed "1" across
// every light/brightness change observed and are presumed cover-related flags
// that don't apply to this motorless model. Only d1 is needed here, so pull
// the digit immediately after 'S' rather than treating the payload as one
// trailing integer (parse_trailing_int would read "*S011#" as 11 -- nonzero,
// i.e. wrongly "on").
bool parse_light_flag(const std::string& resp, bool& out) {
    auto pos = resp.find('S');
    if (pos == std::string::npos || pos + 1 >= resp.size()) {
        return false;
    }
    char c = resp[pos + 1];
    if (!std::isdigit(static_cast<unsigned char>(c))) {
        return false;
    }
    out = (c != '0');
    return true;
}
}  // namespace

// Probe a serial port with the flat panel's identity handshake (>H#).
// Returns true if a well-formed ('#'-terminated) response was received.
//
// Mirrors the Gemini focuser's probe_port(): the CH340 USB-serial adapter
// asserts DTR on open, which can reset the panel's microcontroller. HUPCL is
// cleared before closing so DTR stays high and a subsequent connect() doesn't
// trigger a second reset.
static bool probe_port(const std::string& port_path) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return false;
    }

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~HUPCL;  // Keep DTR high on close -- prevents MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 50;  // 5-second per-character timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return false;
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return false;
    }
    tcflush(fd, TCIOFLUSH);

    // Wait for MCU boot after the DTR reset, then try the handshake. Two
    // attempts: first waits 2s for boot, second is a quick retry.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 1));
        tcflush(fd, TCIOFLUSH);

        const char* cmd = ">H#";
        if (!util::write_all(fd, cmd, std::strlen(cmd))) {
            continue;
        }

        // Must hold the full ">H#" reply -- confirmed against real hardware
        // as "*HGeminiFlatPanelLite#" (22 chars). A too-small buffer here
        // silently truncates before the '#' terminator, so the well-formed
        // check below never passes and the port is wrongly rejected.
        char resp[kMaxResponseLen] = {};
        int total = 0;
        auto start = std::chrono::steady_clock::now();
        while (total < static_cast<int>(sizeof(resp)) - 1) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > 5) {
                break;
            }
            char ch = 0;
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                if (ch == '\r' || ch == '\n') continue;
                resp[total++] = ch;
                if (ch == '#') break;
            } else if (r == 0) {
                continue;  // VTIME timeout, keep trying
            } else {
                break;
            }
        }

        if (total > 0 && resp[total - 1] == '#') {
            tcgetattr(fd, &tty);
            tty.c_cflag &= ~HUPCL;
            tcsetattr(fd, TCSANOW, &tty);
            close(fd);
            return true;
        }

        ALPACA_LOG_DEBUG("Gemini", "Flat panel probe attempt " + std::to_string(attempt + 1) +
                                       " got no valid response from " + port_path);
    }

    close(fd);
#else
    (void)port_path;
#endif
    return false;
}

std::vector<GeminiFlatPanelPortInfo> enumerate_gemini_flatpanel_ports() {
    std::vector<GeminiFlatPanelPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (std::filesystem::exists(port)) {
                ALPACA_LOG_INFO("Gemini", "Probing " + port + " for a flat panel...");
                if (probe_port(port)) {
                    ALPACA_LOG_INFO("Gemini", "Found flat panel on " + port);
                    results.push_back({port, ""});
                }
            }
        }
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(serial_by_id)) {
        if (!entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();

        // Confirmed against real hardware: the panel's controller is an
        // ESP32-class board using Espressif's native USB-serial/JTAG stack
        // (by-id name "usb-Espressif_USB_JTAG_serial_debug_unit_..."), not a
        // CH340/CH341 adapter like the Gemini focuser. Keep the CH340/CH341/
        // generic USB_Serial patterns too in case a different panel revision
        // uses an external USB-serial chip instead.
        bool is_candidate = (name.find("USB_Serial") != std::string::npos) ||
                            (name.find("CH340") != std::string::npos) || (name.find("CH341") != std::string::npos) ||
                            (name.find("1a86") != std::string::npos) || (name.find("Espressif") != std::string::npos);
        if (!is_candidate) continue;

        std::string resolved = std::filesystem::canonical(entry.path()).string();
        std::string probe_msg = "Probing ";
        probe_msg += resolved;
        probe_msg += " (";
        probe_msg += name;
        probe_msg += ") for a flat panel...";
        ALPACA_LOG_INFO("Gemini", probe_msg);

        if (probe_port(resolved)) {
            ALPACA_LOG_INFO("Gemini", "Found flat panel on " + resolved);
            results.push_back({resolved, name});
        }
    }
#endif

    return results;
}

class GeminiFlatPanelProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    std::string connect(const FlatPanelConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;

        connect_serial();

        // Handshake: identity query with retries, same cadence as the Gemini
        // focuser (worst case ~9.1s, under the ASCOM client ~10s timeout).
        bool success = false;
        int saved_timeout = config_.serial_timeout_s;
        config_.serial_timeout_s = kHandshakeReadTimeoutS;

        for (int attempt = 0; attempt < kHandshakeRetries && !success; ++attempt) {
            if (attempt == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else if (attempt == 1) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(kHandshakeRetryDelayS));
            }

#ifndef _WIN32
            tcflush(serial_fd_, TCIOFLUSH);
#else
            PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#endif

            try {
                // Confirmed against real hardware: >H# replies
                // "*HGeminiFlatPanelLite#". Any well-formed '#'-terminated
                // reply is still accepted here (rather than requiring an exact
                // string match) since that's enough to prove a live device is
                // on the other end.
                send_command_locked(">H#");
                success = true;
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini",
                                "Flat panel handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }

        config_.serial_timeout_s = saved_timeout;

        if (!success) {
            disconnect_locked();
            throw AlpacaException(
                "Gemini flat panel handshake failed after " + std::to_string(kHandshakeRetries) + " attempts",
                AlpacaError::NotConnected);
        }

        std::string firmware;
        try {
            std::string resp = send_command_locked(">V#");
            int ver = 0;
            if (parse_trailing_int(resp, ver)) {
                firmware = std::to_string(ver);
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Flat panel firmware query failed: " + std::string(e.what()));
        }

        connected_ = true;
        ALPACA_LOG_INFO("Gemini", "Flat panel connected" + (firmware.empty() ? "" : (", firmware: " + firmware)));
        return firmware;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool get_light_on() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(">S#");
        bool on = false;
        if (!parse_light_flag(resp, on)) {
            throw AlpacaException("Failed to parse light status: " + resp, AlpacaError::DriverException);
        }
        return on;
    }

    int get_brightness() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(">J#");
        int val = 0;
        if (!parse_trailing_int(resp, val)) {
            throw AlpacaException("Failed to parse brightness: " + resp, AlpacaError::DriverException);
        }
        return val;
    }

    void light_on() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_locked(">L#");
    }

    void light_off() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_locked(">D#");
    }

    void set_brightness(int value) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[kMaxResponseLen];
        std::snprintf(cmd, sizeof(cmd), ">B%d#", value);
        send_command_locked(cmd);
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Gemini flat panel not connected", AlpacaError::NotConnected);
        }
    }

    void connect_serial() {
#ifdef _WIN32
        std::string port_name = "\\\\.\\" + config_.serial_port;
        serial_handle_ =
            CreateFileA(port_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (serial_handle_ == INVALID_HANDLE_VALUE) {
            throw AlpacaException("Failed to open serial port: " + config_.serial_port, AlpacaError::NotConnected);
        }

        DCB dcb = {};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            throw AlpacaException("Failed to get serial port state", AlpacaError::DriverException);
        }

        dcb.BaudRate = static_cast<DWORD>(config_.baud_rate);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;

        if (!SetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }

        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = 100;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.ReadTotalTimeoutConstant = config_.serial_timeout_s * 1000;
        timeouts.WriteTotalTimeoutConstant = config_.serial_timeout_s * 1000;
        SetCommTimeouts(serial_handle_, &timeouts);

        PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + std::strerror(errno) + ")",
                AlpacaError::NotConnected);
        }

        struct termios tty{};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to get serial port attributes", AlpacaError::DriverException);
        }

        speed_t baud = B9600;
        switch (config_.baud_rate) {
            case 9600:
                baud = B9600;
                break;
            case 19200:
                baud = B19200;
                break;
            case 38400:
                baud = B38400;
                break;
            case 57600:
                baud = B57600;
                break;
            case 115200:
                baud = B115200;
                break;
            default:
                baud = B9600;
                break;
        }
        cfsetospeed(&tty, baud);
        cfsetispeed(&tty, baud);

        // 8N1
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= CREAD | CLOCAL;
        tty.c_cflag &= ~HUPCL;  // Keep DTR high on close -- prevents MCU reset on reopen

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

        tty.c_oflag &= ~OPOST;
        tty.c_oflag &= ~ONLCR;

        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 10;  // 1s per-character timeout; read_response() has its own timeout

        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to set serial port attributes", AlpacaError::DriverException);
        }

        if (!util::clear_nonblocking(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to set serial port to blocking mode", AlpacaError::DriverException);
        }

        tcflush(serial_fd_, TCIOFLUSH);
#endif
    }

    void disconnect_locked() {
        connected_ = false;
        close_serial();
    }

    void close_serial() {
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

    void write_data(const std::string& data) {
#ifdef _WIN32
        std::size_t total = 0;
        while (total < data.length()) {
            DWORD bytes_written = 0;
            if (!WriteFile(serial_handle_, data.c_str() + total, static_cast<DWORD>(data.length() - total),
                           &bytes_written, nullptr) ||
                bytes_written == 0) {
                throw AlpacaException("Serial write failed", AlpacaError::DriverException);
            }
            total += bytes_written;
        }
#else
        if (!util::write_all(serial_fd_, data.c_str(), data.length())) {
            throw AlpacaException("Write failed: " + std::string(std::strerror(errno)), AlpacaError::DriverException);
        }
#endif
    }

    std::string read_response() {
        std::string response;
        response.reserve(kMaxResponseLen);
        char ch = 0;

        int timeout_ms = config_.serial_timeout_s * 1000;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) {
                throw AlpacaException("Read timeout", AlpacaError::DriverException);
            }

            bool got_char = false;
#ifdef _WIN32
            DWORD bytes_read = 0;
            if (ReadFile(serial_handle_, &ch, 1, &bytes_read, nullptr) && bytes_read == 1) {
                got_char = true;
            }
#else
            ssize_t r = read(serial_fd_, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
            if (r == 1) {
                got_char = true;
            }
#endif

            if (got_char) {
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                response += ch;
                if (ch == '#') {
                    break;
                }
                if (response.length() >= kMaxResponseLen) {
                    throw AlpacaException("Response too long", AlpacaError::DriverException);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReadCharDelayMs));
            }
        }

        ALPACA_LOG_TRACE("Gemini", "Flat panel response: " + response);
        return response;
    }

    std::string send_command_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("Gemini", "Flat panel command: " + cmd);
        write_data(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(kCommandDelayMs));
        return read_response();
    }

    mutable std::mutex mutex_;
    FlatPanelConnectionConfig config_;
    bool connected_ = false;

#ifdef _WIN32
    HANDLE serial_handle_ = INVALID_HANDLE_VALUE;
#else
    int serial_fd_ = -1;
#endif
};

// --- GeminiFlatPanelProtocolWrapper public interface forwarding ---

GeminiFlatPanelProtocolWrapper::GeminiFlatPanelProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

GeminiFlatPanelProtocolWrapper::~GeminiFlatPanelProtocolWrapper() = default;

std::string GeminiFlatPanelProtocolWrapper::connect(const FlatPanelConnectionConfig& config) {
    return impl_->connect(config);
}

void GeminiFlatPanelProtocolWrapper::disconnect() { impl_->disconnect(); }

bool GeminiFlatPanelProtocolWrapper::is_connected() const { return impl_->is_connected(); }

bool GeminiFlatPanelProtocolWrapper::get_light_on() { return impl_->get_light_on(); }

int GeminiFlatPanelProtocolWrapper::get_brightness() { return impl_->get_brightness(); }

void GeminiFlatPanelProtocolWrapper::light_on() { impl_->light_on(); }

void GeminiFlatPanelProtocolWrapper::light_off() { impl_->light_off(); }

void GeminiFlatPanelProtocolWrapper::set_brightness(int value) { impl_->set_brightness(value); }

}  // namespace alpacacore::vendor::gemini

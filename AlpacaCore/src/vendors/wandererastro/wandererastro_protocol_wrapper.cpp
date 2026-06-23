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
#include <alpacacore/vendor/wandererastro/wandererastro_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
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

namespace alpacacore::vendor::wandererastro {

namespace {

constexpr char kModelPrefix[] = "WandererCoverV4";
constexpr int MAX_LINE_LEN = 256;

// Configure an already-open POSIX fd for 19200 8N1 raw I/O. HUPCL is cleared so
// DTR stays asserted on close: the CH340 adapter asserts DTR on open, which
// pulses the cover's MCU reset line; keeping DTR high across close means a
// subsequent reopen does not reset the controller again.
#ifndef _WIN32
bool configure_serial_fd(int fd) {
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        return false;
    }
    cfsetospeed(&tty, B19200);
    cfsetispeed(&tty, B19200);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~HUPCL;  // keep DTR high on close — avoids CH340 MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;  // 0.5s per-read timeout so the reader loop can poll its stop flag
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        return false;
    }
    // Clear O_NONBLOCK so the reader's ::read() blocks for the VTIME window
    // instead of returning EAGAIN immediately. If either fcntl fails, report a
    // configuration failure rather than risk leaving the fd non-blocking (which
    // would spin the reader loop at 100% CPU).
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return false;
    }
    tcflush(fd, TCIOFLUSH);
    return true;
}
#endif

// Parse one 'A'-delimited status line into a WandererStatus. Returns false if
// the line does not look like a WandererCover status frame.
bool parse_status_line(const std::string& line, WandererStatus& out) {
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, 'A')) {
        // Keep empty tokens so every field stays at a fixed index. A malformed
        // frame with a blank field is then rejected by the numeric parse below
        // (std::stod/std::stoi throw on "") rather than silently shifting later
        // fields left — which could transpose close/open positions and report
        // the wrong cover state. A well-formed frame's trailing 'A' yields one
        // trailing empty token past the real fields, which is simply ignored.
        tokens.push_back(tok);
    }
    // Need at least model + firmware + the three cover angles to be useful.
    if (tokens.size() < 5) {
        return false;
    }
    if (tokens[0].rfind(kModelPrefix, 0) != 0) {
        return false;
    }

    WandererStatus s;
    s.model = tokens[0];
    try {
        s.firmware_version = std::stoi(tokens[1]);
        s.close_position = std::stod(tokens[2]);
        s.open_position = std::stod(tokens[3]);
        s.current_position = std::stod(tokens[4]);
        if (tokens.size() > 5) s.voltage = std::stod(tokens[5]);
        if (tokens.size() > 6) s.brightness = std::stoi(tokens[6]);
        if (tokens.size() > 7) s.dew_heater = std::stoi(tokens[7]);
        if (tokens.size() > 8) s.asiair_control = (std::stoi(tokens[8]) != 0);
    } catch (const std::exception&) {
        return false;  // malformed numeric field — ignore this frame
    }
    s.valid = true;
    out = s;
    return true;
}

// Open a candidate port, listen for up to ~2.5s for a streamed status frame,
// and report the model/firmware if one identifies a WandererCover. Returns true
// on a positive identification.
bool probe_port(const std::string& port_path, WandererPortInfo& info) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    if (!configure_serial_fd(fd)) {
        close(fd);
        return false;
    }

    std::string buffer;
    WandererStatus status;
    bool found = false;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() <
           2500) {
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            if (ch == '\n' || ch == '\r') {
                if (parse_status_line(buffer, status)) {
                    found = true;
                    break;
                }
                buffer.clear();
            } else {
                buffer += ch;
                if (buffer.size() > MAX_LINE_LEN) buffer.clear();
            }
        } else if (r < 0 && errno != EINTR) {
            // Persistent read error (e.g. the device was unplugged mid-probe):
            // VTIME rate-limits only the no-data path, not errors, so back off
            // to avoid spinning a CPU core for the rest of the probe window.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Re-apply HUPCL-clear before closing so DTR stays asserted.
    struct termios tty {};
    if (tcgetattr(fd, &tty) == 0) {
        tty.c_cflag &= ~HUPCL;
        tcsetattr(fd, TCSANOW, &tty);
    }
    close(fd);

    if (found) {
        info.port_path = port_path;
        info.model = status.model;
        info.firmware_version = status.firmware_version;
        return true;
    }
#else
    (void)port_path;
    (void)info;
#endif
    return false;
}

}  // namespace

std::vector<WandererPortInfo> enumerate_wanderer_ports() {
    std::vector<WandererPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (!std::filesystem::exists(port)) continue;
            ALPACA_LOG_INFO("WandererAstro", "Probing " + port + "...");
            WandererPortInfo info;
            if (probe_port(port, info)) {
                ALPACA_LOG_INFO("WandererAstro", "Found " + info.model + " on " + port + " (firmware " +
                                                     std::to_string(info.firmware_version) + ")");
                results.push_back(info);
            }
        }
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(serial_by_id)) {
        if (!entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();

        // WandererCover boards use a CH340/CH341 USB-serial adapter (vendor 1a86).
        bool is_candidate = (name.find("USB_Serial") != std::string::npos) ||
                            (name.find("CH340") != std::string::npos) || (name.find("CH341") != std::string::npos) ||
                            (name.find("1a86") != std::string::npos);
        if (!is_candidate) continue;

        // Use the error_code overload: if the device is unplugged between the
        // directory scan and here the by-id symlink dangles, and the throwing
        // canonical() would abort the whole enumeration. Skip the stale entry.
        std::error_code ec;
        std::string resolved = std::filesystem::canonical(entry.path(), ec).string();
        if (ec) continue;
        std::string probe_msg = "Probing ";
        probe_msg.append(resolved).append(" (").append(name).append(")...");
        ALPACA_LOG_INFO("WandererAstro", probe_msg);

        WandererPortInfo info;
        if (probe_port(resolved, info)) {
            info.device_id = name;
            ALPACA_LOG_INFO("WandererAstro", "Found " + info.model + " on " + resolved + " (firmware " +
                                                 std::to_string(info.firmware_version) + ")");
            results.push_back(info);
        }
    }
#endif

    return results;
}

class WandererProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    std::string connect(const ConnectionConfig& config) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Precondition: connect() requires a disconnected wrapper. While a
            // session is live the reader thread is joinable; re-spawning it
            // would overwrite a joinable std::thread and call std::terminate().
            // Every connect() failure path joins the reader before returning, so
            // connected_ == false guarantees no live thread — making this the
            // single, sufficient guard. Fail fast so any future caller that
            // relaxes the driver-layer serialization gets a loud error, never a
            // process abort.
            if (connected_) {
                throw AlpacaException("WandererCover already connected; call disconnect() first",
                                      AlpacaError::InvalidOperation);
            }
            config_ = config;
            open_serial();
        }

        // Start the background reader that keeps the latest streamed status.
        running_.store(true);
        reader_thread_ = std::thread([this] { reader_loop(); });

        // Wait for the first valid frame that identifies a WandererCover.
        auto start = std::chrono::steady_clock::now();
        const int timeout_ms = config.serial_timeout_s * 1000;
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() <
               timeout_ms) {
            WandererStatus s = get_status();
            if (s.valid && s.model.rfind(kModelPrefix, 0) == 0) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    model_ = s.model;
                    connected_ = true;
                }
                ALPACA_LOG_INFO("WandererAstro",
                                "Connected to " + s.model + " (firmware " + std::to_string(s.firmware_version) + ")");
                return s.model;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // No status stream — not a WandererCover (or wrong port).
        stop_reader();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_serial();
        }
        throw AlpacaException("No WandererCover status detected on " + config.serial_port, AlpacaError::NotConnected);
    }

    void disconnect() {
        stop_reader();
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        close_serial();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    WandererStatus get_status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_;
    }

    void open_cover() { send_command("1001"); }
    void close_cover() { send_command("1000"); }
    void turn_off_light() { send_command("9999"); }

    void set_brightness(int brightness) {
        if (brightness <= 0) {
            send_command("9999");
        } else {
            send_command(std::to_string(brightness));
        }
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("WandererCover not connected", AlpacaError::NotConnected);
        }
    }

    void send_command(const std::string& code) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const std::string payload = code + "\n";
        ALPACA_LOG_TRACE("WandererAstro", "Command: " + code);
        // Loop until the whole payload is written: a short write would drop the
        // trailing '\n', leaving the controller with a buffered partial command.
#ifdef _WIN32
        std::size_t total = 0;
        while (total < payload.size()) {
            DWORD written = 0;
            if (!WriteFile(serial_handle_, payload.data() + total, static_cast<DWORD>(payload.size() - total), &written,
                           nullptr)) {
                throw AlpacaException("Serial write failed", AlpacaError::DriverException);
            }
            total += written;
        }
#else
        std::size_t total = 0;
        while (total < payload.size()) {
            ssize_t written = ::write(serial_fd_, payload.data() + total, payload.size() - total);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;  // interrupted before any byte was written — retry
                }
                throw AlpacaException("Serial write failed: " + std::string(std::strerror(errno)),
                                      AlpacaError::DriverException);
            }
            total += static_cast<std::size_t>(written);
        }
#endif
    }

    void reader_loop() {
        std::string buffer;
        buffer.reserve(MAX_LINE_LEN);
        while (running_.load()) {
            char ch = 0;
            bool got = false;
#ifdef _WIN32
            HANDLE handle;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handle = serial_handle_;
            }
            if (handle == INVALID_HANDLE_VALUE) break;
            DWORD bytes_read = 0;
            if (ReadFile(handle, &ch, 1, &bytes_read, nullptr)) {
                if (bytes_read == 1) {
                    got = true;
                }
                // bytes_read == 0 is the ReadTotalTimeoutConstant window elapsing
                // with no data — naturally rate-limited, just loop.
            } else {
                // I/O error (e.g. device unplugged): back off so the loop doesn't
                // peg a CPU core until stop_reader() runs. Linux is rate-limited
                // by the VTIME read timeout and needs no equivalent.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
#else
            int fd;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                fd = serial_fd_;
            }
            if (fd < 0) break;
            ssize_t r = ::read(fd, &ch, 1);
            if (r == 1) {
                got = true;
            } else if (r < 0 && errno != EINTR) {
                // Persistent read error (device unplugged): VTIME rate-limits the
                // no-data path but not errors, so back off to avoid a CPU spin
                // until stop_reader() runs. (The Windows branch above does the
                // same on ReadFile failure.)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
#endif
            if (!got) {
                continue;  // VTIME timeout — loop and re-check running_
            }
            if (ch == '\n' || ch == '\r') {
                WandererStatus parsed;
                if (parse_status_line(buffer, parsed)) {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    status_ = parsed;
                }
                buffer.clear();
            } else {
                buffer += ch;
                if (buffer.size() > MAX_LINE_LEN) buffer.clear();
            }
        }
    }

    void stop_reader() {
        running_.store(false);
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
    }

    void open_serial() {
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
        if (!SetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }
        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 500;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        SetCommTimeouts(serial_handle_, &timeouts);
        PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + std::strerror(errno) + ")",
                AlpacaError::NotConnected);
        }
        if (!configure_serial_fd(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }
#endif
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

    mutable std::mutex mutex_;         // guards serial handle + connected_/config_
    mutable std::mutex status_mutex_;  // guards the latest status frame
    ConnectionConfig config_;
    std::string model_;
    bool connected_ = false;

    std::atomic<bool> running_{false};
    std::thread reader_thread_;
    WandererStatus status_;

#ifdef _WIN32
    HANDLE serial_handle_ = INVALID_HANDLE_VALUE;
#else
    int serial_fd_ = -1;
#endif
};

// --- WandererProtocolWrapper public interface forwarding ---

WandererProtocolWrapper::WandererProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

WandererProtocolWrapper::~WandererProtocolWrapper() = default;

std::string WandererProtocolWrapper::connect(const ConnectionConfig& config) { return impl_->connect(config); }

void WandererProtocolWrapper::disconnect() { impl_->disconnect(); }

bool WandererProtocolWrapper::is_connected() const { return impl_->is_connected(); }

WandererStatus WandererProtocolWrapper::get_status() const { return impl_->get_status(); }

void WandererProtocolWrapper::open_cover() { impl_->open_cover(); }

void WandererProtocolWrapper::close_cover() { impl_->close_cover(); }

void WandererProtocolWrapper::set_brightness(int brightness) { impl_->set_brightness(brightness); }

void WandererProtocolWrapper::turn_off_light() { impl_->turn_off_light(); }

}  // namespace alpacacore::vendor::wandererastro

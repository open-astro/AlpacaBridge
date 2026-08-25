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
#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#endif

namespace alpacacore::vendor::gemini {

static constexpr int COMMAND_DELAY_MS = 50;
static constexpr int READ_CHAR_DELAY_MS = 10;
static constexpr int MAX_RESPONSE_LEN = 32;
static constexpr int HANDSHAKE_RETRIES = 3;
static constexpr int HANDSHAKE_RETRY_DELAY_S = 1;
static constexpr int HANDSHAKE_READ_TIMEOUT_S = 2;  // Shorter timeout during handshake

// Probe a serial port with the MyFP2 firmware version handshake.
// Returns firmware version on success, 0 on failure.
//
// The CH340 USB-serial adapter asserts DTR on open, which resets the
// focuser's microcontroller (Arduino/ESP32) via the rising edge.
// We wait for the MCU to boot, then handshake.  Before closing we
// clear HUPCL so DTR stays high — this way the subsequent driver
// connect() reopens without a DTR edge and the MCU doesn't reset again.
static int probe_port(const std::string& port_path) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return 0;
    }

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~HUPCL;   // Keep DTR high on close — prevents MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 50; // 5-second per-character timeout (matches INDI)

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return 0;
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return 0;
    }
    tcflush(fd, TCIOFLUSH);

    // Wait for MCU to boot after the DTR reset, then try handshake.
    // Two attempts: first waits 2s for boot, second is a quick retry.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 1));

        tcflush(fd, TCIOFLUSH);

        const char* cmd = ":03#";
        if (!util::write_all(fd, cmd, 4)) {
            continue;
        }

        // Read response — INDI reads exactly 5 bytes with a 5s timeout
        char resp[6] = {};
        int total = 0;
        auto start = std::chrono::steady_clock::now();
        while (total < 5) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
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
                continue; // VTIME timeout, keep trying
            } else {
                break;
            }
        }

        int firmware = 0;
        if (std::sscanf(resp, "F%d#", &firmware) > 0 && firmware > 0) {
            // Re-apply HUPCL-clear before closing so DTR stays asserted
            tcgetattr(fd, &tty);
            tty.c_cflag &= ~HUPCL;
            tcsetattr(fd, TCSANOW, &tty);
            close(fd);
            return firmware;
        }

        ALPACA_LOG_DEBUG("Gemini", "Probe attempt " + std::to_string(attempt + 1) +
                         " got no valid response from " + port_path);
    }

    close(fd);
#else
    (void)port_path;
#endif
    return 0;
}

#ifndef _WIN32
namespace {
// The raw /dev/ttyUSBn fallback below has no by-id symlink name to filter
// on, so without this check it would open EVERY serial device on the box --
// unrelated FTDI dongles, GPS receivers, or a mount controller sharing the
// ttyUSB namespace -- not just CH340/CH341-class focuser hardware. Filters
// the raw fallback exactly as narrowly as the by-id loop below.
bool raw_port_looks_like_focuser_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor, {"1a86", "CH340", "CH341", "USB_Serial"});
}
}  // namespace
#endif

std::vector<GeminiPortInfo> enumerate_gemini_ports() {
    std::vector<GeminiPortInfo> results;

#ifndef _WIN32
    // Resolved (canonical) paths already probed via by-id, so the raw-node
    // pass below never re-opens a port the by-id pass already tried.
    std::set<std::string> probed;

    struct Candidate {
        std::string path;
        std::string name;  // by-id symlink name, empty for raw nodes
    };
    std::vector<Candidate> candidates;

    // Scan /dev/serial/by-id/ for USB-serial adapters
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;

            // CH340/CH341 USB-serial adapters commonly used by MyFocuserPro2 boards
            // Also accept any USB_Serial device as a candidate
            bool is_candidate = (name.find("USB_Serial") != std::string::npos) ||
                                (name.find("CH340") != std::string::npos) ||
                                (name.find("CH341") != std::string::npos) || (name.find("1a86") != std::string::npos);
            if (!is_candidate) continue;

            // error_code overload: a device unplugged after the directory scan
            // leaves a dangling symlink; skip it instead of aborting the
            // enumeration.
            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            candidates.push_back({resolved, name});
        }
    }

    // Always also probe raw /dev/ttyUSB* nodes directly, not only when
    // /dev/serial/by-id is absent. Generic CH340/CH341 adapters (used by
    // this focuser, and commonly also by mount controllers on the same box)
    // report identical descriptor strings with no per-device serial number,
    // so when two such adapters are plugged in at once, udev's by-id naming
    // collides and only ONE of them gets a symlink -- the other is silently
    // absent from by-id even though the directory itself exists, so the loop
    // above never sees it. Deduplicated by resolved canonical path so a port
    // already tried via by-id isn't opened (and reset) a second time.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;

        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_focuser_candidate(resolved)) continue;
        probed.insert(resolved);
        candidates.push_back({resolved, ""});
    }

    // Probe every candidate concurrently: a non-responsive port costs the full
    // handshake timeout, so serial probing scaled linearly with adapter count
    // (issue #218); one thread per port bounds the scan to one port's worst case.
    std::vector<int> firmware(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        ALPACA_LOG_INFO("Gemini", "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") + "...");
        workers.emplace_back([&, i] { firmware[i] = probe_port(candidates[i].path); });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (firmware[i] <= 0) continue;
        const auto& c = candidates[i];
        ALPACA_LOG_INFO("Gemini", "Found focuser on " + c.path + " (firmware " + std::to_string(firmware[i]) + ")");
        results.push_back({c.path, c.name, firmware[i]});
    }
#endif

    return results;
}

class GeminiProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() {
        disconnect();
    }

    int connect(const ConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;

        connect_serial();

        // Handshake: request firmware version with retries.
        //
        // Use a shorter read timeout during handshake so the total connect
        // time stays under NINA/ASCOM's ~10s client timeout.
        //   Attempt 0: 100ms wait + 2s read  = 2.1s max (MCU already running)
        //   Attempt 1: 2s wait   + 2s read  = 4.0s max (MCU boot after DTR reset)
        //   Attempt 2: 1s wait   + 2s read  = 3.0s max (retry)
        //   Total worst case: ~9.1s
        int firmware = 0;
        bool success = false;
        int saved_timeout = config_.serial_timeout_s;
        config_.serial_timeout_s = HANDSHAKE_READ_TIMEOUT_S;

        for (int attempt = 0; attempt < HANDSHAKE_RETRIES && !success; ++attempt) {
            if (attempt == 0) {
                // Quick attempt — MCU likely already running after probe
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else if (attempt == 1) {
                // MCU may have reset via DTR — wait for full boot
                std::this_thread::sleep_for(std::chrono::seconds(2));
            } else {
                // Final retry
                std::this_thread::sleep_for(std::chrono::seconds(HANDSHAKE_RETRY_DELAY_S));
            }

            if (config_.type == ConnectionType::Serial) {
#ifndef _WIN32
                tcflush(serial_fd_, TCIOFLUSH);
#else
                PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#endif
            }

            try {
                std::string resp = send_command_locked(":03#");
                if (std::sscanf(resp.c_str(), "F%d#", &firmware) > 0) {
                    success = true;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini", "Handshake attempt " + std::to_string(attempt + 1) +
                                " failed: " + e.what());
            }
        }

        config_.serial_timeout_s = saved_timeout;

        if (!success) {
            disconnect_locked();
            throw AlpacaException("Gemini focuser handshake failed after " +
                                  std::to_string(HANDSHAKE_RETRIES) + " attempts",
                                  AlpacaError::NotConnected);
        }

        // Set temperature reporting to Celsius (after handshake succeeds).
        // This is a blind command (no response), so use send_command_blind_locked.
        // Using send_command_locked here would block for the full serial timeout
        // waiting for a response that never arrives, and any late MCU output can
        // desynchronize subsequent reads.
        try {
            send_command_blind_locked(":16#");
            std::this_thread::sleep_for(std::chrono::milliseconds(COMMAND_DELAY_MS));
        } catch (const std::exception&) {
            // Non-fatal — some firmware versions may not support this
        }

        // Flush any stale data before sending the speed command so it
        // arrives cleanly (the MCU may echo or ACK the temperature command).
#ifndef _WIN32
        tcflush(serial_fd_, TCIOFLUSH);
#else
        PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#endif

        // Set motor speed to fast so full-range moves complete within
        // ASCOM ConformU's 60-second timeout
        try {
            send_command_blind_locked(":1502#");
            std::this_thread::sleep_for(std::chrono::milliseconds(COMMAND_DELAY_MS));
        } catch (const std::exception&) {
        }

        connected_ = true;
        ALPACA_LOG_INFO("Gemini", "Connected, firmware version: " + std::to_string(firmware));
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

    int get_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":00#");
        int pos = 0;
        if (std::sscanf(resp.c_str(), "%*c%d#", &pos) <= 0) {
            throw AlpacaException("Failed to parse position: " + resp,
                                  AlpacaError::DriverException);
        }
        return pos;
    }

    bool is_moving() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":01#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "I%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse isMoving: " + resp,
                                  AlpacaError::DriverException);
        }
        return val != 0;
    }

    int get_firmware_version() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":03#");
        int ver = 0;
        if (std::sscanf(resp.c_str(), "F%d#", &ver) <= 0) {
            throw AlpacaException("Failed to parse firmware version: " + resp,
                                  AlpacaError::DriverException);
        }
        return ver;
    }

    double get_temperature() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":06#");
        double temp = 0.0;
        if (std::sscanf(resp.c_str(), "Z%lf#", &temp) <= 0) {
            throw AlpacaException("Failed to parse temperature: " + resp,
                                  AlpacaError::DriverException);
        }
        return temp;
    }

    int get_max_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":08#");
        int maxpos = 0;
        if (std::sscanf(resp.c_str(), "M%d#", &maxpos) <= 0) {
            throw AlpacaException("Failed to parse max position: " + resp,
                                  AlpacaError::DriverException);
        }
        return maxpos;
    }

    bool get_coil_power() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":11#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "O%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse coil power: " + resp,
                                  AlpacaError::DriverException);
        }
        return val != 0;
    }

    bool get_reverse_direction() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":13#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "R%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse reverse direction: " + resp,
                                  AlpacaError::DriverException);
        }
        return val != 0;
    }

    bool get_temp_comp_enabled() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":24#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "1%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse temp comp: " + resp,
                                  AlpacaError::DriverException);
        }
        return val != 0;
    }

    int get_temp_coefficient() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":26#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "B%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse temp coefficient: " + resp,
                                  AlpacaError::DriverException);
        }
        return val;
    }

    int get_step_mode() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":29#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "S%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse step mode: " + resp,
                                  AlpacaError::DriverException);
        }
        return val;
    }

    int get_speed() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":43#");
        int val = 0;
        if (std::sscanf(resp.c_str(), "C%d#", &val) <= 0) {
            throw AlpacaException("Failed to parse speed: " + resp,
                                  AlpacaError::DriverException);
        }
        return val;
    }

    void move_to(int position) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":05%d#", position);
        send_command_blind_locked(cmd);
    }

    void set_max_position(int max_pos) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":07%06d#", max_pos);
        send_command_blind_locked(cmd);
    }

    void set_coil_power(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":12%d#", enabled ? 1 : 0);
        send_command_blind_locked(cmd);
    }

    void set_reverse_direction(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":14%d#", enabled ? 1 : 0);
        send_command_blind_locked(cmd);
    }

    void set_temperature_celsius() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":16#");
    }

    void set_temp_coefficient(int coefficient) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":22%d#", coefficient);
        send_command_blind_locked(cmd);
    }

    void set_temp_comp_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":23%c#", enabled ? '1' : '0');
        send_command_blind_locked(cmd);
    }

    void halt() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":27#");
    }

    void goto_home() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":28#");
    }

    void set_step_mode(int mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":30%d#", mode);
        send_command_blind_locked(cmd);
    }

    void sync_position(int position) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":31%u#", static_cast<unsigned>(position));
        send_command_blind_locked(cmd);
    }

    void set_speed(int speed) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[MAX_RESPONSE_LEN];
        std::snprintf(cmd, sizeof(cmd), ":150%d#", speed);
        send_command_blind_locked(cmd);
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Gemini focuser not connected", AlpacaError::NotConnected);
        }
    }

    void connect_serial() {
#ifdef _WIN32
        std::string port_name = "\\\\.\\" + config_.serial_port;
        serial_handle_ = CreateFileA(port_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (serial_handle_ == INVALID_HANDLE_VALUE) {
            throw AlpacaException("Failed to open serial port: " + config_.serial_port,
                                  AlpacaError::NotConnected);
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
            throw AlpacaException("Failed to open serial port: " + config_.serial_port +
                                  " (" + std::strerror(errno) + ")",
                                  AlpacaError::NotConnected);
        }

        struct termios tty{};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to get serial port attributes",
                                  AlpacaError::DriverException);
        }

        speed_t baud = B9600;
        switch (config_.baud_rate) {
            case 9600:   baud = B9600;   break;
            case 19200:  baud = B19200;  break;
            case 38400:  baud = B38400;  break;
            case 57600:  baud = B57600;  break;
            case 115200: baud = B115200; break;
            default:     baud = B9600;   break;
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
        tty.c_cflag &= ~HUPCL;   // Keep DTR high on close — prevents MCU reset on reopen

        // Raw input
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

        tty.c_oflag &= ~OPOST;
        tty.c_oflag &= ~ONLCR;

        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 10; // 1s per-character timeout; read_response() has its own timeout

        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to set serial port attributes",
                                  AlpacaError::DriverException);
        }

        // Switch to blocking mode
        if (!util::clear_nonblocking(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to set serial port to blocking mode", AlpacaError::DriverException);
        }

        tcflush(serial_fd_, TCIOFLUSH);
#endif
        // Serial connection established
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
        // Loop until the whole payload is written and check bytes_written: a
        // short write that still returns TRUE would otherwise drop trailing bytes
        // (mirrors the POSIX write_all path below).
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
            throw AlpacaException("Write failed: " + std::string(std::strerror(errno)),
                                  AlpacaError::DriverException);
        }
#endif
    }

    std::string read_response() {
        std::string response;
        response.reserve(MAX_RESPONSE_LEN);
        char ch = 0;

        int timeout_ms = config_.serial_timeout_s * 1000;

        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
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
            ssize_t r = read(serial_fd_, &ch, 1);
            if (r == 1) {
                got_char = true;
            }
#endif

            if (got_char) {
                // Ignore CR/LF
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                response += ch;
                if (ch == '#') {
                    break;
                }
                if (response.length() >= MAX_RESPONSE_LEN) {
                    throw AlpacaException("Response too long", AlpacaError::DriverException);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(READ_CHAR_DELAY_MS));
            }
        }

        ALPACA_LOG_TRACE("Gemini", "Response: " + response);
        return response;
    }

    std::string send_command_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("Gemini", "Command: " + cmd);
        write_data(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(COMMAND_DELAY_MS));
        return read_response();
    }

    void send_command_blind_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("Gemini", "Command (blind): " + cmd);
        write_data(cmd);
    }

    mutable std::mutex mutex_;
    ConnectionConfig config_;
    bool connected_ = false;

#ifdef _WIN32
    HANDLE serial_handle_ = INVALID_HANDLE_VALUE;
#else
    int serial_fd_ = -1;
#endif
};

// --- GeminiProtocolWrapper public interface forwarding ---

GeminiProtocolWrapper::GeminiProtocolWrapper()
    : impl_(std::make_unique<Impl>()) {}

GeminiProtocolWrapper::~GeminiProtocolWrapper() = default;

int GeminiProtocolWrapper::connect(const ConnectionConfig& config) {
    return impl_->connect(config);
}

void GeminiProtocolWrapper::disconnect() {
    impl_->disconnect();
}

bool GeminiProtocolWrapper::is_connected() const {
    return impl_->is_connected();
}

int GeminiProtocolWrapper::get_position() {
    return impl_->get_position();
}

bool GeminiProtocolWrapper::is_moving() {
    return impl_->is_moving();
}

int GeminiProtocolWrapper::get_firmware_version() {
    return impl_->get_firmware_version();
}

double GeminiProtocolWrapper::get_temperature() {
    return impl_->get_temperature();
}

int GeminiProtocolWrapper::get_max_position() {
    return impl_->get_max_position();
}

bool GeminiProtocolWrapper::get_coil_power() {
    return impl_->get_coil_power();
}

bool GeminiProtocolWrapper::get_reverse_direction() {
    return impl_->get_reverse_direction();
}

bool GeminiProtocolWrapper::get_temp_comp_enabled() {
    return impl_->get_temp_comp_enabled();
}

int GeminiProtocolWrapper::get_temp_coefficient() {
    return impl_->get_temp_coefficient();
}

int GeminiProtocolWrapper::get_step_mode() {
    return impl_->get_step_mode();
}

int GeminiProtocolWrapper::get_speed() {
    return impl_->get_speed();
}

void GeminiProtocolWrapper::move_to(int position) {
    impl_->move_to(position);
}

void GeminiProtocolWrapper::set_max_position(int max_pos) {
    impl_->set_max_position(max_pos);
}

void GeminiProtocolWrapper::set_coil_power(bool enabled) {
    impl_->set_coil_power(enabled);
}

void GeminiProtocolWrapper::set_reverse_direction(bool enabled) {
    impl_->set_reverse_direction(enabled);
}

void GeminiProtocolWrapper::set_temperature_celsius() {
    impl_->set_temperature_celsius();
}

void GeminiProtocolWrapper::set_temp_coefficient(int coefficient) {
    impl_->set_temp_coefficient(coefficient);
}

void GeminiProtocolWrapper::set_temp_comp_enabled(bool enabled) {
    impl_->set_temp_comp_enabled(enabled);
}

void GeminiProtocolWrapper::halt() {
    impl_->halt();
}

void GeminiProtocolWrapper::goto_home() {
    impl_->goto_home();
}

void GeminiProtocolWrapper::set_step_mode(int mode) {
    impl_->set_step_mode(mode);
}

void GeminiProtocolWrapper::sync_position(int position) {
    impl_->sync_position(position);
}

void GeminiProtocolWrapper::set_speed(int speed) {
    impl_->set_speed(speed);
}

} // namespace alpacacore::vendor::gemini

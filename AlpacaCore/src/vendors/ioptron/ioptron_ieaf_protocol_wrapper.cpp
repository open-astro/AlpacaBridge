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
#include <alpacacore/vendor/ioptron/ioptron_ieaf_protocol_wrapper.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::ioptron {

namespace {

constexpr int COMMAND_DELAY_MS =
    5;  // read_response() waits for the # terminator; no need to pre-sleep a full reply time
constexpr int READ_CHAR_DELAY_MS = 10;
constexpr int ACK_TIMEOUT_MS = 300;
constexpr int MAX_RESPONSE_LEN = 32;
constexpr int HANDSHAKE_RETRIES = 3;
constexpr int HANDSHAKE_READ_TIMEOUT_S = 2;        // Shorter than the 4s runtime timeout (INDI iEAFFOCUS_TIMEOUT)
constexpr std::int32_t IEAF_MAX_POSITION = 99999;  // matches INDI FocusAbsPos max

bool parse_device_info(const std::string& resp, IeafDeviceInfo& out) {
    // ":DeviceInfo#" → "%6d%2d%4d#": position, model code, firmware/build.
    int pos = 0, model = 0, fw = 0;
    if (std::sscanf(resp.c_str(), "%6d%2d%4d", &pos, &model, &fw) != 3) {
        return false;
    }
    out.position = pos;
    out.model = model;
    out.firmware = fw;
    return true;
}

bool is_ieaf_model(std::int32_t model) {
    // Model codes 2 and 3 identify the iEAF (INDI ieaffocus.cpp).
    return model == 2 || model == 3;
}

}  // namespace

// Probe a serial port with the :DeviceInfo# handshake. Returns true (and
// fills `info`) only if the reply parses and reports an iEAF model code.
//
// The iEAF ships with a Prolific PL2303 USB-serial bridge (067b:23d3) wired
// straight to the focuser MCU — no Arduino-style DTR reset on open, so no
// CH340 HUPCL dance is needed here.
static bool probe_port(const std::string& port_path, IeafDeviceInfo& info) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }

    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return false;
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
    tty.c_cc[VTIME] = 10;  // 1s per-character timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return false;
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        // First attempt is quick; second waits out any port-open settling.
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 1000));
        tcflush(fd, TCIOFLUSH);

        const char* cmd = ":DeviceInfo#";
        if (!util::write_all(fd, cmd, std::strlen(cmd))) {
            continue;
        }

        std::string resp;
        auto start = std::chrono::steady_clock::now();
        while (resp.length() < MAX_RESPONSE_LEN) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > HANDSHAKE_READ_TIMEOUT_S) {
                break;
            }
            char ch = 0;
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                if (ch == '\r' || ch == '\n') continue;
                if (ch == '#') break;
                resp += ch;
            } else if (r != 0) {
                break;
            }
        }

        IeafDeviceInfo parsed;
        if (parse_device_info(resp, parsed) && is_ieaf_model(parsed.model)) {
            close(fd);
            info = parsed;
            return true;
        }
    }

    close(fd);
#else
    (void)port_path;
    (void)info;
#endif
    return false;
}

#ifndef _WIN32
namespace {
// The raw /dev/ttyUSBn fallback has no by-id symlink name to filter on;
// restrict it to Prolific PL2303-class adapters (the bridge the iEAF ships
// with) so the scan doesn't open every unrelated serial device on the box.
// Note the iOptron mount USB port also enumerates as a Prolific adapter — the
// :DeviceInfo# probe's model-code check is what tells them apart.
bool raw_port_looks_like_ieaf_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor,
                                                        {"067b", "Prolific", "PL2303", "USB-Serial", "USB_Serial"});
}
}  // namespace
#endif

std::vector<IeafPortInfo> enumerate_ieaf_ports() {
    std::vector<IeafPortInfo> results;

#ifndef _WIN32
    // Canonical paths already probed via by-id, so the raw-node pass below
    // never re-opens a port the by-id pass already tried.
    std::set<std::string> probed;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;

            // Prolific PL2303 USB-serial bridge (067b:23d3) used by the iEAF.
            bool is_candidate =
                (name.find("Prolific") != std::string::npos) || (name.find("PL2303") != std::string::npos) ||
                (name.find("067b") != std::string::npos) || (name.find("USB-Serial") != std::string::npos) ||
                (name.find("USB_Serial") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            std::string probe_msg = "Probing ";
            probe_msg += resolved;
            probe_msg += " (";
            probe_msg += name;
            probe_msg += ") for iEAF...";
            ALPACA_LOG_INFO("iOptron", probe_msg);

            IeafDeviceInfo info;
            if (probe_port(resolved, info)) {
                ALPACA_LOG_INFO("iOptron", "Found iEAF on " + resolved + " (model " + std::to_string(info.model) +
                                               ", firmware " + std::to_string(info.firmware) + ")");
                results.push_back({resolved, name, info});
            }
        }
    }

    // Always also probe raw /dev/ttyUSB* nodes: generic Prolific adapters with
    // no per-device serial number collide in udev's by-id naming, so a second
    // adapter can be silently absent from by-id even when the directory exists.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;

        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_ieaf_candidate(resolved)) continue;
        probed.insert(resolved);

        ALPACA_LOG_INFO("iOptron", "Probing " + resolved + " for iEAF...");
        IeafDeviceInfo info;
        if (probe_port(resolved, info)) {
            ALPACA_LOG_INFO("iOptron", "Found iEAF on " + resolved + " (model " + std::to_string(info.model) +
                                           ", firmware " + std::to_string(info.firmware) + ")");
            results.push_back({resolved, "", info});
        }
    }
#endif

    return results;
}

class IeafProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    IeafDeviceInfo connect(const IeafConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;

        connect_serial();

        // Handshake with retries, using a shorter read timeout so the total
        // connect time stays well under ASCOM clients' ~10s budget.
        IeafDeviceInfo info;
        bool success = false;
        int saved_timeout = config_.serial_timeout_s;
        config_.serial_timeout_s = HANDSHAKE_READ_TIMEOUT_S;

        for (int attempt = 0; attempt < HANDSHAKE_RETRIES && !success; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 1000));
            tcflush_port();

            try {
                std::string resp = send_command_locked(":DeviceInfo#");
                if (parse_device_info(resp, info)) {
                    success = true;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron",
                                "iEAF handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }

        config_.serial_timeout_s = saved_timeout;

        if (!success) {
            disconnect_locked();
            throw AlpacaException("iEAF handshake failed after " + std::to_string(HANDSHAKE_RETRIES) + " attempts",
                                  AlpacaError::NotConnected);
        }
        if (!is_ieaf_model(info.model)) {
            disconnect_locked();
            throw AlpacaException("Device on " + config_.serial_port + " reported model code " +
                                      std::to_string(info.model) + ", not an iEAF (expected 2 or 3)",
                                  AlpacaError::NotConnected);
        }

        connected_ = true;
        ALPACA_LOG_INFO("iOptron", "iEAF connected (model " + std::to_string(info.model) + ", firmware " +
                                       std::to_string(info.firmware) + ")");
        return info;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    IeafStatus get_status() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":FI#");
        // "%7d%1d%5d%1d#": position, moving flag, temperature (Kelvin x 100),
        // direction flag (0 = reversed).
        int pos = 0, moving = 0, temp = 0, dir = 0;
        if (std::sscanf(resp.c_str(), "%7d%1d%5d%1d", &pos, &moving, &temp, &dir) != 4) {
            throw AlpacaException("Failed to parse iEAF status: " + resp, AlpacaError::DriverException);
        }
        IeafStatus status;
        status.position = pos;
        status.moving = moving != 0;
        status.temperature_c = temp / 100.0 - 273.15;
        status.reversed = dir == 0;
        return status;
    }

    void move_to(std::int32_t position) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (position < 0 || position > IEAF_MAX_POSITION) {
            throw AlpacaException("iEAF target position out of range", AlpacaError::InvalidValue);
        }
        char cmd[16];
        // Space-padded 7-wide, matching INDI's ":FM%7u#".
        std::snprintf(cmd, sizeof(cmd), ":FM%7u#", static_cast<unsigned>(position));
        send_command_blind_locked(cmd);
    }

    void halt() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":FQ#");
    }

    void set_zero() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":FZ#");
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("iEAF not connected", AlpacaError::NotConnected);
        }
    }

    void connect_serial() {
#ifndef _WIN32
        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + std::strerror(errno) + ")",
                AlpacaError::NotConnected);
        }

        struct termios tty {};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to get serial port attributes", AlpacaError::DriverException);
        }

        // The iEAF runs at a fixed 115200 regardless of config_.baud_rate.
        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);

        // 8N1, raw
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
#else
        throw AlpacaException("iEAF driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    void disconnect_locked() {
        connected_ = false;
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
#endif
    }

    void tcflush_port() {
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            tcflush(serial_fd_, TCIOFLUSH);
        }
#endif
    }

    void write_data(const std::string& data) {
#ifndef _WIN32
        if (!util::write_all(serial_fd_, data.c_str(), data.length())) {
            throw AlpacaException("Write failed: " + std::string(std::strerror(errno)), AlpacaError::DriverException);
        }
#else
        (void)data;
#endif
    }

    std::string read_response() {
        std::string response;
        response.reserve(MAX_RESPONSE_LEN);

        int timeout_ms = config_.serial_timeout_s * 1000;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) {
                throw AlpacaException("iEAF read timeout", AlpacaError::DriverException);
            }

            char ch = 0;
            bool got_char = false;
#ifndef _WIN32
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
                if (response.length() >= MAX_RESPONSE_LEN) {
                    throw AlpacaException("iEAF response too long", AlpacaError::DriverException);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(READ_CHAR_DELAY_MS));
            }
        }

        ALPACA_LOG_TRACE("iOptron", "iEAF response: " + response);
        return response;
    }

    // Flush stale input before every command: a previous blind command or a
    // half-read reply must not shift the fixed-width field parse of the next
    // response (same leaked-byte hazard the mount driver hit — see AGENTS.md).
    std::string send_command_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iEAF command: " + cmd);
        tcflush_port();
        write_data(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(COMMAND_DELAY_MS));
        return read_response();
    }

    // :FM / :FQ / :FZ answer with a single '1' byte and no '#' terminator
    // (INDI ieaffocus.cpp reads exactly one byte after each). Consume it
    // here: leaving it in the input buffer races the tcflush at the start
    // of the next :FI# — when it lands after the flush it prefixes the reply
    // ("1+013211...") and breaks the fixed-width parse. The ack is optional:
    // a firmware that stays silent just costs us ACK_TIMEOUT_MS.
    void send_command_blind_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iEAF command (blind): " + cmd);
        tcflush_port();
        write_data(cmd);
        auto start = std::chrono::steady_clock::now();
        while (true) {
            char ch = 0;
#ifndef _WIN32
            if (read(serial_fd_, &ch, 1) == 1) {  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                if (ch != '1') {
                    ALPACA_LOG_WARN("iOptron", std::string("iEAF unexpected ack byte: ") + ch);
                }
                return;
            }
#endif
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > ACK_TIMEOUT_MS) {
                ALPACA_LOG_DEBUG("iOptron", "iEAF no ack for " + cmd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(READ_CHAR_DELAY_MS));
        }
    }

    mutable std::mutex mutex_;
    IeafConnectionConfig config_;
    bool connected_ = false;

#ifndef _WIN32
    int serial_fd_ = -1;
#endif
};

// --- IeafProtocolWrapper public interface forwarding ---

IeafProtocolWrapper::IeafProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

IeafProtocolWrapper::~IeafProtocolWrapper() = default;

IeafDeviceInfo IeafProtocolWrapper::connect(const IeafConnectionConfig& config) { return impl_->connect(config); }

void IeafProtocolWrapper::disconnect() { impl_->disconnect(); }

bool IeafProtocolWrapper::is_connected() const { return impl_->is_connected(); }

IeafStatus IeafProtocolWrapper::get_status() { return impl_->get_status(); }

void IeafProtocolWrapper::move_to(std::int32_t position) { impl_->move_to(position); }

void IeafProtocolWrapper::halt() { impl_->halt(); }

void IeafProtocolWrapper::set_zero() { impl_->set_zero(); }

}  // namespace alpacacore::vendor::ioptron

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
#include <alpacacore/vendor/ioptron/ioptron_iefw_protocol_wrapper.h>

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

constexpr int COMMAND_DELAY_MS = 5;  // read_response() waits for the '#' terminator
constexpr int READ_CHAR_DELAY_MS = 10;
constexpr int ACK_TIMEOUT_MS = 300;
constexpr int MAX_RESPONSE_LEN = 32;
constexpr int HANDSHAKE_RETRIES = 3;
constexpr int HANDSHAKE_READ_TIMEOUT_S = 2;  // shorter than the 4s runtime timeout (INDI iEFW_TIMEOUT)
constexpr std::int32_t IEFW_MAX_SLOTS = 99;  // 2-digit slot field in :WMnn#

bool parse_device_info(const std::string& resp, IefwDeviceInfo& out) {
    // ":DeviceInfo#" -> 12 digits: "%6d%2d%4d" = position, model code, firmware.
    int pos = 0, model = 0, fw = 0;
    if (std::sscanf(resp.c_str(), "%6d%2d%4d", &pos, &model, &fw) != 3) {
        return false;
    }
    out.position = pos;
    out.model = model;
    out.firmware = fw;
    return true;
}

// Configure an fd for the iEFW's fixed 115200 8N1 raw mode. Shared by the
// auto-detect probe and the live connection.
bool configure_serial_fd(int fd) {
#ifndef _WIN32
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
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
        return false;
    }
    return util::clear_nonblocking(fd);
#else
    (void)fd;
    return false;
#endif
}

}  // namespace

bool is_iefw_model(std::int32_t model) {
    // 99 = iEFW-15 (5 slots), 98 = iEFW-18 (8 slots): INDI ioptron_wheel.cpp.
    return model == 98 || model == 99;
}

std::int32_t iefw_slot_count(std::int32_t model) {
    if (model == 99) return 5;
    if (model == 98) return 8;
    return 0;
}

std::string iefw_model_name(std::int32_t model) {
    if (model == 99) return "iEFW-15";
    if (model == 98) return "iEFW-18";
    return "iEFW";
}

// Probe a serial port with the :DeviceInfo# handshake. Returns true (and
// fills `info`) only if the reply parses and reports an iEFW model code.
// Prolific PL2303 bridge like the focusers: no DTR-reset quirk on open.
static bool probe_port(const std::string& port_path, IefwDeviceInfo& info) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    if (!configure_serial_fd(fd)) {
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

        IefwDeviceInfo parsed;
        if (parse_device_info(resp, parsed) && is_iefw_model(parsed.model)) {
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
// Raw /dev/ttyUSBn nodes have no by-id name to filter on; restrict the
// fallback to Prolific PL2303-class adapters. The iOptron mount and the
// iEAF/iAFS focusers enumerate the same way; the :DeviceInfo# model code
// is what tells an iEFW apart from them.
bool raw_port_looks_like_iefw_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor,
                                                        {"067b", "Prolific", "PL2303", "USB-Serial", "USB_Serial"});
}
}  // namespace
#endif

std::vector<IefwPortInfo> enumerate_iefw_ports() {
    std::vector<IefwPortInfo> results;

#ifndef _WIN32
    std::set<std::string> probed;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
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
            probe_msg += ") for iEFW...";
            ALPACA_LOG_INFO("iOptron", probe_msg);

            IefwDeviceInfo info;
            if (probe_port(resolved, info)) {
                ALPACA_LOG_INFO("iOptron", "Found " + iefw_model_name(info.model) + " on " + resolved + " (firmware " +
                                               std::to_string(info.firmware) + ")");
                results.push_back({resolved, name, info});
            }
        }
    }

    // Always also probe raw /dev/ttyUSB* nodes: generic Prolific adapters with
    // no per-device serial number collide in udev's by-id naming.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;

        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_iefw_candidate(resolved)) continue;
        probed.insert(resolved);

        ALPACA_LOG_INFO("iOptron", "Probing " + resolved + " for iEFW...");
        IefwDeviceInfo info;
        if (probe_port(resolved, info)) {
            ALPACA_LOG_INFO("iOptron", "Found " + iefw_model_name(info.model) + " on " + resolved + " (firmware " +
                                           std::to_string(info.firmware) + ")");
            results.push_back({resolved, "", info});
        }
    }
#endif

    return results;
}

class IefwProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    IefwDeviceInfo connect(const IefwConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;

        connect_serial();

        IefwDeviceInfo info;
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
                                "iEFW handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }

        config_.serial_timeout_s = saved_timeout;

        if (!success) {
            disconnect_locked();
            throw AlpacaException("iEFW handshake failed after " + std::to_string(HANDSHAKE_RETRIES) + " attempts",
                                  AlpacaError::NotConnected);
        }
        if (!is_iefw_model(info.model)) {
            disconnect_locked();
            throw AlpacaException("Device on " + config_.serial_port + " reported model code " +
                                      std::to_string(info.model) + ", not an iEFW (expected 98 or 99)",
                                  AlpacaError::NotConnected);
        }

        connected_ = true;
        ALPACA_LOG_INFO("iOptron", iefw_model_name(info.model) + " connected (model " + std::to_string(info.model) +
                                       ", firmware " + std::to_string(info.firmware) + ", " +
                                       std::to_string(iefw_slot_count(info.model)) + " slots)");
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

    std::string get_firmware() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":FW1#");
        if (!resp.empty() && resp.back() == '#') {
            resp.pop_back();
        }
        return resp;
    }

    std::int32_t get_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        // ":WP#" -> "nn#" (0-based slot) or "-1#" while moving.
        std::string resp = send_command_locked(":WP#");
        int pos = 0;
        if (std::sscanf(resp.c_str(), "%d", &pos) != 1) {
            throw AlpacaException("Failed to parse iEFW position: " + resp, AlpacaError::DriverException);
        }
        return pos < 0 ? -1 : pos;
    }

    void move_to(std::int32_t slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (slot < 0 || slot > IEFW_MAX_SLOTS) {
            throw AlpacaException("iEFW slot out of range", AlpacaError::InvalidValue);
        }
        char cmd[16];
        std::snprintf(cmd, sizeof(cmd), ":WM%02d#", static_cast<int>(slot));
        send_command_ack_locked(cmd);
    }

    std::int32_t get_stored_offset(std::int32_t slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (slot < 0 || slot > IEFW_MAX_SLOTS) {
            throw AlpacaException("iEFW slot out of range", AlpacaError::InvalidValue);
        }
        char cmd[16];
        std::snprintf(cmd, sizeof(cmd), ":WF%02d#", static_cast<int>(slot));
        // "snnnnn#": sign + 5 digits.
        std::string resp = send_command_locked(cmd);
        int offset = 0;
        if (std::sscanf(resp.c_str(), "%d", &offset) != 1) {
            throw AlpacaException("Failed to parse iEFW offset: " + resp, AlpacaError::DriverException);
        }
        return offset;
    }

    void set_stored_offset(std::int32_t slot, std::int32_t offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (slot < 0 || slot > IEFW_MAX_SLOTS) {
            throw AlpacaException("iEFW slot out of range", AlpacaError::InvalidValue);
        }
        if (offset < -99999 || offset > 99999) {
            throw AlpacaException("iEFW offset out of range", AlpacaError::InvalidValue);
        }
        char cmd[24];
        // ":WOnnsnnnnn#": slot, sign, 5 zero-padded digits.
        std::snprintf(cmd, sizeof(cmd), ":WO%02d%c%05d#", static_cast<int>(slot), offset < 0 ? '-' : '+',
                      static_cast<int>(offset < 0 ? -offset : offset));
        send_command_ack_locked(cmd);
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("iEFW not connected", AlpacaError::NotConnected);
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
        if (!configure_serial_fd(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }
        tcflush(serial_fd_, TCIOFLUSH);
#else
        throw AlpacaException("iEFW driver is Linux-only", AlpacaError::DriverException);
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
                throw AlpacaException("iEFW read timeout", AlpacaError::DriverException);
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
                    throw AlpacaException("iEFW response too long", AlpacaError::DriverException);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(READ_CHAR_DELAY_MS));
            }
        }

        ALPACA_LOG_TRACE("iOptron", "iEFW response: " + response);
        return response;
    }

    // Flush stale input before every command so a late ack byte or a
    // half-read reply cannot prefix the next fixed-format response.
    std::string send_command_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iEFW command: " + cmd);
        tcflush_port();
        write_data(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(COMMAND_DELAY_MS));
        return read_response();
    }

    // :WM / :WO answer with a single '1' byte and no '#'. Consume it so it
    // cannot race the flush at the start of the next :WP# (the same leaked
    // ack race the iEAF driver hit on hardware). The ack is optional: a
    // silent firmware only costs ACK_TIMEOUT_MS.
    void send_command_ack_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iEFW command (ack): " + cmd);
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
                    ALPACA_LOG_WARN("iOptron", std::string("iEFW unexpected ack byte: ") + ch);
                }
                return;
            }
#endif
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > ACK_TIMEOUT_MS) {
                ALPACA_LOG_DEBUG("iOptron", "iEFW no ack for " + cmd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(READ_CHAR_DELAY_MS));
        }
    }

    mutable std::mutex mutex_;
    IefwConnectionConfig config_;
    bool connected_ = false;

#ifndef _WIN32
    int serial_fd_ = -1;
#endif
};

// --- IefwProtocolWrapper public interface forwarding ---

IefwProtocolWrapper::IefwProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

IefwProtocolWrapper::~IefwProtocolWrapper() = default;

IefwDeviceInfo IefwProtocolWrapper::connect(const IefwConnectionConfig& config) { return impl_->connect(config); }

void IefwProtocolWrapper::disconnect() { impl_->disconnect(); }

bool IefwProtocolWrapper::is_connected() const { return impl_->is_connected(); }

std::string IefwProtocolWrapper::get_firmware() { return impl_->get_firmware(); }

std::int32_t IefwProtocolWrapper::get_position() { return impl_->get_position(); }

void IefwProtocolWrapper::move_to(std::int32_t slot) { impl_->move_to(slot); }

std::int32_t IefwProtocolWrapper::get_stored_offset(std::int32_t slot) { return impl_->get_stored_offset(slot); }

void IefwProtocolWrapper::set_stored_offset(std::int32_t slot, std::int32_t offset) {
    impl_->set_stored_offset(slot, offset);
}

}  // namespace alpacacore::vendor::ioptron

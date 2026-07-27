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
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/wandererastro/wandererastro_box_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::wandererastro {

namespace {

constexpr int MAX_TOKEN_LEN = 64;

// Fields following the identity token in one status frame (firmware + 21
// values, per the INDI reference field order documented on BoxState).
constexpr int kBoxFrameFieldCount = 22;

using util::is_serial_port_in_use;
using util::mark_serial_port_closed;
using util::mark_serial_port_open;

// Dew point from DHT22 humidity/temperature via the Magnus formula — same
// expression as the INDI reference (and the vendor's WandererEmpire app).
double magnus_dew_point(double temp_c, double humidity_pct) {
    if (humidity_pct <= 0.0) {
        return -273.15;  // undefined at 0% RH — report an obviously-floor value
    }
    const double gamma = (17.27 * temp_c) / (237.7 + temp_c) + std::log(humidity_pct / 100.0);
    return (237.7 * gamma) / (17.27 - gamma);
}

#ifndef _WIN32
// Configure an already-open POSIX fd for 19200 8N1 raw I/O. HUPCL is cleared so
// DTR stays asserted on close: the CH340 adapter asserts DTR on open, which
// pulses the controller's MCU reset line; keeping DTR high across close means a
// subsequent reopen does not reset the controller again.
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
    tty.c_cc[VTIME] = 5;  // 0.5s per-read timeout so loops can poll their stop flags
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        return false;
    }
    if (!util::clear_nonblocking(fd)) {
        return false;
    }
    tcflush(fd, TCIOFLUSH);
    return true;
}

// Read one 'A'-terminated token from the port, skipping CR/LF, within
// timeout_ms. Returns std::nullopt on timeout or read error.
//
// @p carry holds partial token bytes across calls (a token straddling a poll
// window must not lose its leading characters — the rotator wrapper lesson).
std::optional<std::string> read_token(int fd, int timeout_ms, std::string& carry) {
    std::string token = std::move(carry);
    carry.clear();
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() <
           timeout_ms) {
        char ch = 0;
        // Intentional bounded read under the caller's lock: VMIN=0/VTIME=0.5s
        // caps each read at 0.5s (same pattern as the other serial wrappers).
        ssize_t r = ::read(fd, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (r == 1) {
            if (ch == 'A') {
                return token;
            }
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            token += ch;
            if (token.size() > MAX_TOKEN_LEN) {
                token.clear();  // garbage — resynchronise on the next delimiter
            }
        } else if (r < 0 && errno != EINTR) {
            // Persistent read error (e.g. unplugged): VTIME rate-limits only the
            // no-data path, so back off to avoid spinning for the whole window.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    carry = std::move(token);  // preserve the partial token for the next call
    return std::nullopt;
}

// Parse the 22 fields that follow the identity token into a BoxState.
// Returns std::nullopt on any malformed numeric field.
std::optional<BoxState> parse_frame_fields(const std::vector<std::string>& f) {
    if (f.size() != static_cast<std::size_t>(kBoxFrameFieldCount)) {
        return std::nullopt;
    }
    BoxState s;
    try {
        s.firmware_version = std::stoi(f[0]);
        s.probe_temps[0] = std::stod(f[1]);
        s.probe_temps[1] = std::stod(f[2]);
        s.probe_temps[2] = std::stod(f[3]);
        s.humidity = std::stod(f[4]);
        s.ambient_temp = std::stod(f[5]);
        s.total_current = std::stod(f[6]);
        s.v19_current = std::stod(f[7]);
        s.adj_current = std::stod(f[8]);
        s.input_voltage = std::stod(f[9]);
        s.usb31_1 = (std::stoi(f[10]) != 0);
        s.usb31_2 = (std::stoi(f[11]) != 0);
        s.usb31_3 = (std::stoi(f[12]) != 0);
        s.usb2_13 = (std::stoi(f[13]) != 0);
        s.usb2_46 = (std::stoi(f[14]) != 0);
        s.dc3_4 = (std::stoi(f[15]) != 0);
        s.dc5_pwm = std::stoi(f[16]);
        s.dc6_pwm = std::stoi(f[17]);
        s.dc7_pwm = std::stoi(f[18]);
        s.dc8_9 = (std::stoi(f[19]) != 0);
        s.dc10_11 = (std::stoi(f[20]) != 0);
        s.dc3_4_voltage = std::stod(f[21]) / 10.0;  // frame carries volts x 10
    } catch (const std::exception&) {
        return std::nullopt;
    }
    // The firmware streams literal "nan" for an absent/unread DHT22 sensor and
    // std::stod parses it as NaN. NaN cannot survive into the Alpaca layer:
    // nlohmann::json serializes NaN as JSON null, which crashes ConformU's
    // DeviceState parser (verified on real hardware, firmware 20250410). Map
    // non-finite readings to the DS18B20 "unconnected" convention (-127 degC)
    // for temperatures and to 0 for humidity/power values.
    for (auto& t : s.probe_temps) {
        if (!std::isfinite(t)) t = -127.0;
    }
    if (!std::isfinite(s.ambient_temp)) s.ambient_temp = -127.0;
    if (!std::isfinite(s.humidity)) s.humidity = 0.0;
    if (!std::isfinite(s.total_current)) s.total_current = 0.0;
    if (!std::isfinite(s.v19_current)) s.v19_current = 0.0;
    if (!std::isfinite(s.adj_current)) s.adj_current = 0.0;
    if (!std::isfinite(s.input_voltage)) s.input_voltage = 0.0;
    if (!std::isfinite(s.dc3_4_voltage)) s.dc3_4_voltage = 0.0;
    s.dew_point = (s.humidity > 0.0 && s.ambient_temp > -100.0)
                      ? magnus_dew_point(s.ambient_temp, s.humidity)
                      : -127.0;  // sensor absent — same unconnected convention
    if (!std::isfinite(s.dew_point)) s.dew_point = -127.0;
    s.valid = true;
    return s;
}

// Listen on an already-configured fd for one complete status frame: wait for
// the identity token, then collect the following 22 fields. The controller
// streams continuously, so a bounded listen suffices.
std::optional<BoxState> await_frame(int fd, int timeout_ms) {
    std::string carry;
    std::vector<std::string> fields;
    bool in_frame = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto token = read_token(fd, static_cast<int>(std::max<long long>(remaining.count(), 1)), carry);
        if (!token.has_value()) {
            continue;
        }
        if (*token == kBoxProV3ModelToken) {
            in_frame = true;
            fields.clear();
            continue;
        }
        if (!in_frame) {
            continue;  // mid-frame join — wait for the next identity token
        }
        fields.push_back(*token);
        if (fields.size() == static_cast<std::size_t>(kBoxFrameFieldCount)) {
            auto state = parse_frame_fields(fields);
            if (state.has_value()) {
                return state;
            }
            in_frame = false;  // malformed — resynchronise on the next identity token
            fields.clear();
        }
    }
    return std::nullopt;
}

// Open a candidate port and listen for the streamed identity. Returns true on
// a positive identification. Fully passive.
bool probe_box_port(const std::string& port_path, BoxPortInfo& info) {
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    if (!configure_serial_fd(fd)) {
        close(fd);
        return false;
    }
    // Re-check after opening: a driver may have claimed this port in the window
    // between the caller's is_serial_port_in_use() check and this open(). Bail
    // rather than listening on that device's stream.
    if (is_serial_port_in_use(port_path)) {
        close(fd);
        return false;
    }

    // 3.5 s covers one full frame period plus a mid-frame join.
    auto state = await_frame(fd, 3500);

    // Re-apply HUPCL-clear before closing so DTR stays asserted.
    struct termios tty {};
    if (tcgetattr(fd, &tty) == 0) {
        tty.c_cflag &= ~HUPCL;
        tcsetattr(fd, TCSANOW, &tty);
    }
    close(fd);

    if (state.has_value()) {
        info.port_path = port_path;
        info.model = kBoxProV3ModelToken;
        info.firmware_version = state->firmware_version;
        return true;
    }
    return false;
}
#endif  // _WIN32

}  // namespace

std::vector<BoxPortInfo> enumerate_wandererbox_ports() {
    std::vector<BoxPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (!std::filesystem::exists(port)) continue;
            if (is_serial_port_in_use(port)) continue;  // held by another connected device
            ALPACA_LOG_INFO("WandererAstro", "Probing " + port + " for WandererBox Pro V3...");
            BoxPortInfo info;
            if (probe_box_port(port, info)) {
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

        // WandererBox boards use a CH340/CH341 USB-serial adapter (vendor 1a86).
        bool is_candidate = (name.find("USB_Serial") != std::string::npos) ||
                            (name.find("CH340") != std::string::npos) || (name.find("CH341") != std::string::npos) ||
                            (name.find("1a86") != std::string::npos);
        if (!is_candidate) continue;

        // error_code overload: a device unplugged after the directory scan leaves
        // a dangling symlink; skip it instead of aborting the enumeration.
        std::error_code ec;
        std::string resolved = std::filesystem::canonical(entry.path(), ec).string();
        if (ec) continue;

        if (is_serial_port_in_use(resolved)) continue;
        std::string probe_msg = "Probing ";
        probe_msg.append(resolved).append(" (").append(name).append(") for WandererBox Pro V3...");
        ALPACA_LOG_INFO("WandererAstro", probe_msg);

        BoxPortInfo info;
        if (probe_box_port(resolved, info)) {
            info.device_id = name;
            ALPACA_LOG_INFO("WandererAstro", "Found " + info.model + " on " + resolved + " (firmware " +
                                                 std::to_string(info.firmware_version) + ")");
            results.push_back(info);
        }
    }
#endif

    return results;
}

class WandererBoxProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    std::string connect(const BoxConnectionConfig& config) {
#ifdef _WIN32
        (void)config;
        throw AlpacaException("WandererBox serial support is POSIX-only", AlpacaError::DriverException);
#else
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (connected_) {
            throw AlpacaException("WandererBox already connected; call disconnect() first",
                                  AlpacaError::InvalidOperation);
        }
        // The protocol is fixed at 19200 8N1; configure_serial_fd() hardcodes
        // B19200, so reject any other configured rate instead of silently
        // ignoring it (the stored config value must not imply control it
        // doesn't have — PR #152 review).
        if (config.baud_rate != 19200) {
            throw AlpacaException("WandererBox baud rate is fixed at 19200, got " + std::to_string(config.baud_rate),
                                  AlpacaError::InvalidValue);
        }
        config_ = config;
        open_serial_locked();

        // The controller streams unprompted; wait up to the configured timeout
        // plus one frame period for the first complete frame.
        auto state = await_frame(serial_fd_, config_.serial_timeout_s * 1000 + 3500);
        if (!state.has_value()) {
            close_serial_locked();
            throw AlpacaException(
                "No WandererBox Pro V3 detected on " + config_.serial_port + " (no status frame received)",
                AlpacaError::NotConnected);
        }
        if (state->firmware_version < kBoxCalibratedPowerMinFirmware) {
            // Not fatal (INDI behaviour): older firmware just lacks calibrated
            // current readings.
            ALPACA_LOG_WARN("WandererAstro", "WandererBox firmware " + std::to_string(state->firmware_version) +
                                                 " predates calibrated power readings (min " +
                                                 std::to_string(kBoxCalibratedPowerMinFirmware) +
                                                 "); consider a firmware upgrade");
        }

        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            state_ = *state;
            firmware_date_ = format_firmware_date(state->firmware_version);
        }
        connected_ = true;
        reader_running_.store(true);
        reader_thread_ = std::thread([this] { reader_loop(); });
        ALPACA_LOG_INFO("WandererAstro", std::string("Connected to ") + kBoxProV3ModelToken + " (firmware " +
                                             std::to_string(state->firmware_version) + ") on " + config_.serial_port);
        return kBoxProV3ModelToken;
#endif
    }

    void disconnect() {
        // Mark disconnected under io_mutex_ FIRST so a concurrent command can
        // no longer write to the fd after we join. Then join WITHOUT holding
        // io_mutex_ (the reader locks it each iteration to fetch the fd), and
        // only then close the fd.
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            connected_ = false;
        }
        stop_reader();
        std::lock_guard<std::mutex> lock(io_mutex_);
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            state_ = BoxState{};
            firmware_date_.clear();
        }
#ifndef _WIN32
        close_serial_locked();
#endif
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return connected_;
    }

    BoxState get_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

    std::optional<std::string> get_firmware_date() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (firmware_date_.empty()) {
            return std::nullopt;
        }
        return firmware_date_;
    }

    void set_dc3_4(bool on) { send_command(on ? "101" : "100"); }

    void set_dc3_4_voltage(double volts) {
        if (volts < kBoxDc34VoltageMin || volts > kBoxDc34VoltageMax) {
            throw AlpacaException("DC3-4 voltage must be within [5.0, 13.2] V, got " + std::to_string(volts),
                                  AlpacaError::InvalidValue);
        }
        char cmd[8];
        std::snprintf(cmd, sizeof(cmd), "20%03d", static_cast<int>(std::lround(volts * 10.0)));
        send_command(cmd);
    }

    void set_pwm(int channel, int value) {
        if (channel < 5 || channel > 7) {
            throw AlpacaException("PWM channel must be 5, 6 or 7, got " + std::to_string(channel),
                                  AlpacaError::InvalidValue);
        }
        if (value < 0 || value > kBoxPwmMax) {
            throw AlpacaException("PWM value must be within [0, 255], got " + std::to_string(value),
                                  AlpacaError::InvalidValue);
        }
        char cmd[8];
        std::snprintf(cmd, sizeof(cmd), "%d%03d", channel, value);
        send_command(cmd);
    }

    void set_dc8_9(bool on) { send_command(on ? "201" : "200"); }

    void set_dc10_11(bool on) { send_command(on ? "211" : "210"); }

    void set_usb(int group, bool on) {
        // Command families 11x..15x: USB3.1-1, USB3.1-2, USB3.1-3,
        // USB2.0(1-3), USB2.0(4-6) — per the INDI reference.
        if (group < 0 || group > 4) {
            throw AlpacaException("USB group must be within [0, 4], got " + std::to_string(group),
                                  AlpacaError::InvalidValue);
        }
        const std::string cmd = std::to_string(11 + group) + (on ? "1" : "0");
        send_command(cmd);
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("WandererBox not connected", AlpacaError::NotConnected);
        }
    }

    void send_command(const std::string& code) {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_connected_locked();
        const std::string payload = code + "\n";
        ALPACA_LOG_TRACE("WandererAstro", "Box command: " + code);
        if (!util::write_all(serial_fd_, payload.data(), payload.size())) {
            throw AlpacaException("Serial write failed: " + std::string(std::strerror(errno)),
                                  AlpacaError::DriverException);
        }
#else
        (void)code;
        throw AlpacaException("WandererBox serial support is POSIX-only", AlpacaError::DriverException);
#endif
    }

    static std::string format_firmware_date(int fw) {
        const int year = fw / 10000;
        const int month = (fw / 100) % 100;
        const int day = fw % 100;
        char buf[16];
        if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
        } else {
            // Not a plausible YYYYMMDD — surface the raw integer instead of an
            // invalid date like 2024-13-99.
            std::snprintf(buf, sizeof(buf), "%d", fw);
        }
        return buf;
    }

#ifndef _WIN32
    // Consume the controller's continuous status stream: a rolling token
    // stream resynchronising on the identity token, collecting 22 fields per
    // frame and committing each complete, well-formed frame to the cache.
    void reader_loop() {
        std::string carry;  // partial-token bytes preserved across poll windows
        std::vector<std::string> fields;
        bool in_frame = false;
        while (reader_running_.load()) {
            int fd;
            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                fd = serial_fd_;
            }
            if (fd < 0) {
                return;  // disconnected under us
            }
            auto token = read_token(fd, 600, carry);
            if (!token.has_value()) {
                continue;
            }
            if (*token == kBoxProV3ModelToken) {
                in_frame = true;
                fields.clear();
                continue;
            }
            if (!in_frame) {
                continue;
            }
            fields.push_back(*token);
            if (fields.size() == static_cast<std::size_t>(kBoxFrameFieldCount)) {
                auto state = parse_frame_fields(fields);
                in_frame = false;
                fields.clear();
                if (state.has_value()) {
                    std::lock_guard<std::mutex> state_lock(state_mutex_);
                    state_ = *state;
                    firmware_date_ = format_firmware_date(state->firmware_version);
                }
            }
        }
    }
#endif

    void stop_reader() {
        reader_running_.store(false);
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
    }

#ifndef _WIN32
    void open_serial_locked() {
        // Claim the port in the registry BEFORE opening it so a concurrent
        // auto-detect scan can't slip in between check and open. Store the
        // canonical path so by-id symlinks and ttyUSBn nodes compare equal.
        std::error_code path_ec;
        std::string canonical_path = std::filesystem::canonical(config_.serial_port, path_ec).string();
        opened_port_ = path_ec ? config_.serial_port : canonical_path;
        mark_serial_port_open(opened_port_);

        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + std::strerror(errno) + ")",
                AlpacaError::NotConnected);
        }
        if (!configure_serial_fd(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }
    }

    void close_serial_locked() {
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
        mark_serial_port_closed(opened_port_);
        opened_port_.clear();
    }
#endif

    mutable std::mutex io_mutex_;     // guards serial fd + connected_/config_
    mutable std::mutex state_mutex_;  // guards state_ + firmware_date_
    BoxConnectionConfig config_;
    bool connected_ = false;
    std::string opened_port_;  // path registered in the in-use registry while open

    BoxState state_;
    std::string firmware_date_;  // YYYY-MM-DD, from the status stream

    std::atomic<bool> reader_running_{false};
    std::thread reader_thread_;

#ifndef _WIN32
    int serial_fd_ = -1;
#endif
};

// --- WandererBoxProtocolWrapper public interface forwarding ---

WandererBoxProtocolWrapper::WandererBoxProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

WandererBoxProtocolWrapper::~WandererBoxProtocolWrapper() = default;

std::string WandererBoxProtocolWrapper::connect(const BoxConnectionConfig& config) { return impl_->connect(config); }

void WandererBoxProtocolWrapper::disconnect() { impl_->disconnect(); }

bool WandererBoxProtocolWrapper::is_connected() const { return impl_->is_connected(); }

BoxState WandererBoxProtocolWrapper::get_state() const { return impl_->get_state(); }

std::optional<std::string> WandererBoxProtocolWrapper::get_firmware_date() const { return impl_->get_firmware_date(); }

void WandererBoxProtocolWrapper::set_dc3_4(bool on) { impl_->set_dc3_4(on); }

void WandererBoxProtocolWrapper::set_dc3_4_voltage(double volts) { impl_->set_dc3_4_voltage(volts); }

void WandererBoxProtocolWrapper::set_pwm(int channel, int value) { impl_->set_pwm(channel, value); }

void WandererBoxProtocolWrapper::set_dc8_9(bool on) { impl_->set_dc8_9(on); }

void WandererBoxProtocolWrapper::set_dc10_11(bool on) { impl_->set_dc10_11(on); }

void WandererBoxProtocolWrapper::set_usb(int group, bool on) { impl_->set_usb(group, on); }

}  // namespace alpacacore::vendor::wandererastro

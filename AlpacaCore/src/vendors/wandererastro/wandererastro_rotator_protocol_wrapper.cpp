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
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>

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

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::wandererastro {

namespace {

// The V2 hardware reports the same handshake token family as the V1 ("Mini"
// covers both per the INDI reference); prefix-match so a future
// "WandererRotatorMiniV2" token is also accepted.
constexpr char kRotatorModelPrefix[] = "WandererRotatorMini";
constexpr int MAX_TOKEN_LEN = 64;

// The motor sweeps roughly 1 degree per 240 ms (INDI reference constant); used
// only to extrapolate the reported angle while a move is in flight and to
// bound how long the completion monitor waits for the device's reply.
constexpr double kMsPerDegree = 240.0;

using util::is_serial_port_in_use;
using util::mark_serial_port_closed;
using util::mark_serial_port_open;

#ifndef _WIN32
// Configure an already-open POSIX fd for 19200 8N1 raw I/O. HUPCL is cleared so
// DTR stays asserted on close: the CH340 adapter asserts DTR on open, which
// pulses the rotator's MCU reset line; keeping DTR high across close means a
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
// @p carry holds partial token bytes across calls: a token that arrives
// straddling a timeout boundary must NOT lose its already-read leading
// characters. Without this, the move-completion monitor (which polls in short
// windows for the whole multi-second move) intermittently dropped the leading
// digit or sign of the report ("136.02" -> "36.02", "-40.00" -> "40.00"),
// producing ConformU position failures of exactly +100/-90 degrees.
std::optional<std::string> read_section(int fd, int timeout_ms, std::string& carry) {
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

struct HandshakeResult {
    std::string model;
    int firmware = 0;
    double mechanical_angle = 0.0;  // degrees
    double backlash = 0.0;          // degrees
    bool reverse = false;
};

// Perform the "1500001" identity handshake on an already-configured fd.
// The reply is five 'A'-terminated fields:
//   <name>A<firmware>A<angle*1000>A<backlash>A<reverse>A
// Returns std::nullopt if the port does not answer like a WandererRotator.
std::optional<HandshakeResult> do_handshake(int fd, int timeout_ms) {
    tcflush(fd, TCIOFLUSH);
    // The controller parses the bare digit string; no terminator is sent
    // (matches the INDI reference, which only appends '\n' to config commands).
    if (!util::write_all(fd, "1500001", 7)) {
        return std::nullopt;
    }
    std::string carry;
    auto name = read_section(fd, timeout_ms, carry);
    if (!name.has_value() || name->empty()) {
        // One retry: the first write can race leftover garbage in the MCU's
        // command buffer (same retry the INDI reference performs).
        tcflush(fd, TCIOFLUSH);
        carry.clear();  // flushed the wire — a held partial token is stale too
        if (!util::write_all(fd, "1500001", 7)) {
            return std::nullopt;
        }
        name = read_section(fd, timeout_ms, carry);
        if (!name.has_value() || name->empty()) {
            return std::nullopt;
        }
    }

    HandshakeResult result;
    result.model = *name;
    try {
        auto firmware = read_section(fd, timeout_ms, carry);
        auto angle = read_section(fd, timeout_ms, carry);
        auto backlash = read_section(fd, timeout_ms, carry);
        auto reverse = read_section(fd, timeout_ms, carry);
        if (!firmware || !angle || !backlash || !reverse) {
            return std::nullopt;
        }
        result.firmware = std::stoi(*firmware);
        const double raw_angle = std::stod(*angle);
        // Angle field format differs by firmware generation (verified on real
        // Mini V2 hardware, ConformU 2026-07-23): V1 firmware reports
        // (degrees * 1000) as a bare integer (the INDI convention); V2 reports
        // decimal degrees (token contains a '.'). Distinguish by the token text.
        const bool decimal_degrees = (angle->find('.') != std::string::npos);
        const double angle_limit = decimal_degrees ? 400.0 : 400000.0;
        if (std::fabs(raw_angle) > angle_limit) {
            // Accumulated virtual angle is implausible — reset to zero like the
            // INDI reference, then re-handshake once for a consistent snapshot.
            const char reset_cmd[] = "1500002\n";
            if (!util::write_all(fd, reset_cmd, sizeof(reset_cmd) - 1)) {
                return std::nullopt;
            }
            ALPACA_LOG_WARN("WandererAstro", "Rotator virtual mechanical angle out of range; reset to zero");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return do_handshake(fd, timeout_ms);
        }
        result.mechanical_angle = decimal_degrees ? std::fabs(raw_angle) : std::fabs(raw_angle) / 1000.0;
        result.backlash = std::stod(*backlash);
        result.reverse = (std::stod(*reverse) != 0.0);
    } catch (const std::exception&) {
        return std::nullopt;  // malformed numeric field
    }
    tcflush(fd, TCIFLUSH);
    return result;
}

// Open a candidate port, handshake, and report the model/firmware if it
// identifies a WandererRotator. Returns true on a positive identification.
bool probe_rotator_port(const std::string& port_path, RotatorPortInfo& info) {
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
    // rather than injecting a handshake into that device's command stream.
    if (is_serial_port_in_use(port_path)) {
        close(fd);
        return false;
    }

    auto handshake = do_handshake(fd, 1500);

    // Re-apply HUPCL-clear before closing so DTR stays asserted.
    struct termios tty {};
    if (tcgetattr(fd, &tty) == 0) {
        tty.c_cflag &= ~HUPCL;
        tcsetattr(fd, TCSANOW, &tty);
    }
    close(fd);

    if (handshake.has_value() && handshake->model.rfind(kRotatorModelPrefix, 0) == 0) {
        info.port_path = port_path;
        info.model = handshake->model;
        info.firmware_version = handshake->firmware;
        return true;
    }
    return false;
}
#endif  // _WIN32

}  // namespace

std::vector<RotatorPortInfo> enumerate_wanderer_rotator_ports() {
    std::vector<RotatorPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        for (int i = 0; i < 10; ++i) {
            std::string port = "/dev/ttyUSB" + std::to_string(i);
            if (!std::filesystem::exists(port)) continue;
            if (is_serial_port_in_use(port)) continue;  // held by another connected device
            ALPACA_LOG_INFO("WandererAstro", "Probing " + port + " for WandererRotator...");
            RotatorPortInfo info;
            if (probe_rotator_port(port, info)) {
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

        // WandererRotator boards use a CH340/CH341 USB-serial adapter (vendor 1a86).
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
        probe_msg.append(resolved).append(" (").append(name).append(") for WandererRotator...");
        ALPACA_LOG_INFO("WandererAstro", probe_msg);

        RotatorPortInfo info;
        if (probe_rotator_port(resolved, info)) {
            info.device_id = name;
            ALPACA_LOG_INFO("WandererAstro", "Found " + info.model + " on " + resolved + " (firmware " +
                                                 std::to_string(info.firmware_version) + ")");
            results.push_back(info);
        }
    }
#endif

    return results;
}

class WandererRotatorProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    std::string connect(const RotatorConnectionConfig& config) {
#ifdef _WIN32
        (void)config;
        throw AlpacaException("WandererRotator serial support is POSIX-only", AlpacaError::DriverException);
#else
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (connected_) {
            throw AlpacaException("WandererRotator already connected; call disconnect() first",
                                  AlpacaError::InvalidOperation);
        }
        config_ = config;
        open_serial_locked();

        auto handshake = do_handshake(serial_fd_, config_.serial_timeout_s * 1000);
        if (!handshake.has_value() || handshake->model.rfind(kRotatorModelPrefix, 0) != 0) {
            const std::string found = handshake.has_value() ? (" (device reported '" + handshake->model + "')") : "";
            close_serial_locked();
            throw AlpacaException("No WandererRotator Mini detected on " + config_.serial_port + found,
                                  AlpacaError::NotConnected);
        }
        if (handshake->firmware < kRotatorMiniMinFirmware) {
            close_serial_locked();
            throw AlpacaException("WandererRotator firmware " + std::to_string(handshake->firmware) +
                                      " is older than the minimum supported " +
                                      std::to_string(kRotatorMiniMinFirmware) + "; please upgrade the firmware",
                                  AlpacaError::NotConnected);
        }

        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            state_ = RotatorState{};
            state_.valid = true;
            state_.model = handshake->model;
            state_.firmware_version = handshake->firmware;
            state_.mechanical_angle = handshake->mechanical_angle;
            state_.backlash = handshake->backlash;
            state_.reverse = handshake->reverse;
            firmware_date_ = format_firmware_date(handshake->firmware);
        }
        connected_ = true;
        ALPACA_LOG_INFO("WandererAstro", "Connected to " + handshake->model + " (firmware " +
                                             std::to_string(handshake->firmware) + ") on " + config_.serial_port);
        return handshake->model;
#endif
    }

    void disconnect() {
        stop_monitor();
        std::lock_guard<std::mutex> lock(io_mutex_);
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            state_ = RotatorState{};
            firmware_date_.clear();
        }
        connected_ = false;
#ifndef _WIN32
        close_serial_locked();
#endif
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return connected_;
    }

    RotatorState get_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        RotatorState s = state_;
        if (s.moving) {
            s.mechanical_angle = extrapolated_angle_locked();
        }
        return s;
    }

    std::optional<std::string> get_firmware_date() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (firmware_date_.empty()) {
            return std::nullopt;
        }
        return firmware_date_;
    }

    void move_relative(double delta_degrees) {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_connected_locked();
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (state_.moving) {
                throw AlpacaException("WandererRotator is already moving; halt first", AlpacaError::InvalidOperation);
            }
        }
        // The previous monitor has finished (moving == false) but may not have
        // been joined yet.
        join_monitor();

        const int steps = static_cast<int>(delta_degrees * kRotatorMiniStepsPerDegree);
        if (steps == 0) {
            return;  // sub-step request — nothing the hardware can do
        }
        const std::string cmd = std::to_string(steps + 1000000);
        tcflush(serial_fd_, TCIFLUSH);  // stale completion bytes would fake an instant arrival
        write_command_locked(cmd, /*terminator=*/false);

        const double commanded = static_cast<double>(steps) / kRotatorMiniStepsPerDegree;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            move_start_angle_ = state_.mechanical_angle;
            move_delta_ = commanded;
            move_start_time_ = std::chrono::steady_clock::now();
            state_.moving = true;
        }
        monitor_running_.store(true);
        monitor_thread_ = std::thread([this, commanded] { monitor_move(commanded); });
#else
        (void)delta_degrees;
        throw AlpacaException("WandererRotator serial support is POSIX-only", AlpacaError::DriverException);
#endif
    }

    void halt() {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_connected_locked();
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            if (!state_.moving) {
                return;  // nothing in flight — ASCOM Halt when idle is a no-op
            }
        }
        // "Stop" is sent bare (no terminator), matching the INDI reference. The
        // controller decelerates and then reports its final angle; the monitor
        // thread picks that up and clears the moving flag.
        write_command_locked("Stop", /*terminator=*/false);
#endif
    }

    void set_reverse(bool reverse) {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_connected_locked();
        ensure_not_moving("set reverse");
        write_command_locked(reverse ? "1700001" : "1700000", /*terminator=*/true);
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        state_.reverse = reverse;
#else
        (void)reverse;
#endif
    }

    void set_backlash(double degrees) {
#ifndef _WIN32
        if (degrees < 0.0 || degrees > 3.0) {
            throw AlpacaException("Backlash must be within [0, 3] degrees, got " + std::to_string(degrees),
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_connected_locked();
        ensure_not_moving("set backlash");
        write_command_locked(std::to_string(static_cast<int>(degrees * 10.0) + 1600000), /*terminator=*/true);
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        state_.backlash = degrees;
#else
        (void)degrees;
#endif
    }

    void set_zero() {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_connected_locked();
        ensure_not_moving("set zero");
        write_command_locked("1500002", /*terminator=*/true);
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        state_.mechanical_angle = 0.0;
#endif
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("WandererRotator not connected", AlpacaError::NotConnected);
        }
    }

    void ensure_not_moving(const char* what) const {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (state_.moving) {
            throw AlpacaException(std::string("Cannot ") + what + " while the rotator is moving",
                                  AlpacaError::InvalidOperation);
        }
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
    // Extrapolate the in-flight angle at ~1 degree / 240 ms toward the target.
    // Caller must hold state_mutex_.
    double extrapolated_angle_locked() const {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - move_start_time_)
                .count();
        const double travelled = static_cast<double>(elapsed_ms) / kMsPerDegree;
        if (travelled >= std::fabs(move_delta_)) {
            return move_start_angle_ + move_delta_;
        }
        return move_start_angle_ + std::copysign(travelled, move_delta_);
    }

    // Write an ASCII command. Caller must hold io_mutex_. terminator selects
    // whether a trailing '\n' is appended (config commands) or not (handshake /
    // move / stop), matching the INDI reference byte-for-byte.
    void write_command_locked(const std::string& code, bool terminator) {
        const std::string payload = terminator ? code + "\n" : code;
        ALPACA_LOG_TRACE("WandererAstro", "Rotator command: " + code);
        if (!util::write_all(serial_fd_, payload.data(), payload.size())) {
            throw AlpacaException("Serial write failed: " + std::string(std::strerror(errno)),
                                  AlpacaError::DriverException);
        }
    }

    // Wait for the device's end-of-move report. The controller is silent while
    // the motor runs and then emits 'A'-terminated numeric token(s) whose last
    // value is (mechanicalAngle * 1000). A halt produces the same report early.
    void monitor_move(double commanded_degrees) {
        const auto expected_ms = static_cast<long long>(std::fabs(commanded_degrees) * kMsPerDegree);
        // Budget: the full sweep, plus deceleration/backlash slack, plus margin
        // for a halted-then-reported move. Beyond this the rotator is presumed
        // unpowered (DC in absent) — the serial link stays alive in that state.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(expected_ms * 2 + 5000);

        std::optional<std::string> last_token;
        std::string carry;  // partial-token bytes preserved across poll windows
        while (monitor_running_.load() && std::chrono::steady_clock::now() < deadline) {
            int fd;
            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                fd = serial_fd_;
            }
            if (fd < 0) {
                return;  // disconnected under us
            }
            auto token = read_section(fd, 600, carry);
            if (token.has_value() && !token->empty()) {
                ALPACA_LOG_TRACE("WandererAstro", "Rotator move report token: " + *token);
                try {
                    (void)std::stod(*token);  // keep only numeric tokens
                    last_token = *token;
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                    // non-numeric garbage on the wire — skip the token and keep
                    // listening for the real completion report.
                }
                continue;  // drain any follow-up token before declaring arrival
            }
            if (last_token.has_value()) {
                break;  // report received and the line went quiet — move done
            }
        }

        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (last_token.has_value()) {
            // Completion-report semantics differ by firmware generation
            // (verified on real Mini V2 hardware, ConformU 2026-07-23):
            //   - V1 (INDI convention): ABSOLUTE accumulated angle as a bare
            //     integer of (degrees * 1000).
            //   - V2: RELATIVE distance travelled in decimal degrees (the
            //     token contains a '.'), reported as a magnitude.
            // Distinguish by the token text, exactly like the handshake parse.
            const double value = std::stod(*last_token);
            if (last_token->find('.') != std::string::npos) {
                state_.mechanical_angle = move_start_angle_ + std::copysign(std::fabs(value), move_delta_);
            } else {
                state_.mechanical_angle = std::fabs(value) / 1000.0;
            }
        } else if (monitor_running_.load()) {
            // No report inside the budget: assume the commanded sweep happened
            // but warn — the classic cause is missing DC power (motor dead, MCU
            // alive). ConformU will surface any real position error.
            state_.mechanical_angle = move_start_angle_ + move_delta_;
            ALPACA_LOG_WARN("WandererAstro",
                            "Rotator move completion report not received; check DC power. Position estimated.");
        }
        state_.moving = false;
    }
#endif

    void stop_monitor() {
        monitor_running_.store(false);
        join_monitor();
    }

    void join_monitor() {
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
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
    mutable std::mutex state_mutex_;  // guards state_ + move bookkeeping + firmware_date_
    RotatorConnectionConfig config_;
    bool connected_ = false;
    std::string opened_port_;  // path registered in the in-use registry while open

    RotatorState state_;
    std::string firmware_date_;  // YYYY-MM-DD, from the handshake

    // In-flight move bookkeeping (guarded by state_mutex_).
    double move_start_angle_ = 0.0;
    double move_delta_ = 0.0;
    std::chrono::steady_clock::time_point move_start_time_{};

    std::atomic<bool> monitor_running_{false};
    std::thread monitor_thread_;

#ifndef _WIN32
    int serial_fd_ = -1;
#endif
};

// --- WandererRotatorProtocolWrapper public interface forwarding ---

WandererRotatorProtocolWrapper::WandererRotatorProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

WandererRotatorProtocolWrapper::~WandererRotatorProtocolWrapper() = default;

std::string WandererRotatorProtocolWrapper::connect(const RotatorConnectionConfig& config) {
    return impl_->connect(config);
}

void WandererRotatorProtocolWrapper::disconnect() { impl_->disconnect(); }

bool WandererRotatorProtocolWrapper::is_connected() const { return impl_->is_connected(); }

RotatorState WandererRotatorProtocolWrapper::get_state() const { return impl_->get_state(); }

std::optional<std::string> WandererRotatorProtocolWrapper::get_firmware_date() const {
    return impl_->get_firmware_date();
}

void WandererRotatorProtocolWrapper::move_relative(double delta_degrees) { impl_->move_relative(delta_degrees); }

void WandererRotatorProtocolWrapper::halt() { impl_->halt(); }

void WandererRotatorProtocolWrapper::set_reverse(bool reverse) { impl_->set_reverse(reverse); }

void WandererRotatorProtocolWrapper::set_backlash(double degrees) { impl_->set_backlash(degrees); }

void WandererRotatorProtocolWrapper::set_zero() { impl_->set_zero(); }

}  // namespace alpacacore::vendor::wandererastro

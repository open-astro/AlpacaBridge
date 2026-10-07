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
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/gemini/gemini_pdh_protocol_wrapper.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::gemini {

namespace {

// Tag letters the vendor driver splits the >G# payload on (C# String.Split).
constexpr std::string_view kFrameDelimiters = "DUATMBCSHVP";
constexpr int kFrameFieldCount = 17;

// Longest plausible frame: the >G# status is ~60 chars. Anything longer is
// garbage -- drop it and resynchronise on the next '#'.
constexpr std::size_t kMaxFrameLen = 256;

// Re-poll >G# when the cached frame is older than this. Two seconds keeps
// NINA's telemetry fresh while staying well under the firmware's own ~3 s
// sensor cadence the vendor app assumes.
constexpr int kStatusPollMs = 2000;

// Reply timeout for the identity/version/status requests at connect.
constexpr int kRequestTimeoutMs = 2500;

// Consecutive >G# polls that produce no status frame before the link is
// declared faulted (issue #237). Three polls at kStatusPollMs is ~6 s of
// silence: long enough that one lost frame or a slow reply never trips it,
// short enough that a client polling at 2-8 s sees the fault on its next
// read instead of half an hour of byte-identical telemetry. A failed poll
// write (EIO after a USB re-enumeration) counts the same as an unanswered one.
constexpr int kLinkFaultPolls = 3;

// Handshake cadence: the CH340 adapter asserts DTR on open, which resets the
// hub's MCU; the vendor driver sleeps a flat 2 s before its first >H#. Same
// staggered retry as the Gemini focuser (100 ms, 2 s, 1 s -- worst case ~9 s,
// inside the ASCOM client's 10 s connect budget).
constexpr int kHandshakeRetries = 3;

using util::is_serial_port_in_use;
using util::mark_serial_port_closed;
using util::mark_serial_port_open;

double parse_double_or(const std::string& text, double fallback) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) return fallback;
    return v;
}

int parse_int_or(const std::string& text, int fallback) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return fallback;
    return static_cast<int>(v);
}

bool all_digits(const std::string& s, std::size_t at_least) {
    if (s.size() < at_least) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

PdhDewMode mode_from_int(int m) {
    switch (m) {
        case 0:
            return PdhDewMode::Auto;
        case 2:
            return PdhDewMode::Switch;
        default:
            return PdhDewMode::Manual;
    }
}

std::string strip_frame(const std::string& frame_in) {
    std::string frame;
    frame.reserve(frame_in.size());
    for (char c : frame_in) {
        if (c == '\r' || c == '\n' || c == '#') continue;
        frame += c;
    }
    return frame;
}

#ifndef _WIN32
// Configure an already-open POSIX fd for 19200 8N1 raw I/O. HUPCL is cleared so
// DTR stays asserted on close: the CH340 adapter asserts DTR on open, which
// pulses the MCU reset line; keeping DTR high across close means a subsequent
// reopen does not reset the hub again (Gemini focuser / flat panel lesson).
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
    tty.c_cflag &= ~HUPCL;  // keep DTR high on close -- avoids CH340 MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;  // 0.5 s per-read timeout so loops can poll their stop flags
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        return false;
    }
    if (!util::clear_nonblocking(fd)) {
        return false;
    }
    tcflush(fd, TCIOFLUSH);
    return true;
}

// Read one '#'-terminated frame (terminator stripped, CR/LF skipped) within
// timeout_ms. @p carry preserves a partial frame across calls so a frame that
// straddles two poll windows keeps its leading bytes.
std::optional<std::string> read_frame(int fd, int timeout_ms, std::string& carry) {
    std::string frame = std::move(carry);
    carry.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        char ch = 0;
        // Bounded by VMIN=0/VTIME=0.5s (same pattern as the other serial wrappers).
        const ssize_t r = ::read(fd, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (r == 1) {
            if (ch == '#') {
                return frame;
            }
            if (ch == '\r' || ch == '\n') {
                continue;
            }
            frame += ch;
            if (frame.size() > kMaxFrameLen) {
                frame.clear();  // garbage -- resynchronise on the next terminator
            }
        } else if (r == 0 || errno != EINTR) {
            // Persistent read error, or no byte: VTIME expired, or the tty is
            // hung up (unplugged adapter, closed pty master), where read()
            // returns 0 at once (issue #772). Back off instead of spinning.
            // Connected stays true; the unanswered polls latch the fault (#237).
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    carry = std::move(frame);
    return std::nullopt;
}

bool write_line(int fd, const std::string& cmd) {
    // The vendor driver sends every command via SerialPort.WriteLine(), i.e.
    // ">X#\n". Mirror it exactly: the firmware is only proven against that.
    const std::string payload = cmd + "\n";
    return util::write_all(fd, payload.data(), payload.size());
}

// Probe a serial port with the hub's identity handshake. Returns true only
// when the exact kPdhHandshakeReply comes back, so ports hosting the Gemini
// focuser or a flat panel (same CH340 adapter family) are rejected.
bool probe_pdh_port(const std::string& port_path) {
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    if (!configure_serial_fd(fd)) {
        close(fd);
        return false;
    }
    // Re-check after opening: a driver may have claimed this port between the
    // caller's is_serial_port_in_use() check and this open().
    if (is_serial_port_in_use(port_path)) {
        close(fd);
        return false;
    }

    bool found = false;
    for (int attempt = 0; attempt < 2 && !found; ++attempt) {
        // First attempt waits out the post-DTR-reset boot (vendor: 2 s flat).
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 1));
        tcflush(fd, TCIOFLUSH);
        if (!write_line(fd, ">H#")) {
            continue;
        }
        // The firmware may interleave a streamed status frame before the
        // handshake reply; keep reading frames until the deadline.
        std::string carry;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            auto frame = read_frame(fd, static_cast<int>(std::max<long long>(remaining.count(), 1)), carry);
            if (!frame.has_value()) continue;
            if (is_pdh_handshake_reply(*frame)) {
                found = true;
                break;
            }
            if (frame->rfind("*HGeminiPowerBox", 0) == 0) {
                ALPACA_LOG_WARN("Gemini", "Port " + port_path + " answered '" + *frame +
                                              "' -- a Gemini power box, but not the Advanced 3 this driver supports");
                break;
            }
        }
        if (!found) {
            ALPACA_LOG_DEBUG("Gemini", "Power hub probe attempt " + std::to_string(attempt + 1) +
                                           " got no identity reply from " + port_path);
        }
    }

    // HUPCL is already cleared by configure_serial_fd(); DTR stays high on close.
    close(fd);
    return found;
}

// The raw /dev/ttyUSBn fallback has no by-id name to filter on, so without
// this check it would open (and DTR-reset) EVERY serial device on the box.
bool raw_port_looks_like_pdh_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor, {"1a86", "CH340", "CH341", "USB_Serial"});
}
#endif  // _WIN32

}  // namespace

bool is_pdh_handshake_reply(const std::string& reply) { return strip_frame(reply) == kPdhHandshakeReply; }

std::string format_pdh_firmware(int version) {
    if (version >= 100 && version <= 999) {
        const std::string digits = std::to_string(version);
        return std::string(1, digits[0]) + "." + digits[1] + "." + digits[2];
    }
    return std::to_string(version);
}

std::optional<PdhState> parse_pdh_status_frame(const std::string& frame_in) {
    const std::string frame = strip_frame(frame_in);
    if (frame.size() < 2 || frame[0] != '*' || frame[1] != 'G') {
        return std::nullopt;
    }

    // Split on the tag letters, keeping empty pieces (C# String.Split semantics)
    // so the positional indexes match the vendor driver exactly.
    std::vector<std::string> f;
    std::string cur;
    for (std::size_t i = 2; i < frame.size(); ++i) {
        const char c = frame[i];
        if (kFrameDelimiters.find(c) != std::string_view::npos) {
            f.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    f.push_back(cur);
    if (f.size() < static_cast<std::size_t>(kFrameFieldCount)) {
        return std::nullopt;
    }
    if (!all_digits(f[0], 4) || !all_digits(f[1], 6)) {
        return std::nullopt;
    }

    PdhState s;
    for (std::size_t i = 0; i < s.dc.size(); ++i) {
        s.dc[i] = (f[0][i] != '0');
    }
    for (std::size_t i = 0; i < s.usb.size(); ++i) {
        s.usb[i] = (f[1][i] != '0');
    }
    s.aht20_attached = (f[2] == "1");
    s.ds18b20_attached = (f[3] == "1");

    s.dew6.enabled = (parse_int_or(f[4], 0) != 0);
    s.dew6.mode = mode_from_int(parse_int_or(f[5], 1));
    s.dew7.enabled = (parse_int_or(f[6], 0) != 0);
    s.dew7.mode = mode_from_int(parse_int_or(f[7], 1));
    s.dew6.manual_pwm = std::clamp(parse_int_or(f[8], 0), 0, kPdhDewPwmMax);
    s.dew7.manual_pwm = std::clamp(parse_int_or(f[9], 0), 0, kPdhDewPwmMax);

    // Absent sensors: the firmware's fields are meaningless (the vendor app
    // shows "NaN"), so apply the -127 degC / 0 % RH unconnected convention.
    s.lens_temp = s.ds18b20_attached ? parse_double_or(f[10], -127.0) : -127.0;
    s.ambient_temp = s.aht20_attached ? parse_double_or(f[11], -127.0) : -127.0;
    s.humidity = s.aht20_attached ? std::clamp(parse_double_or(f[12], 0.0), 0.0, 100.0) : 0.0;
    s.dew_point = s.aht20_attached ? parse_double_or(f[13], -127.0) : -127.0;

    s.input_voltage = std::max(0.0, parse_double_or(f[14], 0.0));
    s.output_current = std::max(0.0, parse_double_or(f[15], 0.0));
    s.output_power = std::max(0.0, parse_double_or(f[16], 0.0));
    s.valid = true;
    return s;
}

std::vector<GeminiPdhPortInfo> enumerate_gemini_pdh_ports() {
    std::vector<GeminiPdhPortInfo> results;

#ifndef _WIN32
    // Canonical paths already queued from by-id, so the raw-node pass never
    // re-opens (and DTR-resets) a port the by-id pass already claimed.
    std::set<std::string> probed;

    struct Candidate {
        std::string path;
        std::string name;  // by-id symlink name, empty for raw nodes
    };
    std::vector<Candidate> candidates;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            // The hub uses a CH340/CH341 USB-serial bridge (vendor 1a86): the
            // vendor's ReadMe requires the CH341 driver on Windows.
            const bool is_candidate =
                (name.find("USB_Serial") != std::string::npos) || (name.find("CH340") != std::string::npos) ||
                (name.find("CH341") != std::string::npos) || (name.find("1a86") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code ec;
            std::string resolved = std::filesystem::canonical(sym.path, ec).string();
            if (ec) continue;  // unplugged mid-scan -- skip, don't abort the enumeration
            probed.insert(resolved);
            if (is_serial_port_in_use(resolved)) continue;  // held by a connected device
            candidates.push_back({resolved, name});
        }
    }

    // Also probe raw /dev/ttyUSB* nodes: generic CH340 adapters report identical
    // descriptor strings with no serial number, so udev's by-id naming collides
    // when two are plugged in and only ONE gets a symlink.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;
        std::error_code ec;
        std::string resolved = std::filesystem::canonical(port, ec).string();
        if (ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_pdh_candidate(resolved)) continue;
        probed.insert(resolved);
        if (is_serial_port_in_use(resolved)) continue;
        candidates.push_back({resolved, ""});
    }

    // Probe every candidate concurrently: a non-responsive port costs the full
    // handshake timeout, so one thread per port bounds the scan to one port's
    // worst case (issue #218 lesson).
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        ALPACA_LOG_INFO("Gemini", "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") +
                                      " for a Power & Data Hubs Advanced 3...");
        workers.emplace_back([&, i] { found[i] = probe_pdh_port(candidates[i].path) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!found[i]) continue;
        ALPACA_LOG_INFO("Gemini", "Found Power & Data Hubs Advanced 3 on " + candidates[i].path);
        results.push_back({candidates[i].path, candidates[i].name});
    }
#endif

    return results;
}

class GeminiPdhProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    int connect(const PdhConnectionConfig& config) {
#ifdef _WIN32
        (void)config;
        throw AlpacaException("Gemini power hub serial support is POSIX-only", AlpacaError::DriverException);
#else
        std::lock_guard<std::mutex> transition(transition_mutex_);
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (connected_) {
                throw AlpacaException("Gemini power hub already connected; call disconnect() first",
                                      AlpacaError::InvalidOperation);
            }
            // configure_serial_fd() hardcodes B19200; reject any other rate
            // rather than silently ignoring it.
            if (config.baud_rate != kPdhBaudRate) {
                throw AlpacaException(
                    "Gemini power hub baud rate is fixed at 19200, got " + std::to_string(config.baud_rate),
                    AlpacaError::InvalidValue);
            }
            config_ = config;
            open_serial_locked();
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            polls_since_frame_ = 0;
            link_fault_.clear();
        }

        // The reader thread owns every read from here on (handshake included),
        // so an unsolicited status frame arriving mid-handshake is routed to
        // the cache instead of being mistaken for the >H# reply.
        reader_running_.store(true);
        reader_thread_ = std::thread([this] { reader_loop(); });

        int firmware = 0;
        try {
            bool success = false;
            for (int attempt = 0; attempt < kHandshakeRetries && !success; ++attempt) {
                if (attempt == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } else if (attempt == 1) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                try {
                    const std::string reply = request(">H#", 'H', kRequestTimeoutMs);
                    if (is_pdh_handshake_reply(reply)) {
                        success = true;
                    } else {
                        ALPACA_LOG_WARN("Gemini", "Power hub handshake attempt " + std::to_string(attempt + 1) +
                                                      " got an unexpected identity: " + reply);
                    }
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("Gemini", "Power hub handshake attempt " + std::to_string(attempt + 1) +
                                                  " failed: " + e.what());
                }
            }
            if (!success) {
                throw AlpacaException(
                    "Gemini power hub handshake failed after " + std::to_string(kHandshakeRetries) + " attempts",
                    AlpacaError::NotConnected);
            }

            const std::string version_reply = request(">V#", 'V', kRequestTimeoutMs);
            firmware = parse_int_or(strip_frame(version_reply).substr(2), -1);
            if (firmware < 0) {
                throw AlpacaException("Gemini power hub returned an unparseable firmware version: " + version_reply,
                                      AlpacaError::NotConnected);
            }
            if (firmware < kPdhMinFirmware) {
                // Vendor behaviour: the frame layout below is only known for
                // 3.0.8+, so refuse rather than parse garbage.
                throw AlpacaException("Gemini power hub firmware " + format_pdh_firmware(firmware) +
                                          " is older than the minimum " + format_pdh_firmware(kPdhMinFirmware) +
                                          "; upgrade with the vendor's FirmwareUpgradeTool",
                                      AlpacaError::NotConnected);
            }

            // First status frame: the request is fulfilled by any >G frame,
            // streamed or in reply, and lands in the cache via the reader.
            const std::string status_reply = request(">G#", 'G', kRequestTimeoutMs);
            if (!parse_pdh_status_frame(status_reply).has_value()) {
                throw AlpacaException("Gemini power hub returned an unparseable status frame: " + status_reply,
                                      AlpacaError::NotConnected);
            }
        } catch (...) {
            teardown_locked_free();
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            connected_ = true;
        }
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            firmware_ = format_pdh_firmware(firmware);
        }
        ALPACA_LOG_INFO("Gemini", "Power & Data Hubs Advanced 3 connected on " + config_.serial_port + " (firmware " +
                                      format_pdh_firmware(firmware) + ")");
        return firmware;
#endif
    }

    void disconnect() {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        teardown_locked_free();
    }

    /// Forget the cached firmware now, ahead of disconnect(). The driver
    /// calls this before it stores connected_ = false (issue #387): the
    /// teardown clears the cache only after joining the reader, which can
    /// sit in a 500 ms read, and the driver's firmware getter relies on the
    /// cache being empty whenever the driver reports disconnected
    /// (AGENTS.md) rather than consulting connected_.
    void clear_firmware() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        firmware_.clear();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return connected_;
    }

    PdhState get_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }

    std::optional<std::string> get_firmware() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    std::optional<std::string> link_fault() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (link_fault_.empty()) {
            return std::nullopt;
        }
        return link_fault_;
    }

    void set_output(int channel, bool on) {
        if (channel < kPdhOutputChannelMin || channel > kPdhOutputChannelMax) {
            throw AlpacaException("Output channel must be within [1, 11], got " + std::to_string(channel),
                                  AlpacaError::InvalidValue);
        }
        send_command((on ? ">O" : ">C") + std::to_string(channel) + "#");
    }

    void set_dew_manual_pwm(int channel, int percent) {
        validate_dew_channel(channel);
        if (percent < 0 || percent > kPdhDewPwmMax) {
            throw AlpacaException("Dew heater PWM must be within [0, 100], got " + std::to_string(percent),
                                  AlpacaError::InvalidValue);
        }
        send_command((channel == kPdhDewChannelFirst ? ">X" : ">Y") + std::to_string(percent) + "#");
    }

    void set_dew_enabled(int channel, bool on) {
        validate_dew_channel(channel);
        // >Z10#/>Z11# (DEW6), >Z20#/>Z21# (DEW7)
        send_command(">Z" + std::to_string(channel - 5) + (on ? "1" : "0") + "#");
    }

    void set_dew_mode(int channel, PdhDewMode mode) {
        validate_dew_channel(channel);
        // >M10#/>M11#/>M12# (DEW6), >M20#/>M21#/>M22# (DEW7)
        send_command(">M" + std::to_string(channel - 5) + std::to_string(static_cast<int>(mode)) + "#");
    }

    void request_status() { send_command(">G#"); }

private:
    static void validate_dew_channel(int channel) {
        if (channel < kPdhDewChannelFirst || channel > kPdhDewChannelLast) {
            throw AlpacaException("Dew heater channel must be 6 or 7, got " + std::to_string(channel),
                                  AlpacaError::InvalidValue);
        }
    }

    // Public set path: requires a completed connect().
    void send_command(const std::string& cmd) {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!connected_) {
            throw AlpacaException("Gemini power hub not connected", AlpacaError::NotConnected);
        }
        write_locked(cmd);
#else
        (void)cmd;
        throw AlpacaException("Gemini power hub serial support is POSIX-only", AlpacaError::DriverException);
#endif
    }

#ifndef _WIN32
    void write_locked(const std::string& cmd) {
        if (serial_fd_ < 0) {
            throw AlpacaException("Gemini power hub serial port is not open", AlpacaError::NotConnected);
        }
        ALPACA_LOG_TRACE("Gemini", "Power hub command: " + cmd);
        if (!write_line(serial_fd_, cmd)) {
            throw AlpacaException("Serial write failed: " + util::errno_string(errno), AlpacaError::DriverException);
        }
    }

    // Send a query and wait for the reply whose command letter matches.
    // Serialized so only one request is ever pending; works before connect()
    // completes (handshake) because it gates on the open fd, not connected_.
    std::string request(const std::string& cmd, char letter, int timeout_ms) {
        std::lock_guard<std::mutex> rq(request_mutex_);
        {
            std::lock_guard<std::mutex> pl(pending_mutex_);
            pending_letter_ = letter;
            pending_reply_.reset();
            pending_active_ = true;
        }
        try {
            std::lock_guard<std::mutex> lock(io_mutex_);
            write_locked(cmd);
        } catch (...) {
            std::lock_guard<std::mutex> pl(pending_mutex_);
            pending_active_ = false;
            throw;
        }
        std::unique_lock<std::mutex> pl(pending_mutex_);
        const bool got =
            pending_cv_.wait_for(pl, std::chrono::milliseconds(timeout_ms), [&] { return pending_reply_.has_value(); });
        pending_active_ = false;
        std::optional<std::string> reply = std::move(pending_reply_);
        pending_reply_.reset();
        if (!got || !reply.has_value()) {
            throw AlpacaException("Timeout waiting for the reply to " + cmd, AlpacaError::DriverException);
        }
        return *reply;
    }

    // Route one received frame: status frames to the cache, everything else to
    // the pending request if its letter matches.
    void dispatch_frame(const std::string& frame) {
        if (frame.size() < 2 || frame[0] != '*') {
            ALPACA_LOG_TRACE("Gemini", "Power hub: ignoring malformed frame '" + frame + "'");
            return;
        }
        if (frame[1] == 'G') {
            auto state = parse_pdh_status_frame(frame);
            if (state.has_value()) {
                bool restored = false;
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex_);
                    state_ = *state;
                    last_frame_ = std::chrono::steady_clock::now();
                    polls_since_frame_ = 0;
                    restored = !link_fault_.empty();
                    link_fault_.clear();
                }
                if (restored) {
                    ALPACA_LOG_INFO("Gemini", "Power hub: serial link restored, status frames are flowing again");
                }
            } else {
                ALPACA_LOG_DEBUG("Gemini", "Power hub: unparseable status frame '" + frame + "'");
            }
        }
        std::lock_guard<std::mutex> pl(pending_mutex_);
        if (pending_active_ && !pending_reply_.has_value() && frame[1] == pending_letter_) {
            pending_reply_ = frame;
            pending_cv_.notify_all();
        } else if (frame[1] != 'G') {
            // Set commands are fire-and-forget; whatever the firmware acks
            // with is informational only.
            ALPACA_LOG_TRACE("Gemini", "Power hub: unsolicited reply '" + frame + "'");
        }
    }

    // Consume the port: frames are dispatched as they complete, and a fresh
    // >G# is requested whenever the cache goes stale so Switch reads never
    // touch the wire.
    void reader_loop() {
        std::string carry;
        auto last_poll = std::chrono::steady_clock::now();
        while (reader_running_.load()) {
            int fd;
            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                fd = serial_fd_;
            }
            if (fd < 0) {
                return;  // closed under us
            }
            auto frame = read_frame(fd, 500, carry);
            if (frame.has_value()) {
                dispatch_frame(*frame);
            }

            const auto now = std::chrono::steady_clock::now();
            bool stale = false;
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                stale = (now - last_frame_) > std::chrono::milliseconds(kStatusPollMs);
            }
            if (stale && (now - last_poll) > std::chrono::milliseconds(kStatusPollMs)) {
                last_poll = now;
                std::optional<std::string> write_error;
                bool polled = false;
                {
                    std::lock_guard<std::mutex> lock(io_mutex_);
                    if (connected_ && serial_fd_ >= 0) {
                        polled = true;
                        if (!write_line(serial_fd_, ">G#")) {
                            write_error = util::errno_string(errno);
                        }
                    }
                }
                if (polled) {
                    record_poll_outcome(write_error);
                }
            }
        }
    }

    // Link health (issue #237): the cache is only as good as the link that
    // fills it. Every >G# poll that goes out while the cache is stale counts;
    // a status frame resets the count (dispatch_frame). Past kLinkFaultPolls
    // the link is latched faulted -- the cache is invalidated and the driver
    // refuses to serve it -- until a frame arrives again. Polling continues
    // at the normal cadence so recovery (a re-plugged hub on the same node)
    // is noticed without a reconnect.
    void record_poll_outcome(const std::optional<std::string>& write_error) {
        std::string latched;
        std::string transient;
        int polls = 0;
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            polls = ++polls_since_frame_;
            const bool already_faulted = !link_fault_.empty();
            std::string detail;
            if (write_error.has_value()) {
                detail = "status poll write failed: " + *write_error;
            } else {
                detail = "no status frame for " + std::to_string(polls_since_frame_) + " consecutive polls (" +
                         std::to_string(polls_since_frame_ * kStatusPollMs / 1000) + " s)";
            }
            if (polls_since_frame_ >= kLinkFaultPolls) {
                if (!already_faulted) {
                    latched = detail;
                }
                link_fault_ = detail;
                state_.valid = false;
            } else if (write_error.has_value() || polls_since_frame_ > 1) {
                // First unanswered poll after a frame is normal cadence; a
                // failed write or a second silent poll is worth a warning.
                transient = detail;
            }
        }
        if (!latched.empty()) {
            ALPACA_LOG_ERROR("Gemini", "Power hub: serial link faulted (" + latched +
                                           "); cached status is invalid and reads will fail until frames resume");
        } else if (!transient.empty()) {
            ALPACA_LOG_WARN("Gemini", "Power hub: " + transient + " (" + std::to_string(polls) + "/" +
                                          std::to_string(kLinkFaultPolls) + " before the link is faulted)");
        } else if (write_error.has_value()) {
            ALPACA_LOG_DEBUG("Gemini", "Power hub: status poll write failed: " + *write_error);
        }
    }

    void open_serial_locked() {
        // Claim the port in the registry BEFORE opening it so a concurrent
        // auto-detect scan (ours, or the focuser's/flat panel's) can't slip in
        // between check and open. Canonical path so by-id and ttyUSBn compare equal.
        std::error_code path_ec;
        std::string canonical_path = std::filesystem::canonical(config_.serial_port, path_ec).string();
        opened_port_ = path_ec ? config_.serial_port : canonical_path;
        mark_serial_port_open(opened_port_);

        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + util::errno_string(errno) + ")",
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

    // Full teardown; caller holds transition_mutex_. Marks disconnected under
    // io_mutex_ FIRST so no command can write after the join, joins the reader
    // WITHOUT io_mutex_ held (the reader takes it each iteration), then closes.
    void teardown_locked_free() {
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            connected_ = false;
        }
        reader_running_.store(false);
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
        {
            std::lock_guard<std::mutex> pl(pending_mutex_);
            pending_active_ = false;
            pending_reply_.reset();
            pending_cv_.notify_all();
        }
        std::lock_guard<std::mutex> lock(io_mutex_);
        {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            state_ = PdhState{};
            firmware_.clear();
            last_frame_ = {};
            polls_since_frame_ = 0;
            link_fault_.clear();
        }
#ifndef _WIN32
        close_serial_locked();
#endif
    }

    std::mutex transition_mutex_;     // serializes connect()/disconnect()
    mutable std::mutex io_mutex_;     // guards serial fd + connected_/config_
    mutable std::mutex state_mutex_;  // guards state_/firmware_/last_frame_
    PdhConnectionConfig config_;
    bool connected_ = false;
    std::string opened_port_;  // path registered in the in-use registry while open

    PdhState state_;
    std::string firmware_;  // "3.0.8", from >V#
    std::chrono::steady_clock::time_point last_frame_{};
    int polls_since_frame_ = 0;  // >G# polls sent since the last status frame (issue #237)
    std::string link_fault_;     // non-empty while the link is latched faulted; the reason

    std::mutex request_mutex_;  // one in-flight request at a time
    std::mutex pending_mutex_;  // guards the pending_* fields
    std::condition_variable pending_cv_;
    char pending_letter_ = 0;
    bool pending_active_ = false;
    std::optional<std::string> pending_reply_;

    std::atomic<bool> reader_running_{false};
    std::thread reader_thread_;

#ifndef _WIN32
    int serial_fd_ = -1;
#endif
};

// --- GeminiPdhProtocolWrapper public interface forwarding ---

GeminiPdhProtocolWrapper::GeminiPdhProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

GeminiPdhProtocolWrapper::~GeminiPdhProtocolWrapper() = default;

int GeminiPdhProtocolWrapper::connect(const PdhConnectionConfig& config) { return impl_->connect(config); }

void GeminiPdhProtocolWrapper::disconnect() { impl_->disconnect(); }
void GeminiPdhProtocolWrapper::clear_firmware() { impl_->clear_firmware(); }

bool GeminiPdhProtocolWrapper::is_connected() const { return impl_->is_connected(); }

PdhState GeminiPdhProtocolWrapper::get_state() const { return impl_->get_state(); }

std::optional<std::string> GeminiPdhProtocolWrapper::get_firmware() const { return impl_->get_firmware(); }

std::optional<std::string> GeminiPdhProtocolWrapper::link_fault() const { return impl_->link_fault(); }

void GeminiPdhProtocolWrapper::set_output(int channel, bool on) { impl_->set_output(channel, on); }

void GeminiPdhProtocolWrapper::set_dew_manual_pwm(int channel, int percent) {
    impl_->set_dew_manual_pwm(channel, percent);
}

void GeminiPdhProtocolWrapper::set_dew_enabled(int channel, bool on) { impl_->set_dew_enabled(channel, on); }

void GeminiPdhProtocolWrapper::set_dew_mode(int channel, PdhDewMode mode) { impl_->set_dew_mode(channel, mode); }

void GeminiPdhProtocolWrapper::request_status() { impl_->request_status(); }

}  // namespace alpacacore::vendor::gemini

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

// Protocol reference: INDI 3rd-party indi-qhy/qhy_focuser.cpp (QHY's own
// contribution) and INDIGO indigo_focuser_qhy.c, cross-checked against a
// Q-Focuser (firmware 20231207, board 208) on 2026-09-13.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::qhy {

namespace {

constexpr const char* kLogTag = "QHY";
constexpr int kMaxReplyLen = 256;  // INDI's USB_CDC_RX_LEN is 128; the longest seen reply is ~80 bytes
constexpr int kHandshakeRetries = 3;
constexpr int kHandshakeTimeoutMs = 3000;  // full runtime budget for the first exchange on a fresh open
// The GD32 CDC-ACM firmware transmits a reply only on a USB OUT packet
// boundary, and computes a command's reply AFTER transmitting on that
// command's own OUT, so the reply to command N does not reach the host until
// the NEXT OUT is written -- a naive "write, read" is always exactly one
// reply behind. (It looked fine on the dev VM only because VMware's USB proxy
// coalesced the traffic; a Raspberry Pi's native xHCI shows the real one-
// behind behaviour.) transact_locked handles this by writing the command,
// tcdrain-ing it out as its own packet, then writing a newline "kick" -- the
// firmware parser waits for '{' and ignores the newline, but its OUT packet
// clocks out the pending reply -- and reading until the idx we asked for
// appears, skipping the previous command's reply that the kick shakes loose
// first. A newline never computes a new reply, so it only re-transmits the
// last one; alignment is therefore done per command and never by draining in
// a loop (which would re-read the same reply forever).
constexpr int kKickSliceMs = 200;  // fallback read window before sending another kick
// Floor on the gap between fallback kicks. read_json_object() returns early
// (not just at the slice deadline) on a poll() error or an overlong reply
// with no closing brace; without a floor those paths re-kick in a tight loop
// until the transaction deadline (issue #527).
constexpr int kEmptyReplyPauseMs = 50;
constexpr int kProbeTimeoutMs = 1500;
constexpr int kReadPollMs = 10;

// Command ids (INDI create_cmd()).
constexpr int kCmdVersion = 1;
constexpr int kCmdAbort = 3;
constexpr int kCmdTelemetry = 4;
constexpr int kCmdPosition = 5;
constexpr int kCmdAbsoluteMove = 6;
constexpr int kCmdReverse = 7;
constexpr int kCmdHoldForce = 12;
constexpr int kCmdSpeed = 13;
constexpr int kCmdHoldCurrent = 16;
// {"cmd_id":19,"pdn_d":N} (power-down mode) exists in INDI but is not
// answered by every firmware; deliberately not used here.
// {"idx":-1} is an unsolicited reboot notice; replies carrying it are skipped.

std::string printable(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char ch : raw) {
        out += std::isprint(ch) ? static_cast<char>(ch) : '.';
    }
    return out;
}

bool parse_int(const std::map<std::string, std::string>& fields, const char* key, std::int32_t& out) {
    auto it = fields.find(key);
    if (it == fields.end() || it->second.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || *end != '\0') return false;
    out = static_cast<std::int32_t>(value);
    return true;
}

bool parse_device_info(const std::map<std::string, std::string>& fields, QFocuserDeviceInfo& out) {
    std::int32_t idx = 0;
    if (!parse_int(fields, "idx", idx) || idx != kCmdVersion) return false;
    std::int32_t version = 0;
    if (!parse_int(fields, "version", version)) return false;  // INDI: missing "version" = not a Q-Focuser
    auto id = fields.find("id");
    if (id == fields.end()) return false;
    out.id = printable(id->second);
    out.firmware = version;
    std::int32_t bv = 0;
    out.board_version = parse_int(fields, "bv", bv) ? bv : 0;
    return true;
}

#ifndef _WIN32
bool configure_tty(int fd) {
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) return false;
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
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
    tty.c_cc[VTIME] = 0;  // read() returns at once with whatever is buffered; read_json_object() paces with poll()
    if (tcsetattr(fd, TCSANOW, &tty) != 0) return false;
    // The port is opened O_NONBLOCK so open() cannot hang on a CDC-ACM device
    // that has not finished enumerating, but it must NOT stay that way: with
    // O_NONBLOCK set, the real GD32 CDC-ACM port answers the first write()
    // with EAGAIN while its USB bulk endpoint is still idle (seen on
    // hardware 2026-09-13), which write_all() reports as a hard error. A pty
    // fake never does this, so the unit tests cannot catch it.
    return util::clear_nonblocking(fd);
}

// Reads one {...} object within timeout_ms. Anything before the opening brace
// (a stale partial reply, a reboot-notice fragment) is discarded. Sets
// link_dead = true (and returns
// empty at once) when the port has gone away -- POLLHUP/POLLERR/POLLNVAL, EOF,
// or a hard errno (EIO/ENXIO/ENODEV/EBADF) -- so the caller can fail the
// transaction immediately instead of re-kicking at 100% CPU until the deadline
// (a bare "return {}" on those looks like an ordinary silent slice). An
// ordinary empty/timeout leaves link_dead false.
std::string read_json_object(int fd, int timeout_ms, bool& link_dead) {
    link_dead = false;
    std::string buf;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int r = poll(&pfd, 1, kReadPollMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) link_dead = true;  // fd closed underneath us
            return {};
        }
        if (r == 0) continue;
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
            link_dead = true;  // peer hung up / node removed, with nothing left to read
            return {};
        }
        char ch = 0;
        const ssize_t n = read(fd, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (n == 0) {
            link_dead = true;  // EOF: the tty/node is gone
            return {};
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (errno == EIO || errno == ENXIO || errno == ENODEV || errno == EBADF) {
                link_dead = true;
            }
            return {};
        }
        if (buf.empty()) {
            if (ch != '{') continue;  // skip leading garbage
        }
        buf += ch;
        if (ch == '}') return buf;
        if (buf.size() >= static_cast<std::size_t>(kMaxReplyLen)) return {};
    }
    return {};
}
#endif

}  // namespace

std::map<std::string, std::string> parse_qfocuser_reply(std::string_view text) {
    std::map<std::string, std::string> fields;
    std::size_t i = 0;
    auto skip_ws = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    auto read_string = [&](std::string& out) -> bool {
        if (i >= text.size() || text[i] != '"') return false;
        ++i;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\') {
                // Escapes only occur in the opaque MCU id; collapse each to
                // a placeholder rather than decoding \uXXXX into raw bytes.
                ++i;
                if (i < text.size() && text[i] == 'u') i += 4;
                out += '.';
            } else {
                out += text[i];
            }
            ++i;
        }
        if (i >= text.size()) return false;
        ++i;  // closing quote
        return true;
    };

    skip_ws();
    if (i >= text.size() || text[i] != '{') return {};
    ++i;
    skip_ws();
    if (i < text.size() && text[i] == '}') return fields;  // empty object
    while (i < text.size()) {
        skip_ws();
        std::string key;
        if (!read_string(key)) return {};
        skip_ws();
        if (i >= text.size() || text[i] != ':') return {};
        ++i;
        skip_ws();
        std::string value;
        if (i < text.size() && text[i] == '"') {
            if (!read_string(value)) return {};
        } else {
            while (i < text.size() && text[i] != ',' && text[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(text[i]))) {
                value += text[i++];
            }
            if (value.empty()) return {};
        }
        fields[key] = value;
        skip_ws();
        if (i >= text.size()) return {};
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == '}') return fields;
        return {};
    }
    return {};
}

// Probe a serial port with the version handshake. Returns true (and fills
// `info`) only if a reply with idx 1 and a "version" field comes back.
static bool probe_port(const std::string& port_path, QFocuserDeviceInfo& info) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;
    if (!configure_tty(fd)) {
        close(fd);
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        // The GD32 CDC-ACM bridge needs no DTR settle, but give the first
        // open a moment before the port is flushed and written.
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 500));
        tcflush(fd, TCIOFLUSH);
        const std::string cmd = "{\"cmd_id\":1}";
        if (!util::write_all(fd, cmd.c_str(), cmd.size())) continue;
        // Same one-behind handling as transact_locked(): drain the command out
        // as its own packet and kick so the reply is clocked out on this
        // attempt rather than only on the next one. Without it the probe burns
        // the full kProbeTimeoutMs on attempt 0 on real hardware (~2 s per
        // candidate port); the kick brings it back to the handshake latency.
        tcdrain(fd);
        static const char kick = '\n';
        (void)util::write_all(fd, &kick, 1);
        bool link_dead = false;
        const std::string reply = read_json_object(fd, kProbeTimeoutMs, link_dead);
        if (link_dead) break;  // node vanished mid-probe; not our device
        if (reply.empty()) continue;
        QFocuserDeviceInfo parsed;
        if (parse_device_info(parse_qfocuser_reply(reply), parsed)) {
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
// The raw /dev/ttyACMn fallback has no by-id symlink name to filter on;
// restrict it to the GigaDevice GD32 CDC-ACM interface the Q-Focuser ships
// with so the scan never opens an unrelated ACM device (a mount hand
// controller, a GPS puck) just to send it JSON.
bool raw_port_looks_like_qfocuser_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor, {"28e9", "GigaDevice", "GD32"});
}
}  // namespace
#endif

std::vector<QFocuserPortInfo> enumerate_qfocuser_ports() {
    std::vector<QFocuserPortInfo> results;

#ifndef _WIN32
    std::set<std::string> probed;

    struct Candidate {
        std::string path;
        std::string name;
    };
    std::vector<Candidate> candidates;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            // udev name seen on hardware: usb-GigaDevice_GD32-CDC_ACM_<serial>-if00
            const bool is_candidate = (name.find("GigaDevice") != std::string::npos) ||
                                      (name.find("GD32") != std::string::npos) ||
                                      (name.find("28e9") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            if (alpacacore::util::is_serial_port_in_use(resolved)) continue;
            probed.insert(resolved);
            candidates.push_back({resolved, name});
        }
    }

    // Raw /dev/ttyACM* pass, run unconditionally (not only when by-id is
    // empty): two identical GigaDevice adapters collide on their by-id name
    // and only one gets a symlink, so the other is visible only here. It is
    // filtered by USB descriptor and deduped against the by-id pass above.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyACM" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (alpacacore::util::is_serial_port_in_use(resolved)) continue;
        if (!raw_port_looks_like_qfocuser_candidate(resolved)) continue;
        probed.insert(resolved);
        candidates.push_back({resolved, ""});
    }

    // Probe candidates concurrently so a silent port costs one timeout, not
    // one per adapter (issue #218 lesson).
    std::vector<QFocuserDeviceInfo> infos(candidates.size());
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        ALPACA_LOG_INFO(kLogTag,
                        "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") + " for Q-Focuser...");
        workers.emplace_back([&, i] { found[i] = probe_port(candidates[i].path, infos[i]) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!found[i]) continue;
        const auto& c = candidates[i];
        ALPACA_LOG_INFO(kLogTag, "Found Q-Focuser on " + c.path + " (firmware " + std::to_string(infos[i].firmware) +
                                     ", board " + std::to_string(infos[i].board_version) + ")");
        results.push_back({c.path, c.name, infos[i]});
    }
#endif

    return results;
}

class QFocuserProtocolWrapper::Impl {
public:
    Impl() = default;
    ~Impl() { disconnect(); }

    QFocuserDeviceInfo connect(const QFocuserConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            throw AlpacaException("Q-Focuser already connected", AlpacaError::InvalidOperation);
        }
        config_ = config;
        link_lost_.store(false);
        open_port_locked();

        QFocuserDeviceInfo info;
        bool success = false;
        in_connect_ = true;
        for (int attempt = 0; attempt < kHandshakeRetries && !success; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 500));
            try {
                // INDI retries once when the first reply is not idx 1: the
                // firmware can answer a fresh open with a stale or reboot
                // ({"idx":-1}) message before it services the command.
                auto fields = transact_locked("{\"cmd_id\":1}", kCmdVersion, kHandshakeTimeoutMs);
                success = parse_device_info(fields, info);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag,
                                "Q-Focuser handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }
        in_connect_ = false;
        if (!success) {
            close_port_locked();
            throw AlpacaException("Q-Focuser handshake failed after " + std::to_string(kHandshakeRetries) +
                                      " attempts on " + config_.serial_port,
                                  AlpacaError::NotConnected);
        }

        connected_ = true;
        ALPACA_LOG_INFO(kLogTag, "Q-Focuser connected on " + config_.serial_port + " (firmware " +
                                     std::to_string(info.firmware) + ", board " + std::to_string(info.board_version) +
                                     ", id " + info.id + ")");
        return info;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        close_port_locked();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    // Lock-free on purpose: the driver's get_connected() is a FAST-timing
    // property and must not queue behind an in-flight transaction.
    bool link_alive() const noexcept { return !link_lost_.load(); }

    std::int32_t get_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        auto fields = transact_locked("{\"cmd_id\":5}", kCmdPosition, config_.serial_timeout_ms);
        std::int32_t pos = 0;
        if (!parse_int(fields, "pos", pos)) {
            throw AlpacaException("Q-Focuser position reply missing \"pos\"", AlpacaError::DriverException);
        }
        return pos;
    }

    QFocuserTelemetry get_telemetry() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        auto fields = transact_locked("{\"cmd_id\":4}", kCmdTelemetry, config_.serial_timeout_ms);
        std::int32_t o_t = 0, c_t = 0, c_r = 0;
        if (!parse_int(fields, "o_t", o_t) || !parse_int(fields, "c_t", c_t) || !parse_int(fields, "c_r", c_r)) {
            throw AlpacaException("Q-Focuser telemetry reply missing o_t/c_t/c_r", AlpacaError::DriverException);
        }
        QFocuserTelemetry t;
        t.external_temperature_c = o_t / 1000.0;
        t.chip_temperature_c = c_t / 1000.0;
        t.voltage_v = c_r / 10.0;
        return t;
    }

    void move_to(std::int32_t position) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        transact_locked("{\"cmd_id\":6,\"tar\":" + std::to_string(position) + "}", kCmdAbsoluteMove,
                        config_.serial_timeout_ms);
    }

    void halt() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        // INDIGO accepts idx 3 or idx 5 here: some firmware answers the
        // abort with a position report.
        transact_locked("{\"cmd_id\":3}", kCmdAbort, config_.serial_timeout_ms, kCmdPosition);
    }

    void set_reverse(bool reversed) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        transact_locked(std::string("{\"cmd_id\":7,\"rev\":") + (reversed ? "1" : "0") + "}", kCmdReverse,
                        config_.serial_timeout_ms);
    }

    void set_speed(int speed) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        transact_locked("{\"cmd_id\":13,\"speed\":" + std::to_string(speed) + "}", kCmdSpeed,
                        config_.serial_timeout_ms);
    }

    void set_hold_force(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        transact_locked(std::string("{\"cmd_id\":12,\"force\":") + (enabled ? "1" : "0") + "}", kCmdHoldForce,
                        config_.serial_timeout_ms);
    }

    void set_hold_current(int ihold, int irun) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        transact_locked("{\"cmd_id\":16,\"ihold\":" + std::to_string(ihold) + ",\"irun\":" + std::to_string(irun) + "}",
                        kCmdHoldCurrent, config_.serial_timeout_ms);
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Q-Focuser not connected", AlpacaError::NotConnected);
        }
    }

    void open_port_locked() {
#ifndef _WIN32
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(config_.serial_port, canon_ec).string();
        if (canon_ec) resolved = config_.serial_port;
        if (!util::try_mark_serial_port_open(resolved)) {
            throw AlpacaException("Serial port " + config_.serial_port + " is already in use by another device",
                                  AlpacaError::NotConnected);
        }
        opened_port_ = resolved;

        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            const int err = errno;
            util::mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + util::errno_string(err) + ")",
                AlpacaError::NotConnected);
        }
        if (!configure_tty(serial_fd_)) {
            close_port_locked();
            throw AlpacaException("Failed to configure serial port " + config_.serial_port,
                                  AlpacaError::DriverException);
        }
        tcflush(serial_fd_, TCIOFLUSH);
#else
        throw AlpacaException("Q-Focuser driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    void close_port_locked() {
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
        if (!opened_port_.empty()) {
            util::mark_serial_port_closed(opened_port_);
            opened_port_.clear();
        }
#endif
    }

    // Send one command and return the parsed reply whose "idx" is
    // `expected_idx` (or `alt_idx`). Replies with any other idx (a stale
    // answer the kick shook loose first, or a {"idx":-1} reboot notice) are
    // skipped until the deadline.
    // The port is gone (unplug / re-enumeration): the #445 shape. A removed
    // node never comes back on this fd, and holding it keeps the kernel from
    // reusing the name when the focuser is replugged, so release it now,
    // latch link_lost_ (a lock-free flag the driver's Connected getter reads,
    // since this is a request/response wrapper with no background reader to
    // keep link health current), and fail the call with NotConnected. Only a
    // new connect() clears the latch. While connect() itself is still inside
    // its handshake loop the port is NOT released and nothing is latched: a
    // half-enumerated CDC-ACM node can POLLHUP once at open time, and the
    // retry loop exists to absorb exactly that; connect() closes the port
    // itself if every attempt fails. Caller holds mutex_. Always throws.
    [[noreturn]] void fail_link_locked() {
        if (!in_connect_) {
            ALPACA_LOG_ERROR(kLogTag, "Q-Focuser link lost on " + config_.serial_port + " (port released)");
            link_lost_.store(true);
            connected_ = false;
            close_port_locked();
        }
        throw AlpacaException("Q-Focuser link lost on " + config_.serial_port, AlpacaError::NotConnected);
    }

    std::map<std::string, std::string> transact_locked(const std::string& cmd, int expected_idx, int timeout_ms,
                                                       int alt_idx = -2) {
#ifndef _WIN32
        if (serial_fd_ < 0) {
            // The port was released by an earlier link-loss detection (below)
            // and nothing has reconnected since.
            throw AlpacaException("Q-Focuser link lost on " + config_.serial_port, AlpacaError::NotConnected);
        }
        ALPACA_LOG_TRACE(kLogTag, "Q-Focuser command: " + cmd);
        tcflush(serial_fd_, TCIOFLUSH);
        if (!util::write_all(serial_fd_, cmd.c_str(), cmd.size())) {
            const int err = errno;
            // A write is as good a witness of a removed node as a read
            // (driver-lifecycle.instructions.md #445: EIO/ENXIO/ENODEV/EBADF from either side).
            if (err == EIO || err == ENXIO || err == ENODEV || err == EBADF) fail_link_locked();
            throw AlpacaException("Q-Focuser write failed: " + util::errno_string(err), AlpacaError::DriverException);
        }
        // tcdrain forces the command out as its OWN USB OUT packet before the
        // kick is written; without it the kernel coalesces the command and
        // the kick into one packet and the reply is never clocked out (the
        // device only transmits on a packet boundary). Draining then kicking
        // brings the reply back in ~1 ms on a Raspberry Pi, versus the ~150 ms
        // a wait-then-kick costs -- which is what blew ConformU's 0.1 s FAST
        // target on DeviceState/IsMoving.
        tcdrain(serial_fd_);
        static const char kick = '\n';
        (void)util::write_all(serial_fd_, &kick, 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            bool link_dead = false;
            const auto slice_start = std::chrono::steady_clock::now();
            const std::string reply = read_json_object(serial_fd_, kKickSliceMs, link_dead);
            if (link_dead) fail_link_locked();
            if (reply.empty()) {
                // Fallback: the proactive kick did not clock the reply out
                // (should not happen once the command packet is drained), so
                // send another. A newline is ignored by the firmware parser
                // but its OUT packet transmits the pending reply. If the read
                // came back before its slice elapsed (poll error, or a stream
                // of garbage with no closing brace), pace the re-kick so a
                // present-but-babbling tty cannot spin a core to the deadline.
                const auto slice_elapsed = std::chrono::steady_clock::now() - slice_start;
                if (slice_elapsed < std::chrono::milliseconds(kEmptyReplyPauseMs)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kEmptyReplyPauseMs) - slice_elapsed);
                }
                (void)util::write_all(serial_fd_, &kick, 1);
                continue;
            }
            ALPACA_LOG_TRACE(kLogTag, "Q-Focuser reply: " + printable(reply));
            auto fields = parse_qfocuser_reply(reply);
            std::int32_t idx = 0;
            if (!parse_int(fields, "idx", idx)) {
                ALPACA_LOG_DEBUG(kLogTag, "Q-Focuser reply without idx ignored: " + printable(reply));
                continue;
            }
            // Return on the first matching idx. Known bounded staleness: when
            // two consecutive commands share an idx (e.g. back-to-back
            // {"cmd_id":5} position polls), the command's own OUT first
            // re-transmits the PREVIOUS reply -- same idx -- and this returns
            // it, so the value lags by exactly one poll while the kick's fresh
            // reply is dropped by the next command's tcflush. Harmless in
            // practice (a one-poll lag in Position/IsMoving during a move,
            // ConformU-clean) because distinct-idx commands never collide and
            // the driver's poll sends cmd_id 5 then cmd_id 4 each tick, so two
            // position polls are never adjacent; revisit if the poll sends
            // two commands with one idx back to back.
            if (idx == expected_idx || idx == alt_idx) return fields;
            ALPACA_LOG_DEBUG(kLogTag, "Q-Focuser reply idx " + std::to_string(idx) + " while waiting for " +
                                          std::to_string(expected_idx) + "; skipping");
        }
        throw AlpacaException("Q-Focuser timed out waiting for reply to " + cmd, AlpacaError::DriverException);
#else
        (void)cmd;
        (void)expected_idx;
        (void)timeout_ms;
        (void)alt_idx;
        throw AlpacaException("Q-Focuser driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    mutable std::mutex mutex_;
    QFocuserConnectionConfig config_;
    bool connected_ = false;
    // Latched by transact_locked() when the port dies mid-session; cleared by
    // connect(). Atomic so link_alive() never takes mutex_ (issue #527).
    std::atomic<bool> link_lost_{false};
    bool in_connect_ = false;  // guarded by mutex_; see fail_link_locked()
#ifndef _WIN32
    int serial_fd_ = -1;
    std::string opened_port_;  // canonical path registered in the serial-port registry
#endif
};

QFocuserProtocolWrapper::QFocuserProtocolWrapper() : impl_(std::make_unique<Impl>()) {}
QFocuserProtocolWrapper::~QFocuserProtocolWrapper() = default;

QFocuserDeviceInfo QFocuserProtocolWrapper::connect(const QFocuserConnectionConfig& config) {
    return impl_->connect(config);
}
void QFocuserProtocolWrapper::disconnect() { impl_->disconnect(); }
bool QFocuserProtocolWrapper::is_connected() const { return impl_->is_connected(); }
bool QFocuserProtocolWrapper::link_alive() const noexcept { return impl_->link_alive(); }
std::int32_t QFocuserProtocolWrapper::get_position() { return impl_->get_position(); }
QFocuserTelemetry QFocuserProtocolWrapper::get_telemetry() { return impl_->get_telemetry(); }
void QFocuserProtocolWrapper::move_to(std::int32_t position) { impl_->move_to(position); }
void QFocuserProtocolWrapper::halt() { impl_->halt(); }
void QFocuserProtocolWrapper::set_reverse(bool reversed) { impl_->set_reverse(reversed); }
void QFocuserProtocolWrapper::set_speed(int speed) { impl_->set_speed(speed); }
void QFocuserProtocolWrapper::set_hold_force(bool enabled) { impl_->set_hold_force(enabled); }
void QFocuserProtocolWrapper::set_hold_current(int ihold, int irun) { impl_->set_hold_current(ihold, irun); }

}  // namespace alpacacore::vendor::qhy

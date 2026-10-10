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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/synscan_handset_probe.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <filesystem>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace alpacacore::vendor::skywatcher {

namespace {

constexpr char kFrameStart = ':';
constexpr char kFrameEnd = '\r';
constexpr char kReplyOk = '=';
constexpr char kReplyError = '!';
// UDP datagrams can be silently dropped (spec: one command per datagram, one
// response per datagram) — retransmit a bounded number of times on timeout.
constexpr int kUdpRetries = 3;

// open-astro#559: the serial transport had no retransmit at all -- a ":K"
// (stop) frame the board never decoded left the axis running at guide rate
// until the driver's OUTER stop loop noticed, 1000 ms (the default reply
// timeout) plus its own 100 ms wait later. A stop is idempotent (stopping a
// stopped axis is harmless), so it alone is resent on timeout, mirroring
// exchange_udp()'s kUdpRetries. State-latching frames (":E" ":S" ":G" ":I"
// ":J" ":F" ":W", …) are NOT retried here, since a reply lost after the board
// already applied the change would otherwise resend and could double-apply
// it. Reads are not retried either: each retry holds io_mutex_ for another
// full reply timeout, which on a flaky link would queue a stop behind a
// status poll for ~3.4 s instead of ~1 s.
constexpr int kSerialIdempotentRetries = 3;
// A stop is idempotent AND urgent: waiting the full data-command reply
// timeout before the first retransmit is exactly the overshoot this fixes.
// 250 ms is generous for a serial round-trip at the mount's usual baud rates
// while still landing a retransmit well before the outer stop loop's own
// 100 ms-later resend would otherwise be needed.
constexpr int kStopReplyTimeoutMs = 250;

// True only for the stop frames (":K" stop_motion, ":L" instant_stop): the
// ones #559 needs retransmitted, and safe to resend after a timeout because
// they leave the board in the same state however many times they land.
bool is_retransmittable_stop(char command) { return command == 'K' || command == 'L'; }

// open-astro#505: consecutive failed exchanges before the link fault latches.
// Matches the iOptron driver's kDeviceFaultThreshold rather than introducing a
// second number for the same decision. This counts EXCHANGES, not seconds: a
// polled board only looks silent when something asks it, so the wall-clock time
// to latch is the caller's poll cadence times this, which on the EQM-35 rig
// (~3-4 s between position polls) is about 10 s, not 3 x the 1 s response
// timeout. Hardware run 2026-09-17: a board powered off with the adapter still
// plugged in produced one WARN per poll indefinitely and never latched.
constexpr int kLinkFaultThreshold = 3;

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

char nibble_hex(uint32_t v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

// Decode the 6 hex chars of a ":e" reply. Byte layout is
// <fw major><fw minor><mount code>: the third byte identifies the MODEL, it is
// not a firmware patch level. Verified two ways on an EQM-35 Pro: ":e1" returns
// "=032732" (0x32 = 50) while the SynScan handset on the same mount reports
// model ID 50 from its own "m" command. Matches INDI skywatcherAPI.cpp, whose
// MountType enum lists 0x44/0x45 for the Wave 100i/150i -- the Wave's
// "=033A44" third byte 0x44 is likewise an identity, not a ".68" patch.
bool decode_mc_version(const std::string& data, int& fw_major, int& fw_minor, std::uint8_t& mount_code) {
    if (data.size() < 6) {
        return false;
    }
    auto byte_at = [&data](std::size_t i) {
        int hi = hex_nibble(data[i]);
        int lo = hex_nibble(data[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        return hi * 16 + lo;
    };
    int b0 = byte_at(0);
    int b1 = byte_at(2);
    int b2 = byte_at(4);
    if (b0 < 0 || b1 < 0 || b2 < 0) {
        return false;
    }
    fw_major = b0;
    fw_minor = b1;
    mount_code = static_cast<std::uint8_t>(b2);
    return true;
}

// Firmware string only, e.g. "3.39". Empty on a malformed reply.
std::string format_mc_version(const std::string& data) {
    int major = 0;
    int minor = 0;
    std::uint8_t code = 0;
    if (!decode_mc_version(data, major, minor, code)) {
        return "";
    }
    std::ostringstream oss;
    oss << major << "." << (minor < 10 ? "0" : "") << minor;
    return oss.str();
}

// Expected "=" reply payload length for each command word, used by the UDP
// transport to reject a mis-paired stale reply (a duplicate ACK from a
// retransmitted command otherwise pairs itself with the NEXT command --
// seen on Wave 100i Wi-Fi as ':f' answered by a bare '='). -1 = unknown.
int expected_reply_data_len(char command) {
    switch (command) {
        case 'e':
        case 'a':
        case 'b':
        case 'j':
        case 'h':
        case 'i':
        case 'D':
        case 'q':
            return 6;
        case 'f':
            return 3;
        case 'g':
            return 2;
        case 'c':
            return -1;
        case 'E':
        case 'F':
        case 'G':
        case 'S':
        case 'I':
        case 'J':
        case 'K':
        case 'L':
        case 'O':
        case 'P':
        case 'V':
        case 'W':
            return 0;
        default:
            return -1;
    }
}

std::string mc_error_message(const std::string& code) {
    // Error codes from the MC command set; unknown codes are surfaced raw.
    if (code == "0") return "Unknown command";
    if (code == "1") return "Command length error";
    if (code == "2") return "Motor not stopped";
    if (code == "3") return "Invalid character";
    if (code == "4") return "Not initialized";
    if (code == "5") return "Driver sleeping";
    if (code == "7") return "PEC training running";
    if (code == "8") return "No valid PEC data";
    return "Motor controller error code " + code;
}

// A "!<code>" reply: the board received the command and refused it. Kept
// distinct from transport failures (timeout, mis-paired or malformed reply,
// not connected) so a caller can tell "this board does not know the command"
// from "this exchange did not complete".
class MotorControllerRejected : public AlpacaException {
public:
    MotorControllerRejected(const std::string& what, std::string code)
        : AlpacaException(what), code_(std::move(code)) {}
    const std::string& code() const { return code_; }
    bool unknown_command() const { return code_ == "0"; }

private:
    std::string code_;
};

// Build the TRACE wire-log line only when TRACE is actually enabled: the
// macro checks the level inside log(), after the caller has already
// concatenated the string, so the gate has to be here.
bool trace_enabled() { return alpacacore::logging::get_log_level() <= alpacacore::logging::LogLevel::Trace; }

}  // namespace

// Mount-code byte of the ":e" reply. Values follow INDI's skywatcherAPI.cpp
// MountType enum, plus 0x32 for the EQM-35 Pro and 0x09 for the EQ-AL55i Pro,
// which appear in neither INDI's table nor Sky-Watcher's published SynScan model
// list but are what the hardware reports (0x32 on both protocols).
std::string mount_code_to_name(std::uint8_t mount_code) {
    switch (mount_code) {
        case 0x00:
            return "EQ6";
        case 0x01:
            return "HEQ5";
        case 0x02:
            return "EQ5";
        case 0x03:
            return "EQ3";
        case 0x04:
            return "EQ8";
        case 0x05:
            return "AZ-EQ6";
        case 0x06:
            return "AZ-EQ5";
        // EQ-AL55i Pro: reported by its owner on 2026-09-20 (open-astro#306),
        // ":e1" = "=032E09", firmware 3.46. Like 0x32 it is in neither INDI's
        // table nor Sky-Watcher's published SynScan model list. One unit: it is
        // not known whether 0x09 is specific to the AL55i or shared with a
        // family, so treat the name as this board's report, not a range.
        case 0x09:
            return "EQ-AL55i Pro";
        case 0x0A:
            return "Star Adventurer";
        case 0x0C:
            return "Star Adventurer GTi";
        case 0x20:
            return "EQ8-R Pro";
        case 0x22:
            return "AZ-EQ6 Pro";
        case 0x23:
            return "EQ6-R Pro";
        case 0x24:
            return "EQ6 Pro";
        case 0x25:
            return "CQ350 Pro";
        case 0x31:
            return "EQ5 Pro";
        case 0x32:
            return "EQM-35 Pro";
        case 0x44:
            return "Wave 100i";
        case 0x45:
            return "Wave 150i";
        case 0xA2:
            return "AZ-GTe";
        case 0xA5:
            return "AZ-GTi";
        default: {
            // Unknown board: surface the raw code so a new model can be
            // identified from the logs and added above.
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out = "Mount (code 0x";
            out += kHex[(mount_code >> 4) & 0xF];
            out += kHex[mount_code & 0xF];
            out += ")";
            return out;
        }
    }
}

std::string SkyWatcherProtocolWrapper::encode_u24(uint32_t value) {
    // 0x123456 -> "563412": low byte first, each byte high-nibble-first.
    std::string out(6, '0');
    out[0] = nibble_hex((value >> 4) & 0xF);
    out[1] = nibble_hex(value & 0xF);
    out[2] = nibble_hex((value >> 12) & 0xF);
    out[3] = nibble_hex((value >> 8) & 0xF);
    out[4] = nibble_hex((value >> 20) & 0xF);
    out[5] = nibble_hex((value >> 16) & 0xF);
    return out;
}

uint32_t SkyWatcherProtocolWrapper::decode_u24(const std::string& data) {
    if (data.size() < 6) {
        throw AlpacaException("Motor controller reply too short for 24-bit value: '" + data + "'");
    }
    uint32_t value = 0;
    // "563412" -> bytes 0x56, 0x34, 0x12 -> 0x123456
    static constexpr int kByteShift[3] = {0, 8, 16};
    for (int b = 0; b < 3; ++b) {
        auto idx = static_cast<std::size_t>(b) * 2;
        int hi = hex_nibble(data[idx]);
        int lo = hex_nibble(data[idx + 1]);
        if (hi < 0 || lo < 0) {
            throw AlpacaException("Motor controller reply is not hex: '" + data + "'");
        }
        value |= static_cast<uint32_t>(hi * 16 + lo) << kByteShift[b];
    }
    return value;
}

// ── Serial probe / enumeration ──────────────────────────────────────────────

#ifndef _WIN32
// Open a serial port at @p baud_rate 8N1 and probe it with ":e1\r". Returns
// the motor board version payload on success, empty on failure. Declared in
// the public header (with probe_skywatcher_port_any_baud) for the pty tests.
std::string probe_skywatcher_port(const std::string& port_path, int baud_rate) {
    // A SynScan hand controller shares the adapter classes this scan targets
    // and speaks its own protocol at 9600. It is only put at risk by a probe
    // at some OTHER rate: one motor-controller probe at 115200 is enough to
    // stop it answering serial entirely, and only a power-cycle brings it
    // back (EQM-35 Pro rig, 2026-09 - the "hand-controller commands time
    // out" report). Gate on the baud actually being used rather than paying
    // the guard's full timeout on every port for a hazard that baud does
    // not create: the 9600 attempt of probe_skywatcher_port_any_baud() below
    // never trips it, the 115200 attempt (Synta EQ boards over their own USB
    // port) always runs it first. See util/synscan_handset_probe.h.
    // The echo guard below opens the port itself, before the open()/re-check
    // pair further down, so it needs its own look at the cross-vendor
    // registry: the caller's check happened before the 9600 attempt, and
    // another vendor's connect may have claimed the port since.
    if (alpacacore::util::is_serial_port_in_use(port_path)) {
        return "";
    }
    if (baud_rate != 9600 && util::port_answers_synscan_echo(port_path)) {
        ALPACA_LOG_INFO("SkyWatcher", "Skipping " + port_path + ": a SynScan hand controller answered the echo test");
        return "";
    }
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return "";
    }
    // Re-check after opening: another vendor's driver may have claimed this
    // port (its connect marks it before opening) in the window between the
    // caller's is_serial_port_in_use() check and this open(). Bail rather
    // than reading for up to 1.5s and stealing bytes from that device's
    // stream (issue #230: EQDIR cables share PL2303/CH340/FTDI chips with
    // other vendor probes, e.g. iOptron, ZWO EAF, Gemini).
    if (alpacacore::util::is_serial_port_in_use(port_path)) {
        close(fd);
        return "";
    }

    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return "";
    }
    speed_t speed = baud_rate == 115200 ? B115200 : B9600;
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
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
    tty.c_cc[VTIME] = 5;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return "";
    }
    // CH340/CH341-style adapters assert DTR on open and reset an attached MCU
    // on close when HUPCL is set; clear it so the probe doesn't double-reset
    // the controller (~4 s penalty per probe).
    tty.c_cflag &= ~HUPCL;
    tcsetattr(fd, TCSANOW, &tty);

    // Keep the fd non-blocking and bound every read with poll(), as the
    // connected link does (see SkyWatcherProtocolWrapper::connect): a USB
    // CDC-ACM port that ignores VMIN/VTIME parks a blocking read() on a
    // silent candidate forever, hanging the whole auto-detect scan. The
    // 4-byte ":e1" fits an empty TX buffer; should write_all() still hit
    // EAGAIN, the probe just reports no board.
    if (!util::set_nonblocking(fd)) {
        close(fd);
        return "";
    }
    tcflush(fd, TCIOFLUSH);

    const char probe[] = ":e1\r";
    if (!util::write_all(fd, probe, sizeof(probe) - 1)) {
        close(fd);
        return "";
    }

    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, static_cast<int>(remaining));
        if (pr == 0) break;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            if (ch == kFrameEnd) break;
            reply.push_back(ch);
            if (reply.size() > 16) break;
        } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            break;  // hangup or read error; EAGAIN is a spurious "readable"
        }
    }
    close(fd);

    if (reply.size() < 7 || reply[0] != kReplyOk) {
        return "";
    }
    // Raw 6-hex-char payload: the caller decodes firmware AND mount code.
    return reply.substr(1, 6);
}

// Probe a port at each baud a Sky-Watcher board is known to use, returning the
// first that answers ":e1". The Wave's STM32 CDC-ACM port ignores baud, so 9600
// succeeds there on the first try; Synta EQ boards reached over the mount's
// built-in USB port or an EQDIR cable are real UART bridges -- the EQM-35 Pro's
// onboard PL2303 runs at 115200, so a 9600-only scan silently misses it.
constexpr int kProbeBauds[] = {9600, 115200};

bool probe_skywatcher_port_any_baud(const std::string& port_path, MotorBoardInfo& info_out, int& baud_out) {
    for (int baud : kProbeBauds) {
        std::string payload = probe_skywatcher_port(port_path, baud);
        if (payload.empty()) {
            continue;
        }
        int major = 0;
        int minor = 0;
        MotorBoardInfo info;
        if (!decode_mc_version(payload, major, minor, info.mount_code)) {
            // Answered on the frame level but the payload is not a version;
            // treat as a non-match rather than adopting a bogus identity.
            continue;
        }
        info.firmware_version = format_mc_version(payload);
        info.model_name = mount_code_to_name(info.mount_code);
        info_out = std::move(info);
        baud_out = baud;
        return true;
    }
    return false;
}

namespace {

std::string describe_found_port(const SkyWatcherPortInfo& port) {
    return "Found Sky-Watcher " + port.model_name + " on " + port.port_path + " (MC firmware " + port.firmware_version +
           ", " + std::to_string(port.baud_rate) + " baud)";
}

bool raw_port_looks_like_skywatcher_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    // Wave-series mounts expose an STM32 CDC-ACM virtual COM port (0483:5740,
    // /dev/ttyACM*); also accept the classic EQDIRECT cable chips.
    return alpacacore::util::usb_tty_descriptor_matches(
        *descriptor, {"STM32", "STMicroelectronics", "0483", "Prolific", "PL2303", "067b", "FTDI", "CP210", "CH340",
                      "CH341", "1a86", "Silicon_Labs", "USB_Serial", "USB-Serial"});
}
}  // namespace
#endif  // _WIN32

std::vector<SkyWatcherPortInfo> enumerate_skywatcher_ports() {
    std::vector<SkyWatcherPortInfo> results;

#ifndef _WIN32
    std::set<std::string> probed;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            bool is_candidate =
                (name.find("STM32") != std::string::npos) || (name.find("STMicroelectronics") != std::string::npos) ||
                (name.find("Prolific") != std::string::npos) || (name.find("PL2303") != std::string::npos) ||
                (name.find("067b") != std::string::npos) || (name.find("FTDI") != std::string::npos) ||
                (name.find("CP210") != std::string::npos) || (name.find("CH340") != std::string::npos) ||
                (name.find("1a86") != std::string::npos) || (name.find("Silicon_Labs") != std::string::npos) ||
                (name.find("USB_Serial") != std::string::npos) || (name.find("USB-Serial") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            // issue #230: don't steal bytes from a port another connected
            // vendor's device (iOptron, ZWO EAF, Gemini, ...) is streaming on
            // -- several share the same PL2303/CH340/FTDI chip families.
            if (alpacacore::util::is_serial_port_in_use(resolved)) continue;
            {
                std::string msg = "Probing ";
                msg += resolved;
                msg += " (";
                msg += name;
                msg += ")...";
                ALPACA_LOG_INFO("SkyWatcher", msg);
            }
            int baud = 9600;
            MotorBoardInfo board;
            if (probe_skywatcher_port_any_baud(resolved, board, baud)) {
                SkyWatcherPortInfo found{resolved,        name, board.firmware_version, baud, board.mount_code,
                                         board.model_name};
                ALPACA_LOG_INFO("SkyWatcher", describe_found_port(found));
                results.push_back(std::move(found));
            }
        }
    }

    // Raw /dev/ttyUSB* fallback: udev by-id naming collides for serial-number-
    // less adapters, silently dropping one of two identical dongles from by-id
    // (same rationale as the SynScan/Gemini scans).
    std::vector<std::string> raw_candidates;
    for (int i = 0; i < 10; ++i) {
        // Wave mounts enumerate as CDC-ACM (/dev/ttyACM*); EQDIRECT cables as
        // /dev/ttyUSB*.
        raw_candidates.push_back("/dev/ttyACM" + std::to_string(i));
        raw_candidates.push_back("/dev/ttyUSB" + std::to_string(i));
    }
    for (const std::string& port : raw_candidates) {
        if (!alpacacore::util::path_exists(port)) continue;
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_skywatcher_candidate(resolved)) continue;
        probed.insert(resolved);
        if (alpacacore::util::is_serial_port_in_use(resolved)) continue;
        ALPACA_LOG_INFO("SkyWatcher", "Probing " + resolved + "...");
        int baud = 9600;
        MotorBoardInfo board;
        if (probe_skywatcher_port_any_baud(resolved, board, baud)) {
            SkyWatcherPortInfo found{resolved, "", board.firmware_version, baud, board.mount_code, board.model_name};
            ALPACA_LOG_INFO("SkyWatcher", describe_found_port(found));
            results.push_back(std::move(found));
        }
    }
#endif

    return results;
}

// ── Wi-Fi (UDP) discovery ───────────────────────────────────────────────────

namespace {

#ifndef _WIN32
// Probe one host with ":e1\r" over UDP. Returns firmware string or empty.
std::string probe_skywatcher_udp(const std::string& host, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return "";
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return "";
    }
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = static_cast<long>(timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char probe[] = ":e1\r";
    if (sendto(fd, probe, sizeof(probe) - 1, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }
    char buf[64] = {};
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    close(fd);
    if (n < 7 || buf[0] != kReplyOk) {
        return "";
    }
    std::string reply(buf, static_cast<std::size_t>(n));
    while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
        reply.pop_back();
    }
    std::string version = format_mc_version(reply.substr(1));
    return version.empty() ? "unknown" : version;
}

// Broadcast the version probe on every broadcast-capable IPv4 interface and
// collect responders until the timeout expires.
void broadcast_discover(std::vector<SkyWatcherHostInfo>& results, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = static_cast<long>(200) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char probe[] = ":e1\r";
    std::set<std::string> targets = {"255.255.255.255"};
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_BROADCAST) || !ifa->ifa_broadaddr) continue;
            char buf[INET_ADDRSTRLEN] = {};
            auto* baddr = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
            if (inet_ntop(AF_INET, &baddr->sin_addr, buf, sizeof(buf))) {
                targets.insert(buf);
            }
        }
        freeifaddrs(ifaddr);
    }

    for (const auto& target : targets) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, target.c_str(), &addr.sin_addr) <= 0) continue;
        sendto(fd, probe, sizeof(probe) - 1, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }

    std::set<std::string> seen;
    for (const auto& r : results) {
        seen.insert(r.host);
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        char buf[64] = {};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 7 || buf[0] != kReplyOk) {
            continue;
        }
        char host_buf[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &from.sin_addr, host_buf, sizeof(host_buf))) {
            continue;
        }
        std::string host(host_buf);
        if (seen.count(host) != 0) continue;
        seen.insert(host);
        std::string reply(buf, static_cast<std::size_t>(n));
        while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
            reply.pop_back();
        }
        std::string version = format_mc_version(reply.substr(1));
        results.push_back({host, port, version.empty() ? "unknown" : version});
        ALPACA_LOG_INFO("SkyWatcher", "Discovered motor controller at " + host + " (MC firmware " +
                                          (version.empty() ? "unknown" : version) + ")");
    }
    close(fd);
}
#endif  // _WIN32

}  // namespace

std::vector<SkyWatcherHostInfo> discover_skywatcher_hosts(int timeout_ms) {
    std::vector<SkyWatcherHostInfo> results;
#ifndef _WIN32
    constexpr int kPort = 11880;
    // AP-mode address per the MC command set spec.
    std::string fw = probe_skywatcher_udp("192.168.4.1", kPort, 500);
    if (!fw.empty()) {
        results.push_back({"192.168.4.1", kPort, fw});
        ALPACA_LOG_INFO("SkyWatcher", "Found motor controller at 192.168.4.1 (MC firmware " + fw + ")");
    }
    broadcast_discover(results, kPort, timeout_ms);
#else
    (void)timeout_ms;
#endif
    return results;
}

// ── Transport implementation ────────────────────────────────────────────────

class SkyWatcherProtocolWrapper::Impl {
public:
    explicit Impl(SerialRead serial_read, SerialWrite serial_write)
        : serial_read_(std::move(serial_read)), serial_write_(std::move(serial_write)) {}

    ~Impl() { disconnect(); }

    bool connect(const ConnectionInfo& info) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (connected_) {
            disconnect_locked();
        }
        clear_loss_locked();
        if (info.type == ConnectionType::Serial && info.port_path.empty()) {
            ALPACA_LOG_ERROR("SkyWatcher", "Serial connection requested but port_path is empty");
            return false;
        }
        if (info.type == ConnectionType::Network && info.host.empty()) {
            ALPACA_LOG_ERROR("SkyWatcher", "Network connection requested but host is empty");
            return false;
        }
        info_ = info;
        step_period_readback_.store(true, std::memory_order_relaxed);
        response_timeout_ms_.store(info.response_timeout_ms > 0 ? info.response_timeout_ms : 1000,
                                   std::memory_order_relaxed);
        bool ok = info.type == ConnectionType::Serial ? connect_serial(info) : connect_udp(info);
        connected_ = ok;
        return ok;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        disconnect_locked();
        clear_loss_locked();
    }

    // The client's own connect or disconnect settles a loss (decision 0009,
    // point 5): the kept fault text and the sticky flag go; the lost-at stamp
    // stays for the relink's outage-length decision.
    void clear_loss_locked() {
        link_lost_.store(false);
        std::lock_guard<std::mutex> lock(health_mutex_);
        lost_fault_.clear();
    }

    // ":i" step-period readback diagnostic: on for each new connection, off
    // for the rest of it once the board fails to answer ":i".
    bool step_period_readback() const { return step_period_readback_.load(std::memory_order_relaxed); }
    void disable_step_period_readback() { step_period_readback_.store(false, std::memory_order_relaxed); }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(io_mutex_);
        return connected_;
    }

    // open-astro#445: whether the link is still there, without talking to the
    // board and without waiting behind an exchange in flight (a Connected poll
    // must stay fast). A serial link is gone when its configured path no
    // longer resolves to the node that was opened: the node was removed (USB
    // adapter pulled) or a by-id symlink now points at a replugged adapter.
    // UDP has no such signal and reports the flag as-is. When the loss is seen
    // and no exchange holds the link, it is torn down here, so the stale fd
    // stops pinning the old device name.
    bool link_alive() {
        if (!connected_.load() || link_lost_.load()) {
            return false;
        }
        // Decision 0009: a fault that has stood past the staleness bound is a
        // lost link, on UDP and serial alike.
        if (fault_stale()) {
            lose_stale_link();
            return false;
        }
#ifndef _WIN32
        bool present = true;
        {
            std::lock_guard<std::mutex> lock(link_id_mutex_);
            if (!link_path_.empty()) {
                struct stat st {};
                if (::stat(link_path_.c_str(), &st) != 0) {
                    // Only "no such node" is loss; a permission or I/O error on
                    // the lookup says nothing about the adapter.
                    present = !(errno == ENOENT || errno == ENOTDIR);
                } else {
                    present = st.st_dev == link_dev_ && st.st_ino == link_ino_;
                }
            }
        }
        if (!present) {
            // Best-effort teardown: if an exchange holds io_mutex_ right now,
            // this call reports the loss without closing the fd. The next
            // poll, the in-flight exchange's own node check, or a relink all
            // retry the close; nothing is lost, but until one of those runs
            // the stale fd stays open with Connected already reading false.
            keep_fault_text();
            note_link_lost();
            std::unique_lock<std::mutex> lock(io_mutex_, std::try_to_lock);
            if (lock.owns_lock() && connected_.load()) {
                ALPACA_LOG_WARN("SkyWatcher", "Serial device " + info_.port_path + " has gone away; link closed");
                disconnect_locked();
            }
            return false;
        }
#endif
        return connected_.load();
    }

    std::string exchange(const std::string& frame, int timeout_ms, int expected_data_len = -1, int attempts = 1) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to Sky-Watcher motor controller", AlpacaError::NotConnected);
        }
        if (link_lost_.load()) {
            // Lost while an exchange held the lock: finish the close here.
            disconnect_locked();
            throw AlpacaException("Not connected to Sky-Watcher motor controller", AlpacaError::NotConnected);
        }
        // open-astro#505: this is the one place both transports converge, so
        // the consecutive-failure latch lives here and covers serial and UDP
        // alike (link_alive() applies the staleness bound to the latched
        // fault on both).
        //
        // The latch detects SILENCE, so ONLY a genuine no-reply timeout counts.
        // Any frame from the board — mis-paired, malformed, stale or over-long
        // — proves it is alive and talking, which is exactly the condition this
        // must NOT fire on, so it RESETS the counter and leaves the
        // protocol-level problem to the machinery that already owns it (the
        // dirty/settle/resync path and send_command's shape check). A serial write
        // error is neither: it says nothing about whether the board is
        // answering, so it is left uncounted. A UDP send/receive error is
        // reachability evidence and counts as a no-reply exchange
        // (udp_socket_error_locked), except EBADF/ENOTSOCK/ENOTCONN, which
        // lose the link at once.
        exchange_saw_frame_ = false;
        exchange_timed_out_ = false;
        try {
            std::string reply = info_.type == ConnectionType::Serial
                                    ? exchange_serial(frame, timeout_ms, attempts)
                                    : exchange_udp(frame, timeout_ms, expected_data_len);
            note_exchange_ok();
            return reply;
        } catch (const AlpacaException& e) {
            // A lost link is #445's business, not a fault: Connected goes
            // false and operations throw NotConnected, so counting it here
            // would latch a fault on a device that is simply gone. Silence
            // with the node still present is what this latch is for.
            if (e.error_code() != AlpacaError::NotConnected && connected_) {
                if (exchange_saw_frame_) {
                    note_exchange_ok();
                } else if (exchange_timed_out_) {
                    note_exchange_failed(e.what());
#ifndef _WIN32
                    // open-astro#912: line settings belong to the tty, not the
                    // fd, so another process's open can leave it at the wrong
                    // rate while this fd keeps talking. Re-apply ours on every
                    // silent timeout so the next exchange can clear the latch.
                    if (serial_fd_ >= 0 && info_.type == ConnectionType::Serial) {
                        (void)tcsetattr(serial_fd_, TCSANOW, &serial_termios_);
                    }
#endif
                }
            }
            throw;
        }
    }

    // Both run under io_mutex_ (the exchange path); they take health_mutex_
    // only to publish, so link_faulted() never waits behind an exchange.
    void note_exchange_ok() {
        bool restored = false;
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            restored = link_health_.on_reply();
            if (restored) {
                ++recovery_epoch_;
            }
        }
        if (restored) {
            ALPACA_LOG_INFO("SkyWatcher", "Motor controller link restored");
        }
    }

    void note_exchange_failed(const std::string& reason) {
        std::optional<std::string> latched;
        int failures = 0;
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            const bool was_faulted = link_health_.faulted();
            latched = link_health_.note_failure(reason, kLinkFaultThreshold, clock().now());
            failures = link_health_.consecutive_failures();
            if (was_faulted) {
                return;  // already latched: the per-exchange WARN would repeat forever
            }
        }
        if (latched) {
            ALPACA_LOG_ERROR("SkyWatcher", "Motor controller link fault latched: " + *latched);
        } else {
            ALPACA_LOG_WARN("SkyWatcher", "Motor controller exchange failed (transient, " + std::to_string(failures) +
                                              "/" + std::to_string(kLinkFaultThreshold) + "): " + reason);
        }
    }

    // After a loss the last fault text stays until the next connect or
    // disconnect (decision 0009, point 5), so the listing shows why.
    std::string link_fault() {
        std::lock_guard<std::mutex> lock(health_mutex_);
        return link_health_.faulted() ? link_health_.fault() : lost_fault_;
    }

    void set_task_clock(util::TaskClock& clock) { clock_.store(&clock); }

    // Decision 0009 point 5: a loss keeps the latched fault text for
    // get_link_fault() until the next connect or disconnect. Call before
    // note_link_lost() on every loss path.
    void keep_fault_text() {
        std::lock_guard<std::mutex> lock(health_mutex_);
        if (link_health_.faulted()) {
            lost_fault_ = link_health_.fault();
        }
    }

    bool fault_stale() {
        std::lock_guard<std::mutex> lock(health_mutex_);
        return link_health_.fault_stale(clock().now());
    }

    // Marks the link lost for good: the sticky flag makes every later
    // link_alive() and exchange() report NotConnected even when an exchange in
    // flight holds io_mutex_ and the close has to wait for it.
    void lose_stale_link() {
        keep_fault_text();
        note_link_lost();
        link_lost_.store(true);
        std::unique_lock<std::mutex> lock(io_mutex_, std::try_to_lock);
        if (lock.owns_lock() && connected_.load()) {
            const std::string reason = link_fault();
            ALPACA_LOG_WARN("SkyWatcher", "Motor controller link lost: " + reason + "; link closed");
            disconnect_locked();
        }
    }

    util::TaskClock& clock() const { return *clock_.load(); }

    // open-astro#521: when the link was last LOST (not cleanly disconnected).
    // A relink needs the length of the outage to decide whether motion the
    // board is still running was a glitch to ride out or an unattended runaway
    // to stop. Only the loss paths stamp it; a client's own disconnect does
    // not, so a fresh connect finds no stamp and takes the safe branch.
    // Recorded once per outage: the first detection wins, because later polls
    // would otherwise keep pushing the stamp forward and make every outage
    // look brief.
    void note_link_lost() { link_lost_at_.note(clock().now()); }

    // Read-and-clear: the stamp answers exactly one question, asked once per
    // connect, and leaving it set would make the NEXT connect think it was
    // recovering from this outage.
    std::optional<std::chrono::steady_clock::time_point> consume_link_lost_at() { return link_lost_at_.consume(); }

    std::uint64_t recovery_epoch() const { return recovery_epoch_.load(std::memory_order_relaxed); }

    // Read lock-free from send paths; published in connect() under io_mutex_.
    int default_timeout() const { return response_timeout_ms_.load(std::memory_order_relaxed); }

    // After a mis-paired reply: absorb whatever else is in flight so the
    // resend starts from a quiet link.
    void settle_after_mispair() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (!connected_) {
            return;
        }
        if (info_.type == ConnectionType::Serial) {
            settle_serial(200);
        } else {
            settle_drain(300);
            link_dirty_ = true;  // force the ":e1" resync probe on the next UDP exchange
        }
    }

private:
    void disconnect_locked() {
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            // open-astro#912: the exclusive flag lives on the tty and survives
            // our close while anything else still holds it open, so drop it
            // explicitly or a reconnect would fail EBUSY.
            (void)ioctl(serial_fd_, TIOCNXCL);
            close(serial_fd_);
            serial_fd_ = -1;
        }
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        if (!registered_port_.empty()) {
            alpacacore::util::mark_serial_port_closed(registered_port_);
            registered_port_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(link_id_mutex_);
            link_path_.clear();
        }
#endif
        {
            // open-astro#505: a fault belongs to the session that latched it.
            std::lock_guard<std::mutex> lock(health_mutex_);
            link_health_.reset();
        }
        connected_ = false;
    }

    // Decision 0009 point 2: a socket errno is lost-at-once only when it says
    // the socket itself is unusable. Every other one (EHOSTUNREACH after a
    // failed neighbour lookup, ECONNREFUSED while the board's UDP stack
    // reboots, ENETUNREACH while the interface is down) reports reachability,
    // which a Wi-Fi drop or a board reboot restores: it counts as a no-reply
    // exchange for the latch and the staleness bound decides.
    [[noreturn]] void udp_socket_error_locked(const std::string& what, int err) {
        if (err == EBADF || err == ENOTSOCK || err == ENOTCONN) {
            lose_socket_link_locked(what);
        }
        exchange_timed_out_ = true;
        throw AlpacaException(what);
    }

    // Decision 0009 point 3 for a UDP socket that failed hard (send/receive
    // error, not a silent timeout): the transport is gone.
    [[noreturn]] void lose_socket_link_locked(const std::string& what) {
        if (!connected_) {
            // The connect probe: a refused connect, not a loss.
            throw AlpacaException(what);
        }
        ALPACA_LOG_WARN("SkyWatcher", what + " on " + info_.host + "; network link closed");
        keep_fault_text();
        note_link_lost();
        link_lost_.store(true);
        disconnect_locked();
        throw AlpacaException(what + "; network link to the motor controller lost", AlpacaError::NotConnected);
    }

#ifndef _WIN32
    // open-astro#445: the serial fd is unusable (the adapter left the bus).
    // Close it -- releasing the port name and its registry claim -- and report
    // NotConnected, which is what the client needs to act on, rather than a
    // generic transport error on a link that still claims to be up.
    [[noreturn]] void lose_serial_link_locked(const std::string& what) {
        ALPACA_LOG_WARN("SkyWatcher", what + " on " + info_.port_path + "; serial link closed");
        keep_fault_text();
        note_link_lost();
        disconnect_locked();
        throw AlpacaException(what + "; serial link to the motor controller lost", AlpacaError::NotConnected);
    }

    // The fd's node has been removed. Checked on the fd itself, so it holds
    // even while the configured path resolves to a replugged adapter.
    bool serial_node_removed_locked() const {
        struct stat st {};
        return ::fstat(serial_fd_, &st) != 0 || st.st_nlink == 0;
    }
#endif

    bool connect_serial(const ConnectionInfo& info) {
#ifndef _WIN32
        // Canonicalize so this matches whatever form enumerate_skywatcher_ports()
        // or a user-typed config path resolves to (issue #230: a by-id symlink
        // and its /dev/ttyUSBn target must compare equal in the registry).
        std::error_code path_ec;
        std::string canonical_path = std::filesystem::canonical(info.port_path, path_ec).string();
        std::string registry_key = path_ec ? info.port_path : canonical_path;

        // Claim BEFORE opening: a concurrent auto-detect scan (this vendor's
        // own, or another's) checks is_serial_port_in_use() then opens -- claiming
        // first closes the window where it could slip in between our check and
        // our open() and start reading this mount's replies. The check and the
        // claim are one atomic step so two racing connects cannot both pass a
        // separate check and both claim the port (PR #251 review).
        if (!alpacacore::util::try_mark_serial_port_open(registry_key)) {
            ALPACA_LOG_ERROR("SkyWatcher",
                             "Port " + registry_key + " is already held open by another connected device");
            return false;
        }

        serial_fd_ = open(info.port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            ALPACA_LOG_ERROR("SkyWatcher", "Failed to open " + info.port_path + ": " + util::errno_string(errno));
            alpacacore::util::mark_serial_port_closed(registry_key);
            return false;
        }
        // open-astro#912: claim the tty so no other non-root process can open
        // it (an outside open reprograms the shared line settings and a
        // reconnect then fails EBUSY). Released by close() in
        // disconnect_locked(). Ignored on failure: pty back ends may not
        // support it, and a mount that works without it keeps working.
        (void)ioctl(serial_fd_, TIOCEXCL);
        struct termios tty {};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            alpacacore::util::mark_serial_port_closed(registry_key);
            return false;
        }
        speed_t speed = info.baud_rate == 115200 ? B115200 : B9600;
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= CREAD | CLOCAL;
        tty.c_cflag &= ~HUPCL;  // keep DTR asserted on close (CH340 MCU-reset quirk)
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tty.c_oflag &= ~OPOST;
        tty.c_oflag &= ~ONLCR;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 1;
        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            alpacacore::util::mark_serial_port_closed(registry_key);
            return false;
        }
        serial_termios_ = tty;
        // Keep the fd NON-blocking (it was opened O_NONBLOCK) and drive every
        // read through poll() (see poll_serial_readable / exchange_serial /
        // settle_serial). Some USB CDC-ACM virtual COM ports do NOT honour
        // VMIN/VTIME as a read timeout -- a blocking read(fd,&ch,1) on a board
        // that has gone quiet then parks forever in n_tty_read, and because the
        // worker holds the I/O mutex (and the driver mutex above it) every Alpaca
        // request blocks until the service is killed. Such a port can also return
        // a spurious poll() "readable" after which a blocking read still parks, so
        // poll alone is not enough: the read must be non-blocking so it returns
        // EAGAIN instead of parking, bounded by the poll deadline. WRITES on the
        // same non-blocking fd can likewise hit EAGAIN before the first byte when
        // the TX buffer fills (util::write_all only retries EAGAIN after a partial
        // write), so frame sends go through write_all_bounded(), which waits for
        // POLLOUT within the command budget -- the write-side mirror of the reads.
        // (The other serial vendors keep clear_nonblocking + VTIME; this is a
        // Sky-Watcher-specific hardening for a tty that ignores VTIME.)
        if (!util::set_nonblocking(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            alpacacore::util::mark_serial_port_closed(registry_key);
            return false;
        }
        tcflush(serial_fd_, TCIOFLUSH);
        registered_port_ = registry_key;
        // open-astro#445: remember which node this path reached, for link_alive().
        // fstat on a just-opened fd essentially cannot fail; if it ever does,
        // link_path_ stays empty and link_alive() silently falls back to the
        // pre-#445 connected_-only read, so log it rather than degrade quietly.
        struct stat st {};
        if (::fstat(serial_fd_, &st) == 0) {
            std::lock_guard<std::mutex> lock(link_id_mutex_);
            link_path_ = info.port_path;
            link_dev_ = st.st_dev;
            link_ino_ = st.st_ino;
        } else {
            ALPACA_LOG_WARN("SkyWatcher",
                            "fstat failed on the newly opened serial port; link-loss detection "
                            "disabled for this connection: " +
                                util::errno_string(errno));
        }
        return true;
#else
        (void)info;
        return false;
#endif
    }

    bool connect_udp(const ConnectionInfo& info) {
#ifndef _WIN32
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(info.udp_port));
        if (inet_pton(AF_INET, info.host.c_str(), &addr.sin_addr) <= 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(info.host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        // connect() the datagram socket so recv() only accepts the mount's
        // replies and send() needs no per-call address.
        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        // Verify the controller answers before declaring the link up: UDP
        // "connect" succeeds even with nothing listening.
        try {
            std::string reply = exchange_udp(":e1\r", default_timeout(), 6);
            if (reply.empty() || reply[0] != kReplyOk) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            fw_reply_ = reply;  // known ":e1" answer, used by resync_udp()
        } catch (const std::exception&) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        return true;
#else
        (void)info;
        return false;
#endif
    }

    // Absorb a late reply on the serial link: read and discard everything
    // for the whole window. Used after a timeout (the reply may still be in
    // flight) and after a mis-paired reply, so the NEXT command cannot
    // consume a stale frame as its own answer. The window is not cut short
    // when the line is quiet: a reply that has not STARTED arriving when
    // the settle begins would otherwise slip through, and if it has the same
    // shape as the next command's reply (":j" after a timed-out ":j") the
    // shape check in send_command cannot tell it apart either (pty-backed
    // regression in test_skywatcher_serial.cpp). Replies later than the
    // window are only caught when their shape differs.
    // Wait up to budget_ms for the serial fd to have data. Returns >0 readable,
    // 0 timed out, <0 poll error (errno set). poll() bounds the wait on the fd
    // ITSELF, independent of the tty's VMIN/VTIME -- some USB CDC-ACM virtual COM
    // ports do NOT honour VTIME as a read timeout, so a bare read(fd,&ch,1) on a
    // board that went quiet (mid-exchange, or after a mis-paired reply) parked
    // forever in n_tty_read, holding io_mutex_ (and the driver mutex_ above it)
    // and wedging the whole server until it was killed -- observed as a worker
    // stuck in settle_serial -> read (/proc/<tid>/syscall = read, wchan =
    // n_tty_read) with every other worker blocked on the driver mutex. poll()
    // never relies on VTIME, so the caller's deadline is always enforced, on
    // every tty.
    int poll_serial_readable(int budget_ms) {
#ifndef _WIN32
        struct pollfd pfd {};
        pfd.fd = serial_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;
        return ::poll(&pfd, 1, budget_ms);
#else
        (void)budget_ms;
        return 0;
#endif
    }

    // Mirror of poll_serial_readable for the write side.
    int poll_serial_writable(int budget_ms) {
#ifndef _WIN32
        struct pollfd pfd {};
        pfd.fd = serial_fd_;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        return ::poll(&pfd, 1, budget_ms);
#else
        (void)budget_ms;
        return 0;
#endif
    }

    // Write the whole frame on the non-blocking fd, bounded by a deadline SHARED
    // with the reply read that follows (so one exchange stays within timeout_ms,
    // not up to 2x it -- it matters most for the ":K"/":L" stop path's tight cap).
    // The fd is kept O_NONBLOCK for the read-timeout fix (see connect_serial), so
    // write() can return EAGAIN -- even before the first byte -- when the TX buffer
    // fills because the board stopped draining. util::write_all only retries EAGAIN
    // after a PARTIAL write, so it would fail such a frame fast; here we wait for
    // POLLOUT until the deadline instead, the write-side mirror of the poll-bounded
    // reads. errno is left set on failure for the caller's link-loss classification.
    bool write_all_bounded(const char* data, std::size_t len, std::chrono::steady_clock::time_point deadline) {
#ifndef _WIN32
        std::size_t total = 0;
        while (total < len) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining <= 0) {
                errno = EAGAIN;
                return false;
            }
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) -- O_NONBLOCK fd, EAGAIN not block
            const ssize_t n = serial_write_ ? serial_write_(serial_fd_, data + total, len - total)
                                            : ::write(serial_fd_, data + total, len - total);
            if (n > 0) {
                total += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                const int pr = poll_serial_writable(static_cast<int>(remaining));
                if (pr < 0) {
                    if (errno == EINTR) {
                        continue;  // interrupted: retry, as the read loop does
                    }
                    return false;  // poll error: errno set by poll()
                }
                if (pr == 0) {
                    continue;  // timeout: the deadline check above ends the loop
                }
                // Writable. A spurious POLLOUT that still EAGAINs next would busy-loop
                // until the deadline, so yield briefly before retrying.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (n == 0) {
                errno = EIO;  // treat a 0 return as a hard error, like util::write_all
            }
            return false;
        }
        return true;
#else
        (void)data;
        (void)len;
        (void)deadline;
        return false;
#endif
    }

    void settle_serial(int window_ms) {
#ifndef _WIN32
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
        for (;;) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining <= 0) {
                break;
            }
            // Poll for the WHOLE remaining window so a late reply that has not
            // STARTED arriving yet is still absorbed (the original behaviour),
            // but without ever parking a bare read() past the deadline.
            const int pr = poll_serial_readable(static_cast<int>(remaining));
            if (pr <= 0) {
                break;  // quiet for the rest of the window (or poll error): drained
            }
            char ch = 0;
            // Same injectable read seam as exchange_serial, so the poll bound is
            // testable on the settle path that is the one that wedged.
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) -- O_NONBLOCK fd, returns at once
            const ssize_t r = serial_read_ ? serial_read_(serial_fd_, &ch, 1) : read(serial_fd_, &ch, 1);
            if (r == 1) {
                // open-astro#505: a late reply being absorbed is still proof
                // the board is answering, so it must not count toward silence.
                exchange_saw_frame_ = true;
            } else {
                // EOF / EAGAIN: nothing more to drain this instant. If poll()
                // reported the fd readable but the read then yields EAGAIN (a
                // spurious-readable tty), this ends the drain early rather than
                // re-polling for the rest of the window; a straggler that arrives
                // after this point is caught by the mis-paired/malformed shape
                // check on the next exchange, which is the real backstop.
                break;
            }
        }
        tcflush(serial_fd_, TCIFLUSH);
#else
        (void)window_ms;
#endif
    }

    std::string exchange_serial(const std::string& frame, int timeout_ms, int attempts = 1) {
#ifndef _WIN32
        for (int attempt = 0; attempt < attempts; ++attempt) {
            // Leftover bytes from a timed-out earlier exchange would be parsed
            // as this command's reply — drain them first. A plain flush only
            // catches bytes that have ALREADY arrived; after a timeout the
            // reply may still be in flight, so settle (wait for the line to
            // go quiet) before the write. Otherwise the stale frame lands
            // after the flush and is read as this command's reply: a bare
            // "=" ack command (":I") would then be "acknowledged" by the
            // previous command's data while its own frame was never applied.
            // This also drains the frame a PRIOR retransmit attempt in this
            // same loop just gave up on.
            if (serial_node_removed_locked()) {
                lose_serial_link_locked("Serial device removed");
            }
            if (serial_dirty_) {
                settle_serial(200);  // ends with its own tcflush
                serial_dirty_ = false;
            } else {
                tcflush(serial_fd_, TCIFLUSH);
            }
            // One deadline shared by the write and the reply read below, so a
            // single exchange stays bounded by timeout_ms rather than up to 2x it.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            if (!write_all_bounded(frame.data(), frame.size(), deadline)) {
                const int err = errno;
                if (err == EIO || err == ENXIO || err == ENODEV || err == EBADF || serial_node_removed_locked()) {
                    lose_serial_link_locked("Serial write failed: " + util::errno_string(err));
                }
                // A non-blocking write can stop part-way (deadline hit after a partial
                // frame). Discard the queued fragment so it cannot prefix the next
                // command, and mark the link dirty so the next exchange settles first.
                tcflush(serial_fd_, TCOFLUSH);
                serial_dirty_ = true;
                throw AlpacaException("Serial write failed: " + util::errno_string(err));
            }
            std::string reply;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                        .count();
                if (remaining <= 0) {
                    break;
                }
                // Serialized transport: one in-flight command per link. Wait for
                // the reply on the fd ITSELF, never on VMIN/VTIME -- a USB
                // CDC-ACM port does not honour VTIME as a read timeout, so a bare
                // read() on a board that went quiet mid-reply parked forever in
                // n_tty_read (see poll_serial_readable). poll() keeps the whole
                // exchange bounded by timeout_ms and ends the quiet-tty spin.
                const int pr = poll_serial_readable(static_cast<int>(remaining));
                if (pr == 0) {
                    break;  // no (more) reply within the timeout
                }
                if (pr < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    const int err = errno;
                    if (err == EIO || err == ENXIO || err == ENODEV || err == EBADF || serial_node_removed_locked()) {
                        lose_serial_link_locked("Serial poll failed: " + util::errno_string(err));
                    }
                    throw AlpacaException("Serial poll failed: " + util::errno_string(err));
                }
                char ch = 0;
                // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) -- O_NONBLOCK fd, returns at once
                const auto r = serial_read_ ? serial_read_(serial_fd_, &ch, 1) : ::read(serial_fd_, &ch, 1);
                if (r == 1) {
                    exchange_saw_frame_ = true;  // open-astro#505: the board is talking
                    if (ch == kFrameEnd) {
                        return reply;
                    }
                    reply.push_back(ch);
                    if (reply.size() > 32) {
                        serial_dirty_ = true;
                        throw AlpacaException("Motor controller reply overflow");
                    }
                } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
                    const int err = errno;
                    if (err == EIO || err == ENXIO || err == ENODEV || err == EBADF || serial_node_removed_locked()) {
                        lose_serial_link_locked("Serial read failed: " + util::errno_string(err));
                    }
                    throw AlpacaException("Serial read failed: " + util::errno_string(err));
                } else {
                    // poll() reported readable but the read made no progress (EOF,
                    // EAGAIN, or a read that does not consume the pending byte):
                    // pace by 1 ms so this cannot spin the core, bounded by the
                    // deadline, exactly as the pre-poll loop did. The quiet-board
                    // wedge is handled by poll() returning 0 above, not here.
                    std::this_thread::sleep_until(
                        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
                }
            }
            // A hung-up tty reads 0 bytes at once, which looks like a quiet
            // board until the deadline. Checked once here rather than on
            // every VTIME tick, so waiting on a quiet board costs no extra
            // syscalls.
            if (serial_node_removed_locked()) {
                lose_serial_link_locked("Serial device removed");
            }
            serial_dirty_ = true;
            exchange_timed_out_ = true;
            if (attempt + 1 < attempts) {
                // open-astro#559: only reached for ":K"/":L" (callers pass
                // attempts > 1 for those alone) — a stop that got no reply is
                // safe to resend. settle_serial() above (next
                // loop iteration, since serial_dirty_ is now set) soaks up a
                // reply that was merely late before the resend goes out.
                ALPACA_LOG_WARN("SkyWatcher", "Timeout (" + std::to_string(timeout_ms) +
                                                  " ms) waiting for the reply to '" +
                                                  frame.substr(0, frame.size() - 1) + "'; retransmitting (" +
                                                  std::to_string(attempt + 2) + "/" + std::to_string(attempts) + ")");
                continue;
            }
            ALPACA_LOG_WARN("SkyWatcher", "Timeout (" + std::to_string(timeout_ms) + " ms) waiting for the reply to '" +
                                              frame.substr(0, frame.size() - 1) + "'; link marked dirty");
            throw AlpacaException("Timeout waiting for motor controller reply to '" + frame + "'");
        }
        throw AlpacaException("Timeout waiting for motor controller reply to '" + frame + "'");  // unreachable
#else
        (void)frame;
        (void)timeout_ms;
        (void)attempts;
        throw AlpacaException("Serial not supported on this platform");
#endif
    }

    std::string exchange_udp(const std::string& frame, int timeout_ms, int expected_data_len) {
#ifndef _WIN32
        for (int attempt = 0; attempt < kUdpRetries; ++attempt) {
            // Drain any stale datagram (a late reply to a timed-out command)
            // before sending, so replies can't get off-by-one.
            drain_udp();
            bool was_dirty = link_dirty_;
            if (link_dirty_) {
                // A previous exchange timed out, so its reply may still be in
                // flight and would otherwise be consumed as THIS command's
                // reply — the mis-pairing that garbled positions and made the
                // mount swing erratically over Wi-Fi. Soak up late arrivals
                // for a settle window, then RESYNC: probe with ":e1" (whose
                // reply value is fixed and known from connect) and require the
                // known answer before trusting the stream — a same-length
                // stale reply to a different command cannot fake that.
                settle_drain(300);
                if (!resync_udp()) {
                    link_dirty_ = true;
                    continue;  // burn this attempt; settle and probe again
                }
                link_dirty_ = false;
            }
            if (send(socket_fd_, frame.data(), frame.size(), MSG_NOSIGNAL) < 0) {
                link_dirty_ = true;
                // A Wi-Fi drop/rejoin can change the interface's IP address,
                // permanently invalidating a connect()ed datagram socket
                // (every send then fails ENETUNREACH even after the link is
                // back). Rebuild the socket and retry instead of wedging until
                // the client power-cycles the connection.
                if ((errno == ENETUNREACH || errno == EADDRNOTAVAIL || errno == EHOSTUNREACH) && rebuild_udp_socket() &&
                    send(socket_fd_, frame.data(), frame.size(), MSG_NOSIGNAL) >= 0) {
                    ALPACA_LOG_WARN("SkyWatcher",
                                    "UDP socket went stale (interface address changed); rebuilt and resent");
                } else {
                    udp_socket_error_locked("UDP send failed: " + util::errno_string(errno), errno);
                }
            }
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (true) {
                auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0) {
                    break;  // timeout — retransmit
                }
                timeval tv{};
                tv.tv_sec = static_cast<time_t>(remaining.count() / 1000);
                tv.tv_usec = static_cast<suseconds_t>((remaining.count() % 1000) * 1000);
                setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                char buf[64] = {};
                // Serialized transport: bounded by SO_RCVTIMEO.
                ssize_t n =
                    recv(socket_fd_, buf, sizeof(buf) - 1, 0);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
                if (n > 0) {
                    exchange_saw_frame_ = true;  // open-astro#505: the board is talking
                    std::string reply(buf, static_cast<std::size_t>(n));
                    while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
                        reply.pop_back();
                    }
                    if (!reply.empty() && reply[0] == kReplyError) {
                        if (was_dirty) {
                            // Just after a timeout, a stale error reply from
                            // the timed-out command may still arrive: discard
                            // it and retransmit rather than surfacing a
                            // misleading rejection for THIS command. (The next
                            // attempt recomputes was_dirty from link_dirty_.)
                            link_dirty_ = true;
                            break;
                        }
                        return reply;
                    }
                    if (!reply.empty() && reply[0] == kReplyOk &&
                        (expected_data_len < 0 || static_cast<int>(reply.size()) - 1 == expected_data_len)) {
                        return reply;
                    }
                    // Wrong shape for THIS command (a mis-paired duplicate ACK
                    // from a retransmission) or non-protocol garbage: discard
                    // and keep waiting for the matching reply.
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    break;  // timeout — retransmit
                }
                if (n < 0) {
                    link_dirty_ = true;
                    udp_socket_error_locked("UDP receive failed: " + util::errno_string(errno), errno);
                }
            }
            link_dirty_ = true;
        }
        exchange_timed_out_ = true;
        throw AlpacaException("Timeout waiting for motor controller reply to '" + frame + "' after " +
                              std::to_string(kUdpRetries) + " attempts");
#else
        (void)frame;
        (void)timeout_ms;
        throw AlpacaException("Network not supported on this platform");
#endif
    }

#ifndef _WIN32
    void drain_udp() {
        char buf[64];
        while (true) {
            // MSG_DONTWAIT: non-blocking drain, never actually blocks.
            ssize_t n =
                recv(socket_fd_, buf, sizeof(buf), MSG_DONTWAIT);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
            if (n <= 0) {
                break;
            }
            exchange_saw_frame_ = true;  // open-astro#505: a stale datagram is still an answer
        }
    }

    // Recreate the connect()ed datagram socket against the stored peer after
    // the kernel invalidated the old one (interface address change). Returns
    // false if the peer cannot be resolved/connected right now.
    bool rebuild_udp_socket() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(info_.udp_port));
        if (inet_pton(AF_INET, info_.host.c_str(), &addr.sin_addr) <= 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(info_.host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        return true;
    }

    // Blocking drain: absorb late-arriving datagrams for up to @p window_ms.
    // Verify reply-stream identity after a timeout: ":e1" always answers with
    // the motor-board version captured at connect. Returns true when the known
    // reply is received (stream aligned); false when the probe times out or
    // answers wrongly (caller settles and retries).
    bool resync_udp() {
        if (fw_reply_.empty()) {
            return true;  // no baseline captured; fall back to drains only
        }
        const std::string probe = ":e1\r";
        if (send(socket_fd_, probe.data(), probe.size(), MSG_NOSIGNAL) < 0) {
            return false;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < deadline) {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<long>(100) * 1000;
            setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[64] = {};
            ssize_t n =
                recv(socket_fd_, buf, sizeof(buf) - 1, 0);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
            if (n <= 0) {
                continue;
            }
            std::string reply(buf, static_cast<std::size_t>(n));
            while (!reply.empty() && (reply.back() == '\r' || reply.back() == '\n')) {
                reply.pop_back();
            }
            if (reply == fw_reply_) {
                return true;  // stream aligned on the known probe answer
            }
            // Stale reply from an earlier command — keep draining until the
            // probe's answer arrives or the window closes.
        }
        return false;
    }

    void settle_drain(int window_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
        char buf[64];
        while (std::chrono::steady_clock::now() < deadline) {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<long>(50) * 1000;
            setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (recv(socket_fd_, buf, sizeof(buf), 0) > 0) {  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
                exchange_saw_frame_ = true;                   // open-astro#505: still answering
            }
        }
    }
#endif

    SerialRead serial_read_;
    SerialWrite serial_write_;
    mutable std::mutex io_mutex_;
    // Written only under io_mutex_; atomic so link_alive() can read it without.
    std::atomic<bool> connected_{false};
    ConnectionInfo info_{};
    std::atomic<bool> step_period_readback_{true};  // see step_period_readback()
#ifndef _WIN32
    int serial_fd_ = -1;
    struct termios serial_termios_ {};  // line settings applied at connect, re-applied on silence (#912)
    int socket_fd_ = -1;
    std::string registered_port_;  // canonical path marked open in the cross-vendor registry
    // open-astro#445: the configured path and the node it reached at connect.
    // open-astro#505: own leaf mutex, for the same reason link_id_mutex_ has
    // one — a driver read path asking whether the link is faulted must not
    // block behind an exchange in flight.
    mutable std::mutex health_mutex_;
    util::PolledLinkHealth link_health_;
    // Bumped each time a good reply clears a latched fault. The driver watches
    // it to run the post-recovery board check (open-astro#505 hardware run: a
    // power-cycled board answers again with init_done false and its position
    // registers reset, and nothing re-sends ":F" outside connect).
    std::atomic<std::uint64_t> recovery_epoch_{0};
    // open-astro#521: task-clock time of the first detection of the current
    // outage, empty when the link has not been lost since the last connect.
    util::LinkLostStamp link_lost_at_;
    // Decision 0009: the link was lost (transport gone, or a fault past the
    // staleness bound). Sticky until the next connect or disconnect.
    std::atomic<bool> link_lost_{false};
    std::string lost_fault_;  // under health_mutex_: fault text kept after a loss
    std::atomic<util::TaskClock*> clock_{&util::default_task_clock()};
    // open-astro#505: per-exchange evidence, written only under io_mutex_ by
    // the transports. exchange_saw_frame_ means at least one byte/datagram
    // arrived from the board during this exchange (its own reply, a stale one
    // absorbed by a settle, or garbage); exchange_timed_out_ means the
    // exchange ended in a no-reply timeout rather than a write/socket error.
    bool exchange_saw_frame_ = false;
    bool exchange_timed_out_ = false;

    // Own leaf mutex so link_alive() never waits behind io_mutex_.
    mutable std::mutex link_id_mutex_;
    std::string link_path_;  // empty: nothing to watch (UDP, or disconnected)
    dev_t link_dev_ = 0;
    ino_t link_ino_ = 0;
    // Set after a UDP timeout/error: the next exchange runs a settle drain
    // before sending so a late reply cannot be mis-paired. Guarded by io_mutex_.
    bool link_dirty_ = false;
    bool serial_dirty_ = false;  // a serial exchange timed out; settle before the next write
    std::string fw_reply_;
#endif
    // Outside the platform guard: connect()/default_timeout() touch it on
    // every platform (the Windows stubs still compile against it).
    std::atomic<int> response_timeout_ms_{1000};
};

// ── Public wrapper API ──────────────────────────────────────────────────────

SkyWatcherProtocolWrapper::SkyWatcherProtocolWrapper(SerialRead serial_read, SerialWrite serial_write)
    : pimpl_(std::make_unique<Impl>(std::move(serial_read), std::move(serial_write))) {}
SkyWatcherProtocolWrapper::~SkyWatcherProtocolWrapper() = default;

SkyWatcherProtocolWrapper& SkyWatcherProtocolWrapper::instance() {
    static SkyWatcherProtocolWrapper wrapper;
    return wrapper;
}

bool SkyWatcherProtocolWrapper::connect(const ConnectionInfo& info) { return pimpl_->connect(info); }

void SkyWatcherProtocolWrapper::disconnect() { pimpl_->disconnect(); }

bool SkyWatcherProtocolWrapper::is_connected() const { return pimpl_->is_connected(); }

void SkyWatcherProtocolWrapper::set_task_clock(util::TaskClock& clock) { pimpl_->set_task_clock(clock); }

bool SkyWatcherProtocolWrapper::link_alive() { return pimpl_->link_alive(); }

std::string SkyWatcherProtocolWrapper::link_fault() { return pimpl_->link_fault(); }

bool SkyWatcherProtocolWrapper::link_faulted() { return !pimpl_->link_fault().empty(); }

std::uint64_t SkyWatcherProtocolWrapper::link_recovery_epoch() { return pimpl_->recovery_epoch(); }

std::optional<std::chrono::steady_clock::time_point> SkyWatcherProtocolWrapper::consume_link_lost_at() {
    return pimpl_->consume_link_lost_at();
}

std::string SkyWatcherProtocolWrapper::send_command(char command, int axis, const std::string& data,
                                                    int timeout_ms_override) {
    if (axis != kAxisRa && axis != kAxisDec) {
        throw AlpacaException("Invalid motor controller axis " + std::to_string(axis));
    }
    std::string frame;
    frame.reserve(4 + data.size());
    frame.push_back(kFrameStart);
    frame.push_back(command);
    frame.push_back(static_cast<char>('0' + axis));
    frame.append(data);
    frame.push_back(kFrameEnd);

    int timeout = timeout_ms_override > 0 ? timeout_ms_override : pimpl_->default_timeout();
    // open-astro#559: a stop is idempotent and urgent -- waiting the full
    // data-command reply timeout before the first retransmit IS the overshoot
    // this fixes, so ":K"/":L" get a short timeout of their own unless the
    // caller explicitly overrode it, or the device's responseTimeoutMs was
    // raised above the default: that declares a slow link, and capping its
    // stops would time out every one.
    const bool stop = is_retransmittable_stop(command);
    if (timeout_ms_override <= 0 && stop && timeout <= ConnectionInfo{}.response_timeout_ms) {
        timeout = std::min(timeout, kStopReplyTimeoutMs);
    }
    const int retransmit_attempts = stop ? kSerialIdempotentRetries : 1;
    const int expected_len = expected_reply_data_len(command);
    std::string reply;
    for (int attempt = 0;; ++attempt) {
        reply = pimpl_->exchange(frame, timeout, expected_len, retransmit_attempts);
        // TRACE-only wire log: every motor-controller frame and its reply,
        // the only way to see what the board was actually told when a
        // driver-level symptom (e.g. a pulse that produced no motion) has no
        // other trace. Gated on the level here so the string is not built
        // for every exchange below TRACE.
        if (trace_enabled()) {
            ALPACA_LOG_TRACE("SkyWatcher", "MC " + frame.substr(0, frame.size() - 1) + " -> " + reply);
        }
        // Resend once on any reply that is not a clean, expected-shape answer:
        //   - MIS-PAIRED: an OK ("=") reply of the wrong data length for the
        //     command -- a reply to SOMETHING ELSE (a late frame from a
        //     timed-out exchange). Accepting it would report success for a
        //     command the board may never have applied (PR #245).
        //   - MALFORMED: a reply that is neither "=" nor "!", e.g. a truncated
        //     ":j1" "25278" that dropped a byte. Electrical noise on a serial
        //     link corrupts a reply now and then, and a single resend recovers
        //     it instead of failing the whole operation.
        // A "!" error is a genuine board rejection and is NEVER resent.
        // This loop also resends a corrupt reply to a SET/motion command
        // (":G"/":S"/":J"/...), not just an inquiry: the board may already have
        // applied the first frame, so a resend can double-apply. This is the same
        // trade-off the mis-pair path already accepted (PR #245) -- these commands
        // are idempotent in practice (re-issuing the same target/mode/start is a
        // no-op or a harmless repeat).
        // In practice this recovery only engages over SERIAL: exchange_udp returns
        // only a "!" reply or a "=" reply of the expected length and drops any other
        // datagram, so a malformed/mis-paired reply never reaches this branch on the
        // UDP (Wi-Fi) transport.
        // TODO: a resend of a motion command whose FIRST frame was applied can draw
        // a "!" rejection (e.g. ":J" -> "motor not stopped"), surfacing as
        // MotorControllerRejected on a move that actually started. If that proves to
        // matter on a bench, limit the malformed/mis-pair resend to inquiry commands.
        const bool is_ok = !reply.empty() && reply[0] == kReplyOk;
        const bool is_error = !reply.empty() && reply[0] == kReplyError;
        const bool mispaired = is_ok && expected_len >= 0 && static_cast<int>(reply.size()) - 1 != expected_len;
        const bool malformed = !reply.empty() && !is_ok && !is_error;
        if (!mispaired && !malformed) {
            break;
        }
        ALPACA_LOG_WARN("SkyWatcher",
                        std::string(malformed ? "Malformed" : "Mis-paired") + " reply to '" +
                            frame.substr(0, frame.size() - 1) + "': got '" + reply + "'" +
                            (mispaired ? " (expected " + std::to_string(expected_len) + " data chars)" : "") +
                            "; settling the link and " + (attempt == 0 ? "resending" : "giving up"));
        // Settle before the resend AND before giving up: a corrupt/mis-paired
        // reply means a stale or partial frame is (or was just) in flight, and a
        // caller that catches the exception and carries on would otherwise have
        // its next exchange answered by the straggler -- a same-shaped one passes
        // the shape check (PR #245 review).
        pimpl_->settle_after_mispair();
        if (attempt > 0) {
            throw AlpacaException(std::string(malformed ? "Malformed" : "Mis-paired") + " motor controller reply to '" +
                                  std::string(1, command) + std::to_string(axis) + "': '" + reply + "'");
        }
    }
    if (!reply.empty() && reply[0] == kReplyOk) {
        return reply.substr(1);
    }
    if (!reply.empty() && reply[0] == kReplyError) {
        throw MotorControllerRejected("Motor controller rejected '" + std::string(1, command) + std::to_string(axis) +
                                          "': " + mc_error_message(reply.substr(1)),
                                      reply.substr(1));
    }
    throw AlpacaException("Malformed motor controller reply to '" + std::string(1, command) + std::to_string(axis) +
                          "': '" + reply + "'");
}

std::string SkyWatcherProtocolWrapper::send_raw_command(const std::string& frame, int timeout_ms_override) {
    int timeout = timeout_ms_override > 0 ? timeout_ms_override : pimpl_->default_timeout();
    std::string reply = pimpl_->exchange(frame, timeout);
    if (trace_enabled()) {
        ALPACA_LOG_TRACE("SkyWatcher",
                         "MC raw " + frame.substr(0, frame.empty() ? 0 : frame.size() - 1) + " -> " + reply);
    }
    return reply;
}

std::string SkyWatcherProtocolWrapper::get_motor_board_version() { return get_motor_board_info().firmware_version; }

MotorBoardInfo SkyWatcherProtocolWrapper::get_motor_board_info() {
    std::string data = send_command('e', kAxisRa);
    int major = 0;
    int minor = 0;
    MotorBoardInfo info;
    if (!decode_mc_version(data, major, minor, info.mount_code)) {
        throw AlpacaException("Unparseable motor board version reply: '" + data + "'");
    }
    info.firmware_version = format_mc_version(data);
    info.model_name = mount_code_to_name(info.mount_code);
    return info;
}

AxisParameters SkyWatcherProtocolWrapper::get_axis_parameters(int axis) {
    AxisParameters params;
    params.counts_per_revolution = decode_u24(send_command('a', axis));
    params.timer_frequency = decode_u24(send_command('b', axis));
    params.high_speed_ratio = decode_u24(send_command('g', axis) + "0000") & 0xFF;
    if (params.counts_per_revolution == 0 || params.timer_frequency == 0) {
        throw AlpacaException("Motor controller reported zero CPR or timer frequency on axis " + std::to_string(axis));
    }
    if (params.high_speed_ratio == 0) {
        params.high_speed_ratio = 1;
    }
    return params;
}

uint32_t SkyWatcherProtocolWrapper::inquire_position(int axis) { return decode_u24(send_command('j', axis)); }

AxisStatus SkyWatcherProtocolWrapper::inquire_status(int axis) {
    std::string data = send_command('f', axis);
    if (data.size() < 3) {
        throw AlpacaException("Short status reply: '" + data + "'");
    }
    int n0 = hex_nibble(data[0]);
    int n1 = hex_nibble(data[1]);
    int n2 = hex_nibble(data[2]);
    if (n0 < 0 || n1 < 0 || n2 < 0) {
        throw AlpacaException("Non-hex status reply: '" + data + "'");
    }
    AxisStatus status;
    status.speed_mode = (n0 & 0x1) != 0;
    status.ccw = (n0 & 0x2) != 0;
    status.fast = (n0 & 0x4) != 0;
    status.running = (n1 & 0x1) != 0;
    status.blocked = (n1 & 0x2) != 0;
    status.init_done = (n2 & 0x1) != 0;
    status.level_switch_on = (n2 & 0x2) != 0;
    return status;
}

void SkyWatcherProtocolWrapper::set_position(int axis, uint32_t counts) { send_command('E', axis, encode_u24(counts)); }

void SkyWatcherProtocolWrapper::initialization_done(int axis) { send_command('F', axis); }

void SkyWatcherProtocolWrapper::set_motion_mode(int axis, char mode, char direction) {
    send_command('G', axis, std::string(1, mode) + std::string(1, direction));
}

void SkyWatcherProtocolWrapper::set_goto_target(int axis, uint32_t counts) {
    send_command('S', axis, encode_u24(counts));
}

void SkyWatcherProtocolWrapper::set_step_period(int axis, uint32_t t1_preset, bool with_readback) {
    // A plain ":I" write, the way INDI's skywatcherAPI.cpp and indi-eqmod do
    // it: a transport failure (no, garbled or rejected reply) throws from
    // send_command; what the board STORED is never a failure condition.
    send_command('I', axis, encode_u24(t1_preset));

    // The ":i" readback is a diagnostic only -- compare the stored preset and
    // WARN on a mismatch, never resend, never throw. Two reasons. On an
    // EQM-35 Pro the readback matched exactly while the axis kept its old
    // speed (the board stores a live preset without applying it; the driver
    // checks the change in MOTION instead, see verify_live_rate_or_rekick),
    // so a matching readback proves nothing. And the rounding tolerated
    // below was measured on ONE board (MC fw 3.39); failing the write on a
    // Synta board that rounds differently would turn a harmless imprecision
    // into a hard MoveAxis/PulseGuide error (PR #1 review).
    // Callers on a timing-critical path (the pulse-guide dispatch and its
    // end-of-pulse restore: the axis is already moving at the new rate while
    // this runs, so a second round-trip would stretch the pulse) opt out.
    if (!with_readback || !pimpl_->step_period_readback()) {
        return;
    }
    // An EQM-35 Pro (MC fw 3.39) rounds the preset to a multiple of 4,
    // clamps below 168 to 168, and reads the all-ones "stopped" preset back
    // as 0: only compare presets inside the range it stores near-verbatim.
    constexpr uint32_t kReadbackFloor = 168;
    constexpr uint32_t kReadbackCeiling = 0xFFFFF0;
    constexpr uint32_t kReadbackTolerance = 4;
    if (t1_preset <= kReadbackFloor || t1_preset >= kReadbackCeiling) {
        return;
    }
    uint32_t readback = 0;
    try {
        readback = decode_u24(send_command('i', axis));
    } catch (const MotorControllerRejected& e) {
        if (!e.unknown_command()) {
            // Refused for some other reason (busy, not initialised): the
            // board does know ":i"; skip this readback only.
            ALPACA_LOG_WARN("SkyWatcher", std::string("Step-period readback (':i') refused this time: ") + e.what());
            return;
        }
        // "!0" Unknown command: this board has no ":i" -- off until the
        // next connect.
        pimpl_->disable_step_period_readback();
        ALPACA_LOG_WARN("SkyWatcher", std::string("Step-period readback (':i') unavailable on this board; diagnostic "
                                                  "disabled until reconnect: ") +
                                          e.what());
        return;
    } catch (const std::exception& e) {
        // Transport failure on the diagnostic itself (timeout, mis-paired or
        // malformed reply): the ":I" write already succeeded, and a busy link
        // is exactly when this readback matters, so keep it enabled and
        // just skip this one.
        ALPACA_LOG_WARN("SkyWatcher", std::string("Step-period readback (':i') skipped, exchange failed: ") + e.what());
        return;
    }
    const uint32_t diff = readback > t1_preset ? readback - t1_preset : t1_preset - readback;
    if (diff > kReadbackTolerance) {
        ALPACA_LOG_WARN("SkyWatcher", "Axis " + std::to_string(axis) + " step period readback " +
                                          std::to_string(readback) + " != written " + std::to_string(t1_preset) +
                                          " (diagnostic only; not resent)");
    }
}

void SkyWatcherProtocolWrapper::disable_step_period_readback() { pimpl_->disable_step_period_readback(); }

void SkyWatcherProtocolWrapper::start_motion(int axis) { send_command('J', axis); }

void SkyWatcherProtocolWrapper::stop_motion(int axis) { send_command('K', axis); }

void SkyWatcherProtocolWrapper::instant_stop(int axis) { send_command('L', axis); }

void SkyWatcherProtocolWrapper::set_autoguide_speed(int axis, int speed_code) {
    if (speed_code < 0 || speed_code > 4) {
        throw AlpacaException("Autoguide speed code must be 0-4", AlpacaError::InvalidValue);
    }
    send_command('P', axis, std::string(1, static_cast<char>('0' + speed_code)));
}

uint32_t SkyWatcherProtocolWrapper::get_feature(int axis, uint32_t inquiry) {
    return decode_u24(send_command('q', axis, encode_u24(inquiry)));
}

void SkyWatcherProtocolWrapper::set_feature(int axis, uint32_t command) {
    send_command('W', axis, encode_u24(command));
}

}  // namespace alpacacore::vendor::skywatcher

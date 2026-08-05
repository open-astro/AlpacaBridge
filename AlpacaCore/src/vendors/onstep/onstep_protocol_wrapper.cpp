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
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <filesystem>
#endif

namespace alpacacore::vendor::onstep {

namespace {

// Short timeout for :S-prefixed "set" command acks, which reply with a bare
// "1" and no '#' terminator on real OnStep firmware — the response arrives
// in well under 100ms on a working link, so this only bounds the failure
// case (mount silent/rejecting) rather than the normal-success path.
constexpr int kSetAckTimeoutMs = 1000;

// Parse a generic sexagesimal LX200 field ("HH:MM:SS", "HH:MM.T", "sDD*MM:SS",
// "sDD*MM", "sHH.H", ...) into a decimal value. Any run of non-digit,
// non-decimal-point characters is treated as a field separator, so this
// single parser covers RA, Dec, latitude, longitude, sidereal time and UTC
// offset responses without per-field grammars.
std::optional<double> parse_sexagesimal(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && (s.back() == '#' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    if (s.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    std::size_t start = 0;
    if (s[0] == '+') {
        start = 1;
    } else if (s[0] == '-') {
        negative = true;
        start = 1;
    }

    std::vector<double> parts;
    std::string current;
    auto flush_current = [&]() -> bool {
        if (current.empty()) {
            return true;
        }
        try {
            parts.push_back(std::stod(current));
        } catch (const std::exception&) {
            return false;
        }
        current.clear();
        return true;
    };

    for (std::size_t i = start; i <= s.size(); ++i) {
        char c = (i < s.size()) ? s[i] : '\0';
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            current.push_back(c);
        } else {
            if (!flush_current()) {
                return std::nullopt;
            }
        }
    }

    if (parts.empty()) {
        return std::nullopt;
    }

    double value = parts[0];
    if (parts.size() > 1) {
        value += parts[1] / 60.0;
    }
    if (parts.size() > 2) {
        value += parts[2] / 3600.0;
    }
    if (negative) {
        value = -value;
    }
    return value;
}

std::string format_target_ra(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }
    int h = static_cast<int>(wrapped);
    double rem_min = (wrapped - h) * 60.0;
    int m = static_cast<int>(rem_min);
    int s = static_cast<int>(std::lround((rem_min - m) * 60.0));
    if (s >= 60) {
        s = 0;
        ++m;
    }
    if (m >= 60) {
        m = 0;
        ++h;
        if (h >= 24) {
            h = 0;
        }
    }
    // h/m/s are runtime-bounded to [0,24)/[0,60)/[0,60) above, but GCC's
    // format-truncation checker can't prove that from an `int` parameter —
    // size the buffer for the worst case %d could ever produce.
    char buf[48];
    std::snprintf(buf, sizeof(buf), ":Sr%02d:%02d:%02d#", h, m, s);
    return buf;
}

std::string format_target_dec(double degrees) {
    char sign = degrees < 0.0 ? '-' : '+';
    double abs_deg = std::abs(degrees);
    int d = static_cast<int>(abs_deg);
    double rem_min = (abs_deg - d) * 60.0;
    int m = static_cast<int>(rem_min);
    int s = static_cast<int>(std::lround((rem_min - m) * 60.0));
    if (s >= 60) {
        s = 0;
        ++m;
    }
    if (m >= 60) {
        m = 0;
        ++d;
    }
    char buf[48];  // see format_target_ra() for why this isn't sized to the runtime bound
    std::snprintf(buf, sizeof(buf), ":Sd%c%02d*%02d:%02d#", sign, d, m, s);
    return buf;
}

std::string format_latitude(double latitude_degrees) {
    char sign = latitude_degrees < 0.0 ? '-' : '+';
    double abs_lat = std::abs(latitude_degrees);
    int d = static_cast<int>(abs_lat);
    int m = static_cast<int>(std::lround((abs_lat - d) * 60.0));
    if (m >= 60) {
        m = 0;
        ++d;
    }
    char buf[48];  // see format_target_ra() for why this isn't sized to the runtime bound
    std::snprintf(buf, sizeof(buf), ":St%c%02d*%02d#", sign, d, m);
    return buf;
}

// :Sg#'s parser accepts a signed value literally (no conversion), but the
// firmware's OWN sidereal-time formula (which drives :GS# and therefore
// every Alt/Az computation) treats the stored value as WEST-positive —
// confirmed against real hardware: sending ASCOM's East-positive convention
// directly made the mount's :GS# disagree with the correct GMST+longitude
// value by 2x the true longitude (it was subtracting where it should add).
// Sending the NEGATED value here made :GS# match the astronomically correct
// value to within a second. get_site_info() negates back on read so this
// wrapper's public API stays in ASCOM's East-positive convention throughout.
std::string format_longitude(double longitude_degrees) {
    const double wire_longitude_degrees = -longitude_degrees;
    char sign = wire_longitude_degrees < 0.0 ? '-' : '+';
    double abs_lon = std::abs(wire_longitude_degrees);
    int d = static_cast<int>(abs_lon);
    int m = static_cast<int>(std::lround((abs_lon - d) * 60.0));
    if (m >= 60) {
        m = 0;
        ++d;
    }
    char buf[48];  // see format_target_ra() for why this isn't sized to the runtime bound
    std::snprintf(buf, sizeof(buf), ":Sg%c%03d*%02d#", sign, d, m);
    return buf;
}

MountStatus parse_status(const std::string& raw) {
    MountStatus status;
    status.raw_status = raw;
    // See AGENTS.md "OnStep" notes for the full :GU# status-character table,
    // verified against real OnStep firmware (Command.ino) and live hardware
    // during driver development: 'n' is only appended when tracking is NOT
    // sidereal, and 'N' is only appended when there is NO goto in progress —
    // both flags are the ABSENCE of the condition they're named for, the
    // opposite polarity of what the INDI reference driver's comments implied.
    // Unrecognized characters are ignored (forward-compatible with firmware
    // revisions); only total non-response is treated as an error upstream.
    status.is_tracking = raw.find('n') == std::string::npos;
    status.is_slewing = raw.find('N') == std::string::npos;
    status.is_at_home = raw.find('H') != std::string::npos;
    status.is_parked = raw.find('P') != std::string::npos;
    if (raw.find('E') != std::string::npos) {
        status.side_of_pier = 0;  // pier east
    } else if (raw.find('W') != std::string::npos) {
        status.side_of_pier = 1;  // pier west
    } else {
        status.side_of_pier =
            -1;  // 'o' (pier side none/unknown) or not reported — caller falls back to hour-angle calc
    }
    return status;
}

// Probe a serial port for an OnStep controller by sending :GVP# and checking
// for "OnStep" or "On-Step" in the response.
//
// OnStep firmware runs almost exclusively on Arduino/Teensy/ESP32/STM32
// boards behind CH340/CH341/CP2102/native-USB-CDC adapters. These reset the
// MCU when DTR asserts on port open (same issue as the Gemini focuser
// wrapper) — clear HUPCL before AND after the probe so DTR stays high and
// the driver's subsequent real connect() doesn't trigger a second reset.
std::string probe_onstep_port(const std::string& port_path) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return "";
    }

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return "";
    }

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~HUPCL;  // Keep DTR high on close — prevents MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 20;  // 2-second per-read timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return "";
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return "";
    }
    tcflush(fd, TCIOFLUSH);

    // Wait for the MCU to finish booting after the DTR-triggered reset, then
    // try the identity handshake. Two attempts: first waits for boot, second
    // is a quick retry in case the first command landed mid-boot.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 1));
        tcflush(fd, TCIOFLUSH);

        const char cmd[] = ":GVP#";
        if (!util::write_all(fd, cmd, sizeof(cmd) - 1)) {
            continue;
        }

        char resp[32] = {};
        int total = 0;
        auto start = std::chrono::steady_clock::now();
        while (total < static_cast<int>(sizeof(resp) - 1)) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > 3) {
                break;
            }
            char ch = 0;
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                if (ch == '#') {
                    break;
                }
                resp[total++] = ch;
            } else if (r == 0) {
                continue;
            } else {
                break;
            }
        }

        std::string response(resp, static_cast<std::size_t>(total));
        if (response.find("OnStep") != std::string::npos || response.find("On-Step") != std::string::npos) {
            // Re-apply HUPCL-clear before closing so DTR stays asserted for
            // the driver's subsequent real connect().
            tcgetattr(fd, &tty);
            tty.c_cflag &= ~HUPCL;
            tcsetattr(fd, TCSANOW, &tty);
            close(fd);
            return response;
        }

        ALPACA_LOG_DEBUG("OnStep",
                         "Probe attempt " + std::to_string(attempt + 1) + " got no OnStep identity from " + port_path);
    }

    close(fd);
#else
    (void)port_path;
#endif
    return "";
}

std::string probe_onstep_version(const std::string& port_path) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return "";
    }
    struct termios tty{};
    if (tcgetattr(fd, &tty) == 0) {
        tty.c_cc[VTIME] = 10;
        tcsetattr(fd, TCSANOW, &tty);
    }
    util::clear_nonblocking(fd);
    tcflush(fd, TCIOFLUSH);
    const char cmd[] = ":GVN#";
    if (!util::write_all(fd, cmd, sizeof(cmd) - 1)) {
        close(fd);
        return "";
    }
    char resp[32] = {};
    int total = 0;
    auto start = std::chrono::steady_clock::now();
    while (total < static_cast<int>(sizeof(resp) - 1)) {
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > 2) {
            break;
        }
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            if (ch == '#') {
                break;
            }
            resp[total++] = ch;
        } else if (r == 0) {
            continue;
        } else {
            break;
        }
    }
    close(fd);
    return std::string(resp, static_cast<std::size_t>(total));
#else
    (void)port_path;
    return "";
#endif
}

}  // namespace

std::vector<OnStepPortInfo> enumerate_onstep_ports() {
    std::vector<OnStepPortInfo> results;

#ifndef _WIN32
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (!std::filesystem::exists(serial_by_id)) {
        // Fallback: probe /dev/ttyUSB0-9 and /dev/ttyACM0-9 directly. Arduino
        // Mega/Due/Teensy/ESP32 boards running OnStep commonly enumerate as
        // ttyACM (native USB-CDC), not just ttyUSB (USB-serial bridge chip).
        for (const char* prefix : {"/dev/ttyUSB", "/dev/ttyACM"}) {
            for (int i = 0; i < 10; ++i) {
                std::string port = prefix + std::to_string(i);
                if (std::filesystem::exists(port)) {
                    ALPACA_LOG_INFO("OnStep", "Probing " + port + "...");
                    std::string identity = probe_onstep_port(port);
                    if (!identity.empty()) {
                        std::string version = probe_onstep_version(port);
                        std::string message = "Found OnStep mount on ";
                        message += port;
                        message += " (";
                        message += identity;
                        message += ")";
                        ALPACA_LOG_INFO("OnStep", message);
                        results.push_back({port, "", version});
                    }
                }
            }
        }
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(serial_by_id)) {
        if (!entry.is_symlink()) continue;
        std::string name = entry.path().filename().string();

        bool is_candidate =
            (name.find("Prolific") != std::string::npos) || (name.find("PL2303") != std::string::npos) ||
            (name.find("067b") != std::string::npos) || (name.find("FTDI") != std::string::npos) ||
            (name.find("0403") != std::string::npos) || (name.find("CP210") != std::string::npos) ||
            (name.find("10c6") != std::string::npos) || (name.find("Silicon_Labs") != std::string::npos) ||
            (name.find("CH340") != std::string::npos) || (name.find("CH341") != std::string::npos) ||
            (name.find("1a86") != std::string::npos) || (name.find("USB_Serial") != std::string::npos) ||
            (name.find("USB-Serial") != std::string::npos) || (name.find("Arduino") != std::string::npos) ||
            (name.find("Teensy") != std::string::npos);
        if (!is_candidate) continue;

        std::string resolved = std::filesystem::canonical(entry.path()).string();
        std::string probe_message = "Probing ";
        probe_message += resolved;
        probe_message += " (";
        probe_message += name;
        probe_message += ")...";
        ALPACA_LOG_INFO("OnStep", probe_message);

        std::string identity = probe_onstep_port(resolved);
        if (!identity.empty()) {
            std::string version = probe_onstep_version(resolved);
            std::string found_message = "Found OnStep mount on ";
            found_message += resolved;
            found_message += " (";
            found_message += identity;
            found_message += ")";
            ALPACA_LOG_INFO("OnStep", found_message);
            results.push_back({resolved, name, version});
        }
    }
#endif

    return results;
}

class OnStepProtocolWrapper::Impl {
public:
    Impl() : connected_(false), connection_type_(ConnectionType::Serial) {
        serial_fd_ = -1;
        socket_fd_ = -1;
    }

    ~Impl() { disconnect(); }

    bool connect(const ConnectionInfo& info) {
        if (info.type == ConnectionType::Serial && info.port_path.empty()) {
            ALPACA_LOG_ERROR("OnStep", "Serial connection requested but port_path is empty");
            return false;
        }
        if (info.type == ConnectionType::Network && info.host.empty()) {
            ALPACA_LOG_ERROR("OnStep", "Network connection requested but host is empty");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_) {
            // The protocol wrapper is a per-vendor singleton: a second device
            // connecting through it would silently steal/tear down the first
            // device's connection. Refuse instead.
            throw AlpacaException(
                "Only one OnStep mount per bridge: the shared OnStep "
                "protocol wrapper is already connected");
        }

        connection_type_ = info.type;
        connection_info_ = info;

        bool success = false;
        if (info.type == ConnectionType::Serial) {
            success = connect_serial(info.port_path, info.baud_rate);
        } else {
            success = connect_network(info.host, info.tcp_port);
        }

        connected_ = success;
        return success;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    void disconnect_locked() {
        if (!connected_) {
            return;
        }
        if (connection_type_ == ConnectionType::Serial) {
            disconnect_serial();
        } else {
            disconnect_network();
        }
        connected_ = false;
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    std::string send_command(const std::string& command, bool require_hash_terminator, int timeout_ms_override) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }

        std::string full_command = normalize_command(command);

        // Discard any stale bytes before sending — anything already in the
        // receive buffer predates our write and would garble this command's
        // response framing if left in place.
        if (connection_type_ == ConnectionType::Serial) {
            if (serial_fd_ >= 0) {
                tcflush(serial_fd_, TCIFLUSH);
            }
        }

        if (!write_data(full_command)) {
            throw AlpacaException("Failed to send command to mount");
        }

        int timeout_ms = timeout_ms_override;
        if (timeout_ms <= 0) {
            timeout_ms = connection_info_.response_timeout_ms;
        }
        if (timeout_ms <= 0) {
            timeout_ms = 5000;
        }
        std::string response = read_response(require_hash_terminator, timeout_ms);
        ALPACA_LOG_TRACE("OnStep", "CMD " + full_command + " RESP " + response);
        return response;
    }

    void send_command_blind(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            throw AlpacaException("Not connected to mount");
        }

        std::string full_command = normalize_command(command);
        ALPACA_LOG_TRACE("OnStep", "CMD " + full_command + " (blind)");

        if (!write_data(full_command)) {
            throw AlpacaException("Failed to send command to mount");
        }

        // Many OnStep "blind" commands still ack with a stray byte on some
        // firmware. Drain whatever arrives within a short window rather than
        // leaving it to corrupt the next command's response framing — the
        // same lesson learned from iOptron's blind-command handling. Each
        // read() call blocks for a full VTIME period (100ms) when nothing
        // arrives regardless of this budget, so a lower value here still
        // costs one full VTIME cycle — kept short so that AbortSlew's five
        // sequential blind commands (:Q# + four :Qx#) stay under ASCOM's
        // 1.0s STANDARD response target instead of stacking multiple cycles
        // each (confirmed against real hardware: 120ms here cost ~1.04s
        // total; 50ms costs one read cycle per command, ~0.5s total).
        drain_stale(50);
    }

    void drain_stale(int wait_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        bool got_any = false;
        while (true) {
            char ch;
            bool got = (connection_type_ == ConnectionType::Serial) ? read_serial_char(ch) : read_network_char(ch);
            if (got) {
                got_any = true;
                continue;
            }
            if (got_any || std::chrono::steady_clock::now() >= deadline) {
                return;
            }
        }
    }

    void flush_input() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            return;
        }
        if (connection_type_ == ConnectionType::Serial) {
            if (serial_fd_ >= 0) {
                tcflush(serial_fd_, TCIFLUSH);
            }
        } else {
            drain_stale(0);
        }
    }

    // ── High-level protocol operations ──

    std::string get_product_name() { return send_command(":GVP#", true, 0); }

    std::string get_version_number() { return send_command(":GVN#", true, 0); }

    Position get_position() {
        Position pos;
        // :GRa#/:GDe# request OnStep's high-precision reply variants
        // (HH:MM:SS.SSSS# / sDD*MM:SS.SSSS#, 0.0001s / 0.001" resolution)
        // instead of the whole-second/whole-arcsecond :GR#/:GD# forms.
        // :GR#'s 1-second RA resolution (~15" at the equator) is too coarse
        // for ConformU's PulseGuide tolerance (0.07") on short (2-5s) guide
        // pulses — confirmed against real hardware (firmware "On-Step"
        // v10.23a responds to :GRa# with fractional seconds).
        std::string ra_resp = send_command(":GRa#", true, 0);
        std::string dec_resp = send_command(":GDe#", true, 0);
        auto ra = parse_sexagesimal(ra_resp);
        auto dec = parse_sexagesimal(dec_resp);
        if (!ra.has_value() || !dec.has_value()) {
            throw AlpacaException("Invalid OnStep position response");
        }
        pos.ra_hours = ra.value();
        pos.dec_degrees = dec.value();
        return pos;
    }

    AltAz get_alt_az() {
        AltAz altaz;
        auto alt = parse_sexagesimal(send_command(":GA#", true, 0));
        auto az = parse_sexagesimal(send_command(":GZ#", true, 0));
        if (!alt.has_value() || !az.has_value()) {
            throw AlpacaException("Invalid OnStep Alt/Az response");
        }
        altaz.altitude_degrees = alt.value();
        altaz.azimuth_degrees = az.value();
        return altaz;
    }

    MountStatus get_status() {
        std::string raw = send_command(":GU#", true, 0);
        return parse_status(raw);
    }

    void set_target_ra(double ra_hours) {
        // :Sr#/:Sd# are the same "bare '1', no '#'" ack pattern as
        // :St#/:Sg# (see set_latitude()) — confirmed against real hardware:
        // every slew (sync and async) sets target RA/Dec first, so this bug
        // surfaced as SlewToCoordinatesAsync timing out.
        std::string response = send_command(format_target_ra(ra_hours), false, kSetAckTimeoutMs);
        if (response != "1") {
            throw AlpacaException("Mount rejected target right ascension");
        }
    }

    void set_target_dec(double dec_degrees) {
        std::string response = send_command(format_target_dec(dec_degrees), false, kSetAckTimeoutMs);
        if (response != "1") {
            throw AlpacaException("Mount rejected target declination");
        }
    }

    bool slew_to_target() {
        // Same bare-digit-no-'#' ack as the :S-prefixed setters (see
        // set_latitude()) — confirmed against real firmware (Command.ino:
        // the :MS# handler explicitly sets supress_frame=true) and hardware
        // (the mount visibly started slewing while this call sat waiting
        // for a '#' that was never coming).
        std::string response = send_command(":MS#", false, kSetAckTimeoutMs);
        // LX200 convention: "0" means the slew was accepted; a non-"0"
        // response (often "1<reason>") means the mount rejected the target.
        return !response.empty() && response[0] == '0';
    }

    void sync_to_target() {
        // :CM# returns the name/description of the synced object — content
        // isn't meaningful here, only that the mount responded.
        (void)send_command(":CM#", true, 0);
    }

    void abort_slew() { send_command_blind(":Q#"); }

    // :To#/:Tn# (originally guessed from the INDI driver's tracking-
    // compensation switches) do NOT toggle sidereal tracking — verified
    // against real OnStep firmware (Command.ino) and live hardware: :To#
    // only enables RA rate compensation ('t'/'s' appear in :GU#'s status but
    // 'n' — "not tracking sidereal" — never clears). The real control is
    // :ST[freq]# (set tracking rate in Hz, legacy AC-mains-relative units,
    // ignored while slewing): freq < 0.1 stops tracking (TrackingNone);
    // otherwise trackingState becomes TrackingSidereal and the rate is
    // freq/60.0/1.00273790935. Solving for an exact 1.0x (sidereal) result
    // gives freq = 60.0 * 1.00273790935.
    void start_tracking() { send_command_blind(":ST60.164275#"); }

    void stop_tracking() { send_command_blind(":ST0#"); }

    void park() { send_command_blind(":hP#"); }

    void set_park() { send_command_blind(":hQ#"); }

    void unpark() { send_command_blind(":hR#"); }

    void find_home() { send_command_blind(":hC#"); }

    void select_max_slew_rate() { send_command_blind(":RS#"); }

    void move_axis_start(int direction, double rate_deg_per_sec) {
        static constexpr const char* kCommands[] = {":Mn#", ":Ms#", ":Me#", ":Mw#"};
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid OnStep move-axis direction");
        }
        // :RA[n.n]#/:RE[n.n]# set a true arbitrary custom rate (real
        // degrees/second) for the RA/Dec axis respectively — confirmed
        // against Command.ino: setting one flags currentGuideRate=-1, and
        // :Mn#/:Ms#/:Me#/:Mw# then resolve through enableGuideRate(-1),
        // which uses this custom rate rather than a discrete preset. This is
        // OnStep's actual ASCOM-MoveAxis-oriented rate command; the discrete
        // :RG#/:RC#/:RM#/:RS#/:Rn# presets are for the hand-paddle UI, not
        // arbitrary client-requested rates.
        const bool ra_axis = (direction == 2 || direction == 3);  // East/West
        char rate_buf[32];
        std::snprintf(rate_buf, sizeof(rate_buf), ra_axis ? ":RA%.4f#" : ":RE%.4f#", std::abs(rate_deg_per_sec));
        send_command_blind(rate_buf);
        send_command_blind(kCommands[direction]);
    }

    void move_axis_stop(int direction) {
        static constexpr const char* kCommands[] = {":Qn#", ":Qs#", ":Qe#", ":Qw#"};
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid OnStep move-axis direction");
        }
        send_command_blind(kCommands[direction]);
    }

    void pulse_guide(int direction, int duration_ms) {
        static constexpr const char kCodes[] = {'n', 's', 'e', 'w'};
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid OnStep pulse guide direction");
        }
        int clamped = std::clamp(duration_ms, 0, 9999);
        char buf[16];
        std::snprintf(buf, sizeof(buf), ":Mg%c%04d#", kCodes[direction], clamped);
        // Fire-and-forget: the mount times the pulse internally and does not
        // send a response for this command.
        send_command_blind(buf);
    }

    SiteInfo get_site_info() {
        SiteInfo info;
        auto lat = parse_sexagesimal(send_command(":Gt#", true, 0));
        auto lon = parse_sexagesimal(send_command(":Gg#", true, 0));
        if (!lat.has_value() || !lon.has_value()) {
            throw AlpacaException("Invalid OnStep site info response");
        }
        info.latitude_degrees = lat.value();
        // :Gg# echoes back whatever was stored by :Sg# — negated (West-
        // positive) per format_longitude() — negate again here so this
        // wrapper's public API stays in ASCOM's East-positive convention.
        info.longitude_degrees = -lon.value();
        return info;
    }

    void set_latitude(double latitude_degrees) {
        // :St#/:Sg#-style "set" commands ack with a bare "1" and NO trailing
        // '#' on real OnStep firmware (confirmed against hardware — unlike
        // the ':G'-prefixed getters, which do terminate with '#'). Requiring
        // a hash terminator here waited out the full response timeout on
        // every call despite the ack having already arrived.
        std::string response = send_command(format_latitude(latitude_degrees), false, kSetAckTimeoutMs);
        if (response != "1") {
            throw AlpacaException("Mount rejected site latitude");
        }
    }

    void set_longitude(double longitude_degrees) {
        std::string response = send_command(format_longitude(longitude_degrees), false, kSetAckTimeoutMs);
        if (response != "1") {
            throw AlpacaException("Mount rejected site longitude");
        }
    }

    double get_sidereal_time_hours() {
        auto value = parse_sexagesimal(send_command(":GS#", true, 0));
        if (!value.has_value()) {
            throw AlpacaException("Invalid OnStep sidereal time response");
        }
        return value.value();
    }

    TimeInfo get_time() {
        TimeInfo info;
        std::string time_resp = send_command(":GL#", true, 0);
        auto time_val = parse_sexagesimal(time_resp);
        if (!time_val.has_value()) {
            throw AlpacaException("Invalid OnStep local time response");
        }
        double hours = time_val.value();
        info.hour = static_cast<int>(hours);
        double rem_min = (hours - info.hour) * 60.0;
        info.minute = static_cast<int>(rem_min);
        info.second = static_cast<int>(std::lround((rem_min - info.minute) * 60.0));

        std::string date_resp = send_command(":GC#", true, 0);
        // "MM/DD/YY" — not a sexagesimal value, parse directly.
        int mm = 1;
        int dd = 1;
        int yy = 0;
        if (std::sscanf(date_resp.c_str(), "%d/%d/%d", &mm, &dd, &yy) == 3) {
            info.month = mm;
            info.day = dd;
            info.year = yy;
        }

        auto offset = parse_sexagesimal(send_command(":GG#", true, 0));
        if (offset.has_value()) {
            info.utc_offset_hours = offset.value();
        }
        return info;
    }

    void set_time(const TimeInfo& info) {
        // See set_latitude() — :S-prefixed "set" commands ack with a bare
        // "1" and no '#' terminator on real hardware.
        char time_buf[24];
        std::snprintf(time_buf, sizeof(time_buf), ":SL%02d:%02d:%02d#", info.hour, info.minute, info.second);
        std::string time_response = send_command(time_buf, false, kSetAckTimeoutMs);
        if (time_response != "1") {
            throw AlpacaException("Mount rejected local time");
        }

        char date_buf[24];
        std::snprintf(date_buf, sizeof(date_buf), ":SC%02d/%02d/%02d#", info.month, info.day, info.year % 100);
        // :SC# on real LX200/OnStep firmware answers with two "1"-prefixed
        // strings (update-in-progress messages) rather than a single "1" —
        // tolerate any non-empty response rather than an exact match.
        std::string date_response = send_command(date_buf, false, kSetAckTimeoutMs);
        if (date_response.empty()) {
            throw AlpacaException("Mount rejected local date");
        }

        // :SG# expects "sHH#" or "sHH:MM#" with MM restricted to :00/:30/:45
        // (parsed via a strict integer-hours-then-optional-fixed-minutes
        // scanner, not a decimal float) — confirmed against real firmware
        // (Command.ino) and hardware: the previous "%+05.1f" ("+01.0")
        // format was accepted-looking but silently rejected ("0" ack) by
        // the mount every time.
        const char offset_sign = info.utc_offset_hours < 0.0 ? '-' : '+';
        const double abs_offset_hours = std::abs(info.utc_offset_hours);
        const int offset_whole_hours = static_cast<int>(abs_offset_hours);
        const double offset_frac_hours = abs_offset_hours - offset_whole_hours;
        char offset_buf[24];
        if (offset_frac_hours >= 0.7) {
            std::snprintf(offset_buf, sizeof(offset_buf), ":SG%c%d:45#", offset_sign, offset_whole_hours);
        } else if (offset_frac_hours >= 0.4) {
            std::snprintf(offset_buf, sizeof(offset_buf), ":SG%c%d:30#", offset_sign, offset_whole_hours);
        } else {
            std::snprintf(offset_buf, sizeof(offset_buf), ":SG%c%d#", offset_sign, offset_whole_hours);
        }
        std::string offset_response = send_command(offset_buf, false, kSetAckTimeoutMs);
        if (offset_response != "1") {
            throw AlpacaException("Mount rejected UTC offset");
        }
    }

private:
    static std::string normalize_command(const std::string& command) {
        std::string full = command;
        if (full.empty() || full.front() != ':') {
            full = ":" + full;
        }
        if (full.back() != '#') {
            full += "#";
        }
        return full;
    }

    bool connect_serial(const std::string& port_path, int baud_rate) {
        serial_fd_ = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            ALPACA_LOG_ERROR("OnStep",
                             "Failed to open serial port [" + port_path + "]: " + std::string(std::strerror(errno)));
            return false;
        }

        struct termios tty{};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }

        speed_t speed = B9600;
        switch (baud_rate) {
            case 9600:
                speed = B9600;
                break;
            case 19200:
                speed = B19200;
                break;
            case 38400:
                speed = B38400;
                break;
            case 57600:
                speed = B57600;
                break;
            case 115200:
                speed = B115200;
                break;
            case 230400:
                speed = B230400;
                break;
            default:
                speed = B9600;
                break;
        }
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);

        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag |= CREAD | CLOCAL;
        // Keep DTR asserted on close (matches the probe path) — an OnStep
        // controller behind CH340/CH341/native-USB-CDC resets its MCU on a
        // DTR edge, so tearing HUPCL down here would reset the board every
        // time the driver reconnects.
        tty.c_cflag &= ~HUPCL;
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
        tty.c_oflag &= ~OPOST;
        tty.c_oflag &= ~ONLCR;
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tty.c_cc[VMIN] = 0;
        // 100ms per read() syscall — NOT the probe path's 2s: read_response()'s
        // and drain_stale()'s software timeout budgets are only enforced in
        // increments of this value (each failed read() blocks for the full
        // VTIME regardless of the caller's shorter deadline), so a long VTIME
        // here made every non-acking blind command (e.g. pulse guide) and
        // every genuinely-silent response cost multiple real seconds instead
        // of the intended tens of milliseconds — confirmed against real
        // hardware, where this alone caused PulseGuide to block ~2s and
        // AbortSlew's multi-command stop sequence to cascade into a timeout.
        tty.c_cc[VTIME] = 1;

        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }

        if (!util::clear_nonblocking(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            return false;
        }

        return true;
    }

    void disconnect_serial() {
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
    }

    // Network path is a test-seam only (see ConnectionType::Network) —
    // exercised by the concurrency stress test against FakeMountServer, never
    // reachable through AlpacaHTTP's router or web UI for OnStep.
    bool connect_network(const std::string& host, int port) {
        constexpr int kConnectTimeoutMs = 7000;
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }

        sockaddr_in addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* result = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }

        if (!util::set_nonblocking(socket_fd_)) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        int rc = ::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        if (rc < 0) {
            struct pollfd pfd{};
            pfd.fd = socket_fd_;
            pfd.events = POLLOUT;
            int poll_rc = poll(&pfd, 1, kConnectTimeoutMs);
            int sock_err = 0;
            socklen_t len = sizeof(sock_err);
            if (poll_rc <= 0 || getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &sock_err, &len) != 0 || sock_err != 0) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
        }
        if (!util::clear_nonblocking(socket_fd_)) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        constexpr int kSocketTimeoutMs = 200;
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(kSocketTimeoutMs) * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        return true;
    }

    void disconnect_network() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    bool write_data(const std::string& data) {
        if (connection_type_ == ConnectionType::Serial) {
            return util::write_all(serial_fd_, data.c_str(), data.length());
        }
        return util::send_all(socket_fd_, data.c_str(), data.length(), MSG_NOSIGNAL);
    }

    bool read_serial_char(char& ch) {
        // Bounded read (VMIN/VTIME), held under mutex_ by design — the mutex
        // provides transaction atomicity for command/response pairs.
        ssize_t bytes_read = read(serial_fd_, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        return bytes_read == 1;
    }

    bool read_network_char(char& ch) {
        // Bounded recv (SO_RCVTIMEO), held under mutex_ by design.
        ssize_t bytes_received = recv(socket_fd_, &ch, 1, 0);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        return bytes_received == 1;
    }

    std::string read_response(bool require_hash_terminator, int timeout_ms) {
        std::string response;
        auto start = std::chrono::steady_clock::now();
        auto last_char_time = start;
        const int idle_break_ms = 100;

        while (true) {
            char ch;
            bool got_char = (connection_type_ == ConnectionType::Serial) ? read_serial_char(ch) : read_network_char(ch);
            if (got_char) {
                last_char_time = std::chrono::steady_clock::now();
                if (ch == '#') {
                    break;
                }
                if (ch == '\r' || ch == '\n') {
                    continue;
                }
                response.push_back(ch);
            }
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            if (elapsed.count() > timeout_ms) {
                if (!require_hash_terminator) {
                    return response;
                }
                throw AlpacaException("Timeout waiting for OnStep response");
            }
            if (!require_hash_terminator && !got_char && !response.empty()) {
                auto idle_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_char_time);
                if (idle_elapsed.count() > idle_break_ms) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return response;
    }

    mutable std::mutex mutex_;
    bool connected_;
    ConnectionType connection_type_;
    ConnectionInfo connection_info_;
    int serial_fd_;
    int socket_fd_;
};

OnStepProtocolWrapper::OnStepProtocolWrapper() : pimpl_(std::make_unique<Impl>()) {}
OnStepProtocolWrapper::~OnStepProtocolWrapper() = default;

OnStepProtocolWrapper& OnStepProtocolWrapper::instance() {
    static OnStepProtocolWrapper wrapper;
    return wrapper;
}

bool OnStepProtocolWrapper::connect(const ConnectionInfo& info) { return pimpl_->connect(info); }

void OnStepProtocolWrapper::disconnect() { pimpl_->disconnect(); }

bool OnStepProtocolWrapper::is_connected() const { return pimpl_->is_connected(); }

std::string OnStepProtocolWrapper::get_product_name() { return pimpl_->get_product_name(); }

std::string OnStepProtocolWrapper::get_version_number() { return pimpl_->get_version_number(); }

Position OnStepProtocolWrapper::get_position() { return pimpl_->get_position(); }

AltAz OnStepProtocolWrapper::get_alt_az() { return pimpl_->get_alt_az(); }

MountStatus OnStepProtocolWrapper::get_status() { return pimpl_->get_status(); }

void OnStepProtocolWrapper::set_target_ra(double ra_hours) { pimpl_->set_target_ra(ra_hours); }

void OnStepProtocolWrapper::set_target_dec(double dec_degrees) { pimpl_->set_target_dec(dec_degrees); }

bool OnStepProtocolWrapper::slew_to_target() { return pimpl_->slew_to_target(); }

void OnStepProtocolWrapper::sync_to_target() { pimpl_->sync_to_target(); }

void OnStepProtocolWrapper::abort_slew() { pimpl_->abort_slew(); }

void OnStepProtocolWrapper::start_tracking() { pimpl_->start_tracking(); }

void OnStepProtocolWrapper::stop_tracking() { pimpl_->stop_tracking(); }

void OnStepProtocolWrapper::park() { pimpl_->park(); }

void OnStepProtocolWrapper::set_park() { pimpl_->set_park(); }

void OnStepProtocolWrapper::unpark() { pimpl_->unpark(); }

void OnStepProtocolWrapper::find_home() { pimpl_->find_home(); }

void OnStepProtocolWrapper::select_max_slew_rate() { pimpl_->select_max_slew_rate(); }

void OnStepProtocolWrapper::move_axis_start(int direction, double rate_deg_per_sec) {
    pimpl_->move_axis_start(direction, rate_deg_per_sec);
}

void OnStepProtocolWrapper::move_axis_stop(int direction) { pimpl_->move_axis_stop(direction); }

void OnStepProtocolWrapper::pulse_guide(int direction, int duration_ms) { pimpl_->pulse_guide(direction, duration_ms); }

SiteInfo OnStepProtocolWrapper::get_site_info() { return pimpl_->get_site_info(); }

void OnStepProtocolWrapper::set_latitude(double latitude_degrees) { pimpl_->set_latitude(latitude_degrees); }

void OnStepProtocolWrapper::set_longitude(double longitude_degrees) { pimpl_->set_longitude(longitude_degrees); }

double OnStepProtocolWrapper::get_sidereal_time_hours() { return pimpl_->get_sidereal_time_hours(); }

TimeInfo OnStepProtocolWrapper::get_time() { return pimpl_->get_time(); }

void OnStepProtocolWrapper::set_time(const TimeInfo& info) { pimpl_->set_time(info); }

std::string OnStepProtocolWrapper::send_command(const std::string& command, bool require_hash_terminator,
                                                int timeout_ms_override) {
    return pimpl_->send_command(command, require_hash_terminator, timeout_ms_override);
}

void OnStepProtocolWrapper::send_command_blind(const std::string& command) { pimpl_->send_command_blind(command); }

void OnStepProtocolWrapper::flush_input() { pimpl_->flush_input(); }

}  // namespace alpacacore::vendor::onstep

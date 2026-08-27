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
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>

#include <cctype>
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
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::gemini {

namespace {
constexpr int kCommandDelayMs = 50;
constexpr int kReadCharDelayMs = 10;
constexpr int kMaxResponseLen = 32;
constexpr int kMaxCommandLen = 32;  // ">Bnnn#" + nul fits comfortably
constexpr int kHandshakeRetries = 3;
constexpr int kHandshakeRetryDelayS = 1;
constexpr int kHandshakeReadTimeoutS = 2;  // Shorter timeout during handshake
// Cover travel is mechanical, not an instant ack -- INDI's Rev2 adapter uses
// a long timeout for open/close specifically (vs. the short default used for
// light/brightness/status queries). 30s matches INDI's move/calibration
// timeout for the same hardware family; unconfirmed against real hardware.
constexpr int kCoverMoveTimeoutS = 30;

// Extract the trailing run of decimal digits from a '#'-terminated response
// (e.g. "*V206#" -> 206, "*J50#" -> 50). Confirmed against real hardware:
// responses are "*" + echoed command letter + decimal payload + "#".
bool parse_trailing_int(const std::string& resp, int& out) {
    std::string digits;
    for (auto it = resp.rbegin(); it != resp.rend(); ++it) {
        if (*it == '#') continue;
        if (std::isdigit(static_cast<unsigned char>(*it))) {
            digits.insert(digits.begin(), *it);
        } else if (!digits.empty()) {
            break;
        }
    }
    if (digits.empty()) {
        return false;
    }
    out = std::atoi(digits.c_str());
    return true;
}

// >S# is confirmed (against real hardware) to reply "*S<d1><d2><d3>#" -- three
// single-digit flags, not one combined number. d1 is the light on/off flag
// (e.g. "*S111#" while lit, "*S011#" once turned off); d2/d3 stayed "1" across
// every light/brightness change observed and are presumed cover-related flags
// that don't apply to this motorless model. Only d1 is needed here, so pull
// the digit immediately after 'S' rather than treating the payload as one
// trailing integer (parse_trailing_int would read "*S011#" as 11 -- nonzero,
// i.e. wrongly "on").
bool parse_light_flag(const std::string& resp, bool& out) {
    auto pos = resp.find('S');
    if (pos == std::string::npos || pos + 1 >= resp.size()) {
        return false;
    }
    char c = resp[pos + 1];
    if (!std::isdigit(static_cast<unsigned char>(c))) {
        return false;
    }
    out = (c != '0');
    return true;
}

// Rev2 >S# reply layout, per INDI's GeminiFlatpanelRev2Adapter::getStatus()
// (indilib/indi, gemini_flatpanel_adapters.cpp): "*S<id0><id1><motor><light><cover>#"
// -- a 2-digit device ID (must be "19" or "99", the two IDs INDI's adapter
// accepts) followed by three single-digit flags, 7 chars minimum before the
// '#'. This is a DIFFERENT layout from the Lite's "*S<light><f2><f3>#" (see
// parse_light_flag() above) -- do not reuse that parser here.
bool parse_rev2_status(const std::string& resp, FlatPanelMotorizedStatus& out) {
    if (resp.size() < 7 || resp[0] != '*' || resp[1] != 'S') {
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(resp[2])) || !std::isdigit(static_cast<unsigned char>(resp[3]))) {
        return false;
    }
    int id = (resp[2] - '0') * 10 + (resp[3] - '0');
    if (id != 19 && id != 99) {
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(resp[4])) || !std::isdigit(static_cast<unsigned char>(resp[5])) ||
        !std::isdigit(static_cast<unsigned char>(resp[6]))) {
        return false;
    }
    out.motor_running = (resp[4] != '0');
    out.light_on = (resp[5] != '0');
    int cover = resp[6] - '0';
    if (cover < 0 || cover > 3) {
        return false;
    }
    out.cover_state = static_cast<FlatPanelCoverState>(cover);
    return true;
}

// Pro >S# reply layout, per INDI's GeminiFlatpanelProAdapter::getStatus():
// the motor/light/cover digits sit at fixed positions 2, 4 and 6 with a
// letter tag after each one, followed by extra fields INDI ignores.
// Confirmed on hardware (firmware 107): "*S0M0L2C0D76C405O#" -> motor 0
// (stopped), light 0 (off), cover 2 (open); the trailing "0D76C405O" is
// presumed dew-heater flag + closed/open position calibration, not parsed.
// No 2-digit device-ID gate here, unlike parse_rev2_status().
bool parse_pro_status(const std::string& resp, FlatPanelMotorizedStatus& out) {
    if (resp.size() < 7 || resp[0] != '*' || resp[1] != 'S') {
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(resp[2])) || !std::isdigit(static_cast<unsigned char>(resp[4])) ||
        !std::isdigit(static_cast<unsigned char>(resp[6]))) {
        return false;
    }
    int motor = resp[2] - '0';
    int light = resp[4] - '0';
    int cover = resp[6] - '0';
    if (motor > 1 || light > 1 || cover > 3) {
        return false;
    }
    out.motor_running = (motor != 0);
    out.light_on = (light != 0);
    out.cover_state = static_cast<FlatPanelCoverState>(cover);
    return true;
}
}  // namespace

std::string_view expected_flatpanel_handshake_reply(FlatPanelModel model) {
    switch (model) {
        case FlatPanelModel::Lite:
            return "*HGeminiFlatPanelLite#";
        case FlatPanelModel::Rev2:
            return "*HGeminiFlatPanel#";
        case FlatPanelModel::Pro:
            return "*HGeminiFlatPanelPro#";
    }
    return "*H";
}

bool is_flatpanel_handshake_reply(const std::string& reply) {
    return reply.size() >= 2 && reply[0] == '*' && reply[1] == 'H';
}

// Probe a serial port with the flat panel's identity handshake (>H#).
// Returns true if a well-formed ('#'-terminated) response was received.
//
// Mirrors the Gemini focuser's probe_port(): the CH340 USB-serial adapter
// asserts DTR on open, which can reset the panel's microcontroller. HUPCL is
// cleared before closing so DTR stays high and a subsequent connect() doesn't
// trigger a second reset.
static bool probe_port(const std::string& port_path) {
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

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~HUPCL;  // Keep DTR high on close -- prevents MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 50;  // 5-second per-character timeout

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return false;
    }

    if (!util::clear_nonblocking(fd)) {
        close(fd);
        return false;
    }
    tcflush(fd, TCIOFLUSH);

    // Wait for MCU boot after the DTR reset, then try the handshake. Two
    // attempts: first waits 2s for boot, second is a quick retry.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 1));
        tcflush(fd, TCIOFLUSH);

        const char* cmd = ">H#";
        if (!util::write_all(fd, cmd, std::strlen(cmd))) {
            continue;
        }

        // Must hold the full ">H#" reply -- confirmed against real hardware
        // as "*HGeminiFlatPanelLite#" (22 chars). A too-small buffer here
        // silently truncates before the '#' terminator, so the well-formed
        // check below never passes and the port is wrongly rejected.
        char resp[kMaxResponseLen] = {};
        int total = 0;
        auto start = std::chrono::steady_clock::now();
        while (total < static_cast<int>(sizeof(resp)) - 1) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
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
                continue;  // VTIME timeout, keep trying
            } else {
                break;
            }
        }

        // See is_flatpanel_handshake_reply()'s doc comment: requiring the
        // "*H" prefix (rather than any well-formed '#'-terminated reply) is
        // what lets auto-detect discriminate the flat panel from the Gemini
        // focuser when both share the same USB-serial by-id patterns.
        if (is_flatpanel_handshake_reply(std::string(resp, static_cast<std::size_t>(total)))) {
            // HUPCL was already cleared in the tty config above; no need to
            // re-fetch/re-clear/re-set it here.
            close(fd);
            return true;
        }

        ALPACA_LOG_DEBUG("Gemini", "Flat panel probe attempt " + std::to_string(attempt + 1) +
                                       " got no valid response from " + port_path);
    }

    close(fd);
#else
    (void)port_path;
#endif
    return false;
}

#ifndef _WIN32
namespace {
// The raw /dev/ttyUSB*//dev/ttyACM* fallback below has no by-id symlink name
// to filter on, so without this check it would open (and DTR-reset, per
// probe_port()'s doc comment) EVERY serial device on the box -- unrelated
// FTDI dongles, GPS receivers, or a mount controller sharing the same
// ttyUSB/ttyACM namespace -- not just flat-panel-shaped hardware. Filters
// the raw fallback exactly as narrowly as the by-id loop above
// (CH340/CH341/1a86/USB_Serial/Espressif) via the shared sysfs
// vendor-descriptor helper. Returns false -- never probe -- if the
// descriptor can't be read.
bool raw_port_looks_like_flatpanel_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(
        *descriptor, {"1a86", "303a", "CH340", "CH341", "Espressif", "USB_Serial", "USB Serial"});
}
}  // namespace
#endif

std::vector<GeminiFlatPanelPortInfo> enumerate_gemini_flatpanel_ports() {
    std::vector<GeminiFlatPanelPortInfo> results;

#ifndef _WIN32
    // Resolved (canonical) paths already probed, so the raw-node pass below
    // never re-opens a port the by-id pass already tried. Re-opening a port
    // is not just wasted time: these controllers (ESP32/CH340-class) reset
    // on DTR toggle at open(), so probing the same physical device twice
    // means it audibly reboots twice.
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

            // Confirmed against real hardware: the panel's controller is an
            // ESP32-class board using Espressif's native USB-serial/JTAG stack
            // (by-id name "usb-Espressif_USB_JTAG_serial_debug_unit_..."), not a
            // CH340/CH341 adapter like the Gemini focuser. Keep the CH340/CH341/
            // generic USB_Serial patterns too in case a different panel revision
            // uses an external USB-serial chip instead.
            bool is_candidate = (name.find("USB_Serial") != std::string::npos) ||
                                (name.find("CH340") != std::string::npos) ||
                                (name.find("CH341") != std::string::npos) || (name.find("1a86") != std::string::npos) ||
                                (name.find("Espressif") != std::string::npos);
            if (!is_candidate) continue;

            // Same hot-unplug race as the raw-node loop below: the physical
            // device can vanish between list_serial_by_id() yielding this
            // symlink and here. Use the error_code overload so a mid-scan
            // unplug just skips this entry instead of throwing
            // filesystem_error out of the whole enumeration -- which would
            // discard any results already collected and skip the raw-node
            // fallback entirely for this connection attempt.
            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            candidates.push_back({resolved, name});
        }
    }

    // Always also probe raw device nodes directly, not only when
    // /dev/serial/by-id is absent. /dev/serial/by-id names entries from the
    // USB descriptor's reported vendor/model/serial strings, and generic
    // CH340/CH341 adapters (used by this panel, and commonly also by mount
    // controllers on the same box) report identical strings with no
    // per-device serial number. When two such adapters are plugged in at
    // once, udev's by-id naming collides and only ONE of them gets a
    // symlink -- the other is silently absent from by-id even though the
    // directory itself exists, so the loop above never sees it. Cover BOTH
    // naming schemes here too: classic USB-serial adapters enumerate as
    // /dev/ttyUSBn, while an Espressif native-USB-serial/JTAG panel
    // enumerates as /dev/ttyACMn via the kernel's cdc_acm driver.
    //
    // raw_port_looks_like_flatpanel_candidate() filters this to the same
    // CH340/CH341/Espressif hardware class the by-id loop above matches by
    // name -- without it, this loop would open (and DTR-reset) any serial
    // device on the box, including unrelated hardware sharing the
    // ttyUSB/ttyACM namespace (a mount controller especially, since this
    // runs on every reconnect, not just once).
    for (const char* prefix : {"/dev/ttyUSB", "/dev/ttyACM"}) {
        for (int i = 0; i < 10; ++i) {
            std::string port = prefix + std::to_string(i);
            if (!alpacacore::util::path_exists(port)) continue;

            // Hot-pluggable node: it can vanish between the exists() check
            // above and here (or during the several-second probe_port() call
            // below, widening the window further across this 10-port scan).
            // Use the error_code overload so a mid-scan unplug skips this
            // port instead of throwing filesystem_error out of the whole
            // enumeration and stranding any real panel on a later slot.
            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(port, canon_ec).string();
            if (canon_ec) continue;
            if (probed.count(resolved) != 0) continue;
            if (!raw_port_looks_like_flatpanel_candidate(resolved)) continue;
            probed.insert(resolved);
            candidates.push_back({port, ""});
        }
    }

    // Probe every candidate concurrently: a non-responsive port costs the full
    // handshake timeout, so serial probing scaled linearly with adapter count
    // (issue #218); one thread per port bounds the scan to one port's worst case.
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        ALPACA_LOG_INFO("Gemini",
                        "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") + " for a flat panel...");
        workers.emplace_back([&, i] { found[i] = probe_port(candidates[i].path) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!found[i]) continue;
        ALPACA_LOG_INFO("Gemini", "Found flat panel on " + candidates[i].path);
        results.push_back({candidates[i].path, candidates[i].name});
    }
#endif

    return results;
}

class GeminiFlatPanelProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    std::string connect(const FlatPanelConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;

        connect_serial();

        // Handshake: identity query with retries, same cadence as the Gemini
        // focuser (worst case ~9.1s, under the ASCOM client ~10s timeout).
        bool success = false;
        int saved_timeout = config_.serial_timeout_s;
        config_.serial_timeout_s = kHandshakeReadTimeoutS;

        for (int attempt = 0; attempt < kHandshakeRetries && !success; ++attempt) {
            if (attempt == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else if (attempt == 1) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(kHandshakeRetryDelayS));
            }

#ifndef _WIN32
            tcflush(serial_fd_, TCIOFLUSH);
#else
            PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#endif

            try {
                // Confirmed against real hardware: >H# replies
                // "*HGeminiFlatPanelLite#". Require the "*H" prefix here too
                // (PR #143 review), not just in auto-detect's probe_port():
                // a manually configured portPath is the ONE path that never
                // went through the probe, and pointing it at the Gemini
                // focuser's port (whose MyFocuserPro2 firmware answers
                // unrelated queries with its own '#'-terminated replies)
                // would otherwise "connect" to the wrong device and parse
                // garbage from every later >S#/>J# reply. Better to fail the
                // handshake loudly at connect.
                std::string reply = send_command_locked(">H#");
                if (is_flatpanel_handshake_reply(reply)) {
                    success = true;
                    // Warn (don't fail) when the identity string doesn't match
                    // the configured model: each model parses >S# differently,
                    // so a mismatch usually means the wrong flatPanelModel was
                    // picked in the web UI and later status reads will fail.
                    const std::string_view expected = expected_flatpanel_handshake_reply(config_.model);
                    if (reply != expected) {
                        ALPACA_LOG_WARN("Gemini", "Flat panel handshake reply '" + reply + "' does not match the " +
                                                      "configured model's expected '" + std::string(expected) +
                                                      "' -- check the Flat Panel Model setting");
                    }
                } else {
                    ALPACA_LOG_WARN("Gemini", "Flat panel handshake attempt " + std::to_string(attempt + 1) +
                                                  " got a non-flat-panel reply: " + reply);
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Gemini",
                                "Flat panel handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }

        config_.serial_timeout_s = saved_timeout;

        if (!success) {
            disconnect_locked();
            throw AlpacaException(
                "Gemini flat panel handshake failed after " + std::to_string(kHandshakeRetries) + " attempts",
                AlpacaError::NotConnected);
        }

        std::string firmware;
        try {
            std::string resp = send_command_locked(">V#");
            int ver = 0;
            if (parse_trailing_int(resp, ver)) {
                firmware = std::to_string(ver);
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Gemini", "Flat panel firmware query failed: " + std::string(e.what()));
        }

        // INDI's Rev2 adapter rejects firmware < 402 outright; we only warn
        // (not confirmed against real hardware whether older Rev2 units in
        // the field would otherwise work fine for open/close/status).
        if (config_.model == FlatPanelModel::Rev2 && !firmware.empty()) {
            int ver = std::atoi(firmware.c_str());
            if (ver < 402) {
                ALPACA_LOG_WARN("Gemini", "Flat panel Rev2 firmware " + firmware +
                                              " is older than the minimum (402) INDI's Rev2 adapter requires -- "
                                              "cover commands may not behave as expected");
            }
        }

        connected_ = true;
        ALPACA_LOG_INFO("Gemini", "Flat panel connected" + (firmware.empty() ? "" : (", firmware: " + firmware)));
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

    bool get_light_on() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(">S#");
        bool on = false;
        if (!parse_light_flag(resp, on)) {
            throw AlpacaException("Failed to parse light status: " + resp, AlpacaError::DriverException);
        }
        return on;
    }

    int get_brightness() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(">J#");
        int val = 0;
        if (!parse_trailing_int(resp, val)) {
            throw AlpacaException("Failed to parse brightness: " + resp, AlpacaError::DriverException);
        }
        return val;
    }

    void light_on() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_locked(">L#");
    }

    void light_off() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_locked(">D#");
    }

    void set_brightness(int value) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        char cmd[kMaxCommandLen];
        std::snprintf(cmd, sizeof(cmd), ">B%d#", value);
        send_command_locked(cmd);
    }

    FlatPanelMotorizedStatus get_motorized_status() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(">S#");
        FlatPanelMotorizedStatus status;
        const bool ok =
            (config_.model == FlatPanelModel::Pro) ? parse_pro_status(resp, status) : parse_rev2_status(resp, status);
        if (!ok) {
            throw AlpacaException("Failed to parse motorized flat panel status: " + resp, AlpacaError::DriverException);
        }
        return status;
    }

    // Rev2 firmware replies with the exact completion string; Pro firmware
    // acks with variants ("*O", "*O#", "*OOpened#" per INDI), so only the
    // "*<letter>" prefix is required there.
    bool is_cover_reply_ok(const std::string& resp, const std::string& exact) const {
        if (config_.model == FlatPanelModel::Pro) {
            return resp.size() >= 2 && resp[0] == '*' && resp[1] == exact[1];
        }
        return resp == exact;
    }

    void open_cover() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        int saved_timeout = config_.serial_timeout_s;
        config_.serial_timeout_s = kCoverMoveTimeoutS;
        std::string resp;
        try {
            resp = send_command_locked(">O#");
        } catch (...) {
            config_.serial_timeout_s = saved_timeout;
            throw;
        }
        config_.serial_timeout_s = saved_timeout;
        if (!is_cover_reply_ok(resp, "*OOpened#")) {
            throw AlpacaException("Unexpected open cover reply: " + resp, AlpacaError::DriverException);
        }
    }

    void close_cover() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        int saved_timeout = config_.serial_timeout_s;
        config_.serial_timeout_s = kCoverMoveTimeoutS;
        std::string resp;
        try {
            resp = send_command_locked(">C#");
        } catch (...) {
            config_.serial_timeout_s = saved_timeout;
            throw;
        }
        config_.serial_timeout_s = saved_timeout;
        if (!is_cover_reply_ok(resp, "*CClosed#")) {
            throw AlpacaException("Unexpected close cover reply: " + resp, AlpacaError::DriverException);
        }
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Gemini flat panel not connected", AlpacaError::NotConnected);
        }
    }

    void connect_serial() {
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

        speed_t baud = B9600;
        switch (config_.baud_rate) {
            case 9600:
                baud = B9600;
                break;
            case 19200:
                baud = B19200;
                break;
            case 38400:
                baud = B38400;
                break;
            case 57600:
                baud = B57600;
                break;
            case 115200:
                baud = B115200;
                break;
            default:
                baud = B9600;
                break;
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
        tty.c_cflag &= ~HUPCL;  // Keep DTR high on close -- prevents MCU reset on reopen

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
#endif
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
            throw AlpacaException("Write failed: " + std::string(std::strerror(errno)), AlpacaError::DriverException);
        }
#endif
    }

    std::string read_response() {
        std::string response;
        response.reserve(kMaxResponseLen);
        char ch = 0;

        int timeout_ms = config_.serial_timeout_s * 1000;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
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
                if (response.length() >= kMaxResponseLen) {
                    throw AlpacaException("Response too long", AlpacaError::DriverException);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReadCharDelayMs));
            }
        }

        ALPACA_LOG_TRACE("Gemini", "Flat panel response: " + response);
        return response;
    }

    std::string send_command_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("Gemini", "Flat panel command: " + cmd);
        write_data(cmd);
        // The Pro firmware answers >S# in ~27 ms wire-to-wire (measured on a
        // Pi, 18-char reply), and its CoverState must land inside ConformU's
        // 0.1 s FAST target; the fixed pre-read settle below pushed a live
        // read to ~0.13 s. read_response() already blocks per byte (VTIME),
        // so the Pro path reads immediately. Lite/Rev2 keep the settle they
        // were ConformU-validated with -- no Lite/Rev2 unit was on hand to
        // prove they tolerate its removal.
        if (config_.model != FlatPanelModel::Pro) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kCommandDelayMs));
        }
        return read_response();
    }

    mutable std::mutex mutex_;
    FlatPanelConnectionConfig config_;
    bool connected_ = false;

#ifdef _WIN32
    HANDLE serial_handle_ = INVALID_HANDLE_VALUE;
#else
    int serial_fd_ = -1;
#endif
};

// --- GeminiFlatPanelProtocolWrapper public interface forwarding ---

GeminiFlatPanelProtocolWrapper::GeminiFlatPanelProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

GeminiFlatPanelProtocolWrapper::~GeminiFlatPanelProtocolWrapper() = default;

std::string GeminiFlatPanelProtocolWrapper::connect(const FlatPanelConnectionConfig& config) {
    return impl_->connect(config);
}

void GeminiFlatPanelProtocolWrapper::disconnect() { impl_->disconnect(); }

bool GeminiFlatPanelProtocolWrapper::is_connected() const { return impl_->is_connected(); }

bool GeminiFlatPanelProtocolWrapper::get_light_on() { return impl_->get_light_on(); }

int GeminiFlatPanelProtocolWrapper::get_brightness() { return impl_->get_brightness(); }

void GeminiFlatPanelProtocolWrapper::light_on() { impl_->light_on(); }

void GeminiFlatPanelProtocolWrapper::light_off() { impl_->light_off(); }

void GeminiFlatPanelProtocolWrapper::set_brightness(int value) { impl_->set_brightness(value); }

FlatPanelMotorizedStatus GeminiFlatPanelProtocolWrapper::get_motorized_status() {
    return impl_->get_motorized_status();
}

void GeminiFlatPanelProtocolWrapper::open_cover() { impl_->open_cover(); }

void GeminiFlatPanelProtocolWrapper::close_cover() { impl_->close_cover(); }

}  // namespace alpacacore::vendor::gemini

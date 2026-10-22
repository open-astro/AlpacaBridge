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
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
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

// Operator-facing owner name for the serial-port registry (#1202): the probe
// and the connected link use the same one, so the auto-detect scan can tell
// a connected OnStep mount (a sibling) from another vendor's device.
constexpr const char* kSerialOwner = "OnStep mount";

// Per-read wait on the connected link. The fd is non-blocking and every read
// goes through util::tty_read(), so this is the poll() bound that replaced
// the old VTIME=1 (100 ms) read window.
constexpr int kSerialReadSliceMs = 100;

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
    // See .github/instructions/onstep.instructions.md for the full :GU# status-character table,
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
    status.is_guide_active = raw.find('g') != std::string::npos;
    status.is_pulse_guide_active = raw.find('G') != std::string::npos;
    status.is_homing = raw.find('h') != std::string::npos;
    status.is_parking = raw.find('I') != std::string::npos;
    // OnStepX writes the mount type first ('E' GEM, 'K' fork, 'A' alt-az,
    // 'L' alt-alt), then the pier side: 'T' east, 'W' west, 'o' none. 'E' is
    // therefore always present on a GEM and says nothing about the pier
    // side (#1217; HM-17 traces nNpEW180 = west, nNpET1:0 = east).
    if (raw.find('T') != std::string::npos) {
        status.side_of_pier = 0;  // pier east
    } else if (raw.find('W') != std::string::npos) {
        status.side_of_pier = 1;  // pier west
    } else {
        status.side_of_pier =
            -1;  // 'o' (pier side none/unknown) or not reported — caller falls back to hour-angle calc
    }
    return status;
}

// Line settings shared by the probe and the connected link: 8N1 raw at
// @p speed, with HUPCL cleared so DTR stays asserted on close.
//
// OnStep firmware runs almost exclusively on Arduino/Teensy/ESP32/STM32
// boards behind CH340/CH341/CP2102/native-USB-CDC adapters. These reset the
// MCU when DTR asserts on port open (same issue as the Gemini focuser
// wrapper) -- keeping HUPCL clear means the probe's close does not drop DTR,
// so the driver's subsequent real connect() doesn't trigger a second reset.
void configure_onstep_line(struct termios& tty, speed_t speed) {
    util::make_raw_8n1(tty, speed);
    tty.c_cflag &= ~static_cast<tcflag_t>(HUPCL);
}

// Send one '#'-terminated query on an open probe link and collect the reply
// (without the '#') for at most @p budget_ms. Empty on a write failure,
// silence or a read error.
std::string probe_query(int fd, const char* cmd, int budget_ms) {
    if (!util::tty_write_all(fd, cmd, std::char_traits<char>::length(cmd))) {
        return "";
    }
    std::string resp;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (resp.size() < 31) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            break;
        }
        char ch = 0;
        const ssize_t r = util::tty_read(fd, &ch, 1, static_cast<int>(remaining));
        if (r != 1) {
            break;  // 0: silent for the rest of the budget; -1: read error or hangup
        }
        if (ch == '#') {
            break;
        }
        resp.push_back(ch);
    }
    return resp;
}

// Probe a serial port for an OnStep controller by sending :GVP# and checking
// for "OnStep" or "On-Step" in the response; on a match, also read :GVN#
// (into @p version) under the same claim, so no other scan or connect can
// take the port between the two. Returns the :GVP# identity, or "" when the
// port is not an OnStep controller or could not be opened (a refused claim
// is logged at DEBUG: the port belongs to a connected device or another
// scan is still probing it).
std::string probe_onstep_port(const std::string& port_path, std::string& version) {
#ifndef _WIN32
    std::string error;
    util::SerialLink link = util::SerialLink::open(
        port_path, util::SerialClaimKind::Probe, kSerialOwner,
        [](struct termios& tty) { configure_onstep_line(tty, B9600); }, &error);
    if (!link) {
        ALPACA_LOG_DEBUG("OnStep", "Not probing " + port_path + ": " + error);
        return "";
    }
    const int fd = link.fd();
    tcflush(fd, TCIOFLUSH);

    // Wait for the MCU to finish booting after the DTR-triggered reset, then
    // try the identity handshake. Two attempts: first waits for boot, second
    // is a quick retry in case the first command landed mid-boot.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 2 : 1));
        tcflush(fd, TCIOFLUSH);

        const std::string response = probe_query(fd, ":GVP#", 3000);
        if (response.find("OnStep") != std::string::npos || response.find("On-Step") != std::string::npos) {
            tcflush(fd, TCIOFLUSH);
            version = probe_query(fd, ":GVN#", 2000);
            return response;
        }

        ALPACA_LOG_DEBUG("OnStep",
                         "Probe attempt " + std::to_string(attempt + 1) + " got no OnStep identity from " + port_path);
    }
#else
    (void)port_path;
    (void)version;
#endif
    return "";
}

#ifndef _WIN32
// The raw /dev/ttyUSB*//dev/ttyACM* fallback below has no by-id symlink name
// to filter on, so without this check it would open EVERY serial device on
// the box -- unrelated FTDI dongles, GPS receivers, or a flat panel/focuser
// sharing the same ttyUSB/ttyACM namespace -- not just OnStep-shaped
// hardware. Filters the raw fallback exactly as narrowly as the by-id loop
// below via the shared sysfs vendor-descriptor helper. Returns false --
// never probe -- if the descriptor can't be read.
bool raw_port_looks_like_onstep_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(
        *descriptor, {"Prolific", "PL2303", "067b", "FTDI", "0403", "CP210", "10c4", "Silicon_Labs", "CH340", "CH341",
                      "1a86", "USB_Serial", "USB Serial", "Arduino", "Teensy"});
}

// Lists /dev entries whose filename starts with prefix (e.g. "ttyUSB"),
// sorted for deterministic probe order. Unlike a fixed `for (i = 0; i < N;
// ++i)` loop over /dev/<prefix>0..N-1, this doesn't silently stop looking on
// a box with N or more matching devices already enumerated -- there's no
// hardware limit on how many USB-serial adapters can be plugged into a hub.
// Built entirely on std::error_code overloads (matching
// alpacacore::util::list_serial_by_id()'s pattern) so a mid-scan unplug ends
// the scan with whatever entries were already found instead of throwing
// filesystem_error out of the whole enumeration.
std::vector<std::string> list_dev_nodes_with_prefix(const std::string& prefix) {
    std::vector<std::string> results;
    std::error_code dir_ec;
    for (auto it = std::filesystem::directory_iterator("/dev", dir_ec);
         !dir_ec && it != std::filesystem::directory_iterator(); it.increment(dir_ec)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0, prefix.size(), prefix) == 0) {
            results.push_back(it->path().string());
        }
    }
    std::sort(results.begin(), results.end());
    return results;
}
#endif

// Never destroyed: the wrapper singleton can disconnect from its destructor at
// exit, after function-local statics built later than it are gone.
std::mutex& link_loss_hook_mutex() {
    static auto* m = new std::mutex;  // NOLINT(cppcoreguidelines-owning-memory)
    return *m;
}

std::function<void(detail::LinkLossHookPoint)>& link_loss_hook() {
    static auto* hook = new std::function<void(detail::LinkLossHookPoint)>;  // NOLINT(cppcoreguidelines-owning-memory)
    return *hook;
}

void run_link_loss_hook(detail::LinkLossHookPoint point) {
    std::function<void(detail::LinkLossHookPoint)> hook;
    {
        std::lock_guard<std::mutex> lock(link_loss_hook_mutex());
        hook = link_loss_hook();
    }
    if (hook) {
        hook(point);
    }
}

}  // namespace

std::vector<OnStepPortInfo> enumerate_onstep_ports() {
    std::vector<OnStepPortInfo> results;

#ifndef _WIN32
    // Resolved (canonical) paths already probed via by-id, so the raw-node
    // pass below never re-opens a port the by-id pass already tried.
    std::set<std::string> probed;

    // One candidate, in scan order (#1202). A port another vendor's connected
    // device holds is never opened. A port a connected OnStep mount holds is
    // not probed either, but keeps its place in the results so the auto-detect
    // index of every other mount stays stable; a driver that picks it is
    // refused by its link claim, which names the holder.
    auto consider = [&results](const std::string& resolved, const std::string& name) {
        switch (util::serial_candidate_use(resolved, kSerialOwner)) {
            case util::SerialCandidateUse::HeldByOther:
                ALPACA_LOG_DEBUG("OnStep", "Skipping " + resolved + ": held by another connected device");
                return;
            case util::SerialCandidateUse::HeldBySibling:
                ALPACA_LOG_DEBUG("OnStep", "Not probing " + resolved + ": a connected OnStep mount holds it");
                results.push_back({resolved, name, ""});
                return;
            case util::SerialCandidateUse::Free:
                break;
        }
        ALPACA_LOG_INFO("OnStep", "Probing " + resolved + (name.empty() ? "" : " (" + name + ")") + "...");
        std::string version;
        const std::string identity = probe_onstep_port(resolved, version);
        if (!identity.empty()) {
            ALPACA_LOG_INFO("OnStep", "Found OnStep mount on " + resolved + " (" + identity + ")");
            results.push_back({resolved, name, version});
        }
    };

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;

            bool is_candidate =
                (name.find("Prolific") != std::string::npos) || (name.find("PL2303") != std::string::npos) ||
                (name.find("067b") != std::string::npos) || (name.find("FTDI") != std::string::npos) ||
                (name.find("0403") != std::string::npos) || (name.find("CP210") != std::string::npos) ||
                (name.find("10c4") != std::string::npos) || (name.find("Silicon_Labs") != std::string::npos) ||
                (name.find("CH340") != std::string::npos) || (name.find("CH341") != std::string::npos) ||
                (name.find("1a86") != std::string::npos) || (name.find("USB_Serial") != std::string::npos) ||
                (name.find("USB-Serial") != std::string::npos) || (name.find("Arduino") != std::string::npos) ||
                (name.find("Teensy") != std::string::npos);
            if (!is_candidate) continue;

            // error_code overload: a device unplugged after the directory
            // scan leaves a dangling symlink; skip it instead of aborting
            // the whole enumeration and discarding results already found.
            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            consider(resolved, name);
        }
    }

    // Always also probe raw device nodes directly, not only when
    // /dev/serial/by-id is absent. Generic Prolific/FTDI/CP210x/CH340
    // adapters (used by OnStep's serial cable, and commonly also by other
    // devices on the same box) can report identical descriptor strings with
    // no per-device serial number, so when two such adapters are plugged in
    // at once, udev's by-id naming collides and only ONE of them gets a
    // symlink -- the other is silently absent from by-id even though the
    // directory itself exists, so the loop above never sees it. Cover BOTH
    // naming schemes here too: classic USB-serial adapters enumerate as
    // /dev/ttyUSBn, while Arduino Mega/Due/Teensy/ESP32 boards running
    // OnStep commonly enumerate as /dev/ttyACMn (native USB-CDC).
    for (const char* prefix : {"ttyUSB", "ttyACM"}) {
        for (const std::string& port : list_dev_nodes_with_prefix(prefix)) {
            // Hot-pluggable node: it can vanish between the directory scan
            // above and here (or during the probe below). Use the
            // error_code overload so a mid-scan unplug skips this port
            // instead of throwing filesystem_error out of the whole
            // enumeration.
            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(port, canon_ec).string();
            if (canon_ec) continue;
            if (probed.count(resolved) != 0) continue;
            if (!raw_port_looks_like_onstep_candidate(resolved)) continue;
            probed.insert(resolved);
            consider(resolved, "");
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

        // A lost link whose fd could not be closed when the loss was found
        // (an exchange held the mutex) is closed here, so a reconnect is not
        // refused as a second mount.
        if (connected_ && link_lost_) {
            disconnect_locked();
        }

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
        last_connect_error_.clear();

        bool success = false;
        if (info.type == ConnectionType::Serial) {
            success = connect_serial(info.port_path, info.baud_rate);
        } else {
            success = connect_network(info.host, info.tcp_port);
        }

        connected_ = success;
        if (success) {
            // Only a successful connect (or disconnect()) clears the loss
            // text; the driver's relink calls disconnect() first (decision
            // 0009 point 5). The stamps stay for the reconnecting driver.
            clear_loss();
            mark_alive();
        }
        return success;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
        clear_loss();
    }

    void disconnect_locked() {
        if (!connected_) {
            return;
        }
        {
            // Down, node cleared and generation bumped in one step, before the
            // port closes: a link_alive() that took its snapshot before this
            // cannot record a loss after it (open-astro#1289). connected_ is
            // published first, so link_alive() never reports a closing fd as live.
            std::lock_guard<std::mutex> id_lock(link_id_mutex_);
            connected_ = false;
            link_path_.clear();
            ++link_generation_;
        }
        run_link_loss_hook(detail::LinkLossHookPoint::DisconnectBeforeClose);
        if (connection_type_ == ConnectionType::Serial) {
            disconnect_serial();
        } else {
            disconnect_network();
        }
    }

    // ── Link loss (decision 0009, #1241) ──

    // Decision 0005 (#1378): the loss and last-alive stamps read the clock of
    // the driver that uses this process-wide wrapper, so the driver's #521
    // outage is measured on one clock. Default: the process clock.
    void set_task_clock(util::TaskClock& c) { clock_.store(&c); }
    void release_task_clock(util::TaskClock& c) {
        util::TaskClock* expected = &c;
        clock_.compare_exchange_strong(expected, &util::default_task_clock());
    }

    bool link_alive() {
        if (!connected_) {
            return false;
        }
        if (link_lost_) {
            // A loss found while an exchange held the fd: close it now if
            // nothing does, so the replugged adapter can take the old name.
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            // Re-checked under the lock: a connect may have cleared the loss
            // and opened a fresh link in between.
            if (lock.owns_lock() && connected_ && link_lost_) {
                disconnect_locked();
            }
            return false;
        }
        bool present = true;
        std::uint64_t generation = 0;
        std::string path;
        {
            std::lock_guard<std::mutex> id_lock(link_id_mutex_);
            generation = link_generation_;
            if (!link_path_.empty()) {
                struct stat st {};
                if (::stat(link_path_.c_str(), &st) != 0) {
                    // Only a path that is gone counts; a transient lookup
                    // error (EACCES, EINTR) is not evidence of loss.
                    present = !(errno == ENOENT || errno == ENOTDIR);
                } else {
                    present = st.st_dev == link_dev_ && st.st_ino == link_ino_;
                }
            }
            if (!present) {
                path = link_path_;  // for the loss text; copied only when it is needed
            }
        }
        if (present) {
            mark_alive();
            return connected_;
        }
        run_link_loss_hook(detail::LinkLossHookPoint::AliveBeforeRecord);
        // Nothing holds mutex_ here, so a Disconnect or a reconnect can have
        // run since the stat(). Record the loss only for the link that was
        // checked: otherwise a deliberate Disconnect would leave a loss (and
        // the next Connect would send the #521 stops), or a relink would see
        // its fresh link marked lost and closed (open-astro#1289).
        if (!note_loss_if_current(generation, "the adapter at " + path + " was removed or re-enumerated")) {
            return connected_ && !link_lost_;
        }
        run_link_loss_hook(detail::LinkLossHookPoint::AliveBeforeClose);
        // Best effort: an exchange in progress will find the loss itself.
        // Re-checked under the lock: a relink may have opened a fresh link
        // since the record, and a successful connect clears the loss.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && connected_ && link_lost_) {
            disconnect_locked();
        }
        return connected_ && !link_lost_;
    }

    std::string link_lost_reason() const {
        std::lock_guard<std::mutex> lock(loss_mutex_);
        return link_lost_ ? lost_reason_ : std::string{};
    }

    std::optional<std::chrono::steady_clock::time_point> consume_link_lost_at() { return link_lost_at_.consume(); }

    std::optional<std::chrono::steady_clock::time_point> consume_last_alive_before_loss() {
        std::lock_guard<std::mutex> lock(loss_mutex_);
        auto at = alive_before_loss_;
        alive_before_loss_.reset();
        return at;
    }

    std::string last_connect_error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_connect_error_;
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    std::string send_command(const std::string& command, bool require_hash_terminator, int timeout_ms_override) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_live_link_locked();

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
            fail_write_locked();
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
        require_live_link_locked();

        std::string full_command = normalize_command(command);
        ALPACA_LOG_TRACE("OnStep", "CMD " + full_command + " (blind)");

        if (!write_data(full_command)) {
            fail_write_locked();
        }

        // Many OnStep "blind" commands still ack with a stray byte on some
        // firmware. Drain whatever arrives within a short window rather than
        // leaving it to corrupt the next command's response framing — the
        // same lesson learned from iOptron's blind-command handling. Each
        // read waits a full kSerialReadSliceMs (100ms) when nothing arrives
        // regardless of this budget, so a lower value here still costs one
        // full read slice — kept short so that AbortSlew's five
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

    std::optional<int> get_mount_coords_mode() {
        // Bare digit, no '#' (OnStepX sets suppressFrame for :GXEE#).
        const std::string reply = send_command(":GXEE#", false, kSetAckTimeoutMs);
        if (reply.size() == 1 && reply[0] >= '0' && reply[0] <= '2') {
            return reply[0] - '0';
        }
        return std::nullopt;
    }

    Position get_position() {
        Position pos;
        // :GRa#/:GDe# request OnStep's high-precision reply variants
        // (HH:MM:SS.SSSS# / sDD*MM:SS.SSSS#, 0.0001s / 0.001" resolution)
        // instead of the whole-second/whole-arcsecond :GR#/:GD# forms.
        // :GR#'s 1-second RA resolution (~15" at the equator) is too coarse
        // for ConformU's PulseGuide tolerance (0.07 s) on short (2-5s) guide
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

    AltitudeLimits get_altitude_limits() {
        auto horizon = parse_sexagesimal(send_command(":Gh#", true, 0));
        auto overhead = parse_sexagesimal(send_command(":Go#", true, 0));
        if (!horizon.has_value() || !overhead.has_value()) {
            throw AlpacaException("Invalid OnStep altitude limit response");
        }
        AltitudeLimits limits;
        limits.horizon_degrees = horizon.value();
        limits.overhead_degrees = overhead.value();
        return limits;
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

    bool park() {
        // Bare digit, no '#' (OnStepX numeric reply): '1' accepted, '0' refused.
        const std::string reply = send_command(":hP#", false, kSetAckTimeoutMs);
        return !reply.empty() && reply[0] == '1';
    }

    void set_park() { send_command_blind(":hQ#"); }

    void unpark() { send_command_blind(":hR#"); }

    void find_home() { send_command_blind(":hC#"); }

    std::optional<int> get_last_command_error() {
        std::string reply;
        try {
            reply = send_command(":GE#", true, kSetAckTimeoutMs);
        } catch (const AlpacaException&) {
            if (!connected_ || link_lost_) {
                throw;  // the link went: not an unknown code
            }
            return std::nullopt;
        }
        if (reply.size() != 2 || !std::isdigit(static_cast<unsigned char>(reply[0])) ||
            !std::isdigit(static_cast<unsigned char>(reply[1]))) {
            return std::nullopt;
        }
        return (reply[0] - '0') * 10 + (reply[1] - '0');
    }

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

        // SerialLink claims the port (a port another connected device holds
        // is refused before it is opened), takes TIOCEXCL so no other program
        // can open and retune it, and keeps the fd non-blocking: every read
        // goes through util::tty_read(), bounded by poll(), so a retune to
        // VMIN=1/VTIME=0 by another program cannot park a read and hang every
        // telescope call (#1202, TeSeek HM-17 after Park). HUPCL stays clear
        // (matches the probe path): an OnStep controller behind
        // CH340/CH341/native-USB-CDC resets its MCU on a DTR edge, so tearing
        // HUPCL down here would reset the board every time the driver
        // reconnects.
        //
        // The read slice is kSerialReadSliceMs (100ms), NOT the probe path's
        // 2s: read_response()'s and drain_stale()'s software timeout budgets
        // are only enforced in increments of it, so a long slice made every
        // non-acking blind command (e.g. pulse guide) and every
        // genuinely-silent response cost multiple real seconds instead of the
        // intended tens of milliseconds — confirmed against real hardware
        // (when this was VTIME), where this alone caused PulseGuide to block
        // ~2s and AbortSlew's multi-command stop sequence to cascade into a
        // timeout.
        std::string error;
        link_ = util::SerialLink::open(
            port_path, util::SerialClaimKind::Link, kSerialOwner,
            [speed](struct termios& tty) { configure_onstep_line(tty, speed); }, &error);
        if (!link_) {
            ALPACA_LOG_ERROR("OnStep", "Failed to open serial port [" + port_path + "]: " + error);
            last_connect_error_ = error;
            return false;
        }
        serial_fd_ = link_.fd();

        // Remember which node the configured path reached, so link_alive()
        // can tell when the adapter is gone or the path now points elsewhere.
        struct stat st {};
        if (::fstat(serial_fd_, &st) == 0) {
            std::lock_guard<std::mutex> id_lock(link_id_mutex_);
            link_path_ = port_path;
            link_dev_ = st.st_dev;
            link_ino_ = st.st_ino;
            ++link_generation_;
        }
        return true;
    }

    void disconnect_serial() {
        serial_fd_ = -1;
        link_.close();
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
            struct pollfd pfd {};
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
            return util::tty_write_all(serial_fd_, data.c_str(), data.length());
        }
        return util::send_all(socket_fd_, data.c_str(), data.length(), MSG_NOSIGNAL);
    }

    bool read_serial_char(char& ch) {
        // Bounded read (poll() on a non-blocking fd), held under mutex_ by
        // design — the mutex provides transaction atomicity for
        // command/response pairs.
        const ssize_t bytes_read = util::tty_read(serial_fd_, &ch, 1, kSerialReadSliceMs);
        if (bytes_read == 1) {
            return true;
        }
        if (bytes_read < 0) {
            // tty_read() reports a hung-up tty (read() returning 0) as EIO.
            const int err = errno;
            if (is_loss_errno(err)) {
                lose_link_locked("read failed: " + util::errno_string(err));
            }
            return false;
        }
        // 0 is a read-slice timeout; the node may be gone without the fd
        // reporting it yet.
        if (node_removed_locked()) {
            lose_link_locked("the adapter was removed");
        }
        return false;
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
                // Deliberately NOT a `continue` back to the top of the loop:
                // that would skip the elapsed/idle timeout checks below for
                // this iteration, and a sustained run of only \r/\n bytes
                // (line noise, a framing error, a firmware echo quirk) would
                // then busy-spin forever with no timeout ever firing --
                // hanging this call, and with it every other Alpaca call for
                // this device, since read_response() runs under mutex_.
                if (ch != '\r' && ch != '\n') {
                    response.push_back(ch);
                }
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
            // For a '#'-terminated reply, back off only when nothing arrived:
            // a pause after every character cost ~10 ms each (a 14-char :GRa#
            // reply took ~0.16 s at 9600 baud, where the bytes take ~15 ms).
            // An unterminated ack keeps it: its idle break needs this pause
            // plus one VTIME wait to pass 100 ms, or it waits a second cycle.
            if (!got_char || !require_hash_terminator) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        return response;
    }

    // ── Link-loss helpers (decision 0009, #1241) ──

    static bool is_loss_errno(int err) { return err == EIO || err == ENXIO || err == ENODEV || err == EBADF; }

    // Caller holds mutex_. True when the open fd's node is gone.
    bool node_removed_locked() const {
        if (connection_type_ != ConnectionType::Serial || serial_fd_ < 0) {
            return false;
        }
        struct stat st {};
        return ::fstat(serial_fd_, &st) != 0 || st.st_nlink == 0;
    }

    // Marks the link lost once: the first detection keeps its reason and stamp.
    void note_loss(const std::string& reason) {
        if (record_loss(reason)) {
            ALPACA_LOG_WARN("OnStep", "Serial link to the mount lost: " + reason);
        }
    }

    // note_loss() for a loss found with no mutex_ held: records it only while
    // the link it was found on is still the current one. The generation check
    // and the record share link_id_mutex_, which connect and disconnect take
    // to change the node, so neither can slip in between (open-astro#1289).
    // False when the link changed and nothing was recorded.
    bool note_loss_if_current(std::uint64_t generation, const std::string& reason) {
        bool recorded = false;
        {
            std::lock_guard<std::mutex> id_lock(link_id_mutex_);
            if (generation != link_generation_) {
                return false;
            }
            recorded = record_loss(reason);
        }
        if (recorded) {
            ALPACA_LOG_WARN("OnStep", "Serial link to the mount lost: " + reason);
        }
        return true;
    }

    // The state half of note_loss(): true when this call marked the link lost.
    bool record_loss(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(loss_mutex_);
            if (link_lost_) {
                return false;
            }
            lost_reason_ = reason;
            link_lost_ = true;
            const auto alive_ns = last_alive_ns_.load();
            if (alive_ns != 0) {  // stored + 1 ns, so 0 means never (see mark_alive())
                alive_before_loss_ = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(alive_ns - 1));
            }
        }
        (void)link_lost_at_.consume();  // an older, unclaimed loss is superseded by this one
        link_lost_at_.note(clock().now());
        return true;
    }

    // The last time the link was confirmed present (connect, a live
    // link_alive(), the check before an exchange). A loss happened after it,
    // so it bounds the outage from above even when nobody looked for a while.
    void mark_alive() {
        // Stored + 1 ns: 0 means "never", and a FakeTaskClock starts at the
        // epoch, where a real stamp would otherwise read as never.
        last_alive_ns_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock().now().time_since_epoch()).count() + 1;
    }

    util::TaskClock& clock() const { return *clock_.load(); }

    void clear_loss() {
        std::lock_guard<std::mutex> lock(loss_mutex_);
        link_lost_ = false;
        lost_reason_.clear();
    }

    // Caller holds mutex_. Closes the dead link and reports it as NotConnected.
    [[noreturn]] void lose_link_locked(const std::string& reason) {
        note_loss(reason);
        disconnect_locked();
        throw AlpacaException("OnStep link lost (" + reason + "); set Connected true to reconnect",
                              AlpacaError::NotConnected);
    }

    // Caller holds mutex_. Every exchange starts here.
    void require_live_link_locked() {
        if (connected_ && link_lost_) {
            disconnect_locked();  // a loss found by link_alive() while an exchange held the fd
        }
        if (!connected_) {
            const std::string reason = link_lost_reason();
            if (!reason.empty()) {
                throw AlpacaException("OnStep link lost (" + reason + "); set Connected true to reconnect",
                                      AlpacaError::NotConnected);
            }
            throw AlpacaException("Not connected to mount", AlpacaError::NotConnected);
        }
        if (node_removed_locked()) {
            lose_link_locked("the adapter was removed");
        }
        mark_alive();
    }

    // Caller holds mutex_; errno is from the failed write.
    [[noreturn]] void fail_write_locked() {
        const int err = errno;
        if (connection_type_ == ConnectionType::Serial) {
            if (is_loss_errno(err)) {
                lose_link_locked("write failed: " + util::errno_string(err));
            }
            if (node_removed_locked()) {
                lose_link_locked("the adapter was removed");
            }
        }
        throw AlpacaException("Failed to send command to mount");
    }

    std::atomic<util::TaskClock*> clock_{&util::default_task_clock()};
    mutable std::mutex mutex_;
    std::atomic<bool> connected_;
    std::atomic<bool> link_lost_{false};
    mutable std::mutex loss_mutex_;  // leaf: lost_reason_ and the link_lost_ transitions
    std::string lost_reason_;
    util::LinkLostStamp link_lost_at_;
    std::atomic<std::int64_t> last_alive_ns_{0};
    std::optional<std::chrono::steady_clock::time_point> alive_before_loss_;  // under loss_mutex_
    // The node recorded at connect. Taken before loss_mutex_ and the
    // link_lost_at_ stamp's mutex (note_loss_if_current), never after them.
    mutable std::mutex link_id_mutex_;
    std::string link_path_;
    dev_t link_dev_{};
    ino_t link_ino_{};
    std::uint64_t link_generation_ = 0;  // under link_id_mutex_: bumped when the node is recorded or cleared
    ConnectionType connection_type_;
    ConnectionInfo connection_info_;
    util::SerialLink link_;  // the connected serial port (#1202); closed while disconnected
    int serial_fd_;          // link_.fd() while the serial link is open, else -1
    int socket_fd_;
    std::string last_connect_error_;  // operator-facing reason the last connect() failed
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

void OnStepProtocolWrapper::set_task_clock(util::TaskClock& clock) { pimpl_->set_task_clock(clock); }

void OnStepProtocolWrapper::release_task_clock(util::TaskClock& clock) { pimpl_->release_task_clock(clock); }

bool OnStepProtocolWrapper::link_alive() { return pimpl_->link_alive(); }

std::string OnStepProtocolWrapper::link_lost_reason() const { return pimpl_->link_lost_reason(); }

std::optional<std::chrono::steady_clock::time_point> OnStepProtocolWrapper::consume_link_lost_at() {
    return pimpl_->consume_link_lost_at();
}

std::optional<std::chrono::steady_clock::time_point> OnStepProtocolWrapper::consume_last_alive_before_loss() {
    return pimpl_->consume_last_alive_before_loss();
}

std::string OnStepProtocolWrapper::last_connect_error() const { return pimpl_->last_connect_error(); }

std::string OnStepProtocolWrapper::get_product_name() { return pimpl_->get_product_name(); }

std::string OnStepProtocolWrapper::get_version_number() { return pimpl_->get_version_number(); }

std::optional<int> OnStepProtocolWrapper::get_mount_coords_mode() { return pimpl_->get_mount_coords_mode(); }

Position OnStepProtocolWrapper::get_position() { return pimpl_->get_position(); }

AltAz OnStepProtocolWrapper::get_alt_az() { return pimpl_->get_alt_az(); }

AltitudeLimits OnStepProtocolWrapper::get_altitude_limits() { return pimpl_->get_altitude_limits(); }

MountStatus OnStepProtocolWrapper::get_status() { return pimpl_->get_status(); }

void OnStepProtocolWrapper::set_target_ra(double ra_hours) { pimpl_->set_target_ra(ra_hours); }

void OnStepProtocolWrapper::set_target_dec(double dec_degrees) { pimpl_->set_target_dec(dec_degrees); }

bool OnStepProtocolWrapper::slew_to_target() { return pimpl_->slew_to_target(); }

void OnStepProtocolWrapper::sync_to_target() { pimpl_->sync_to_target(); }

void OnStepProtocolWrapper::abort_slew() { pimpl_->abort_slew(); }

void OnStepProtocolWrapper::start_tracking() { pimpl_->start_tracking(); }

void OnStepProtocolWrapper::stop_tracking() { pimpl_->stop_tracking(); }

bool OnStepProtocolWrapper::park() { return pimpl_->park(); }

void OnStepProtocolWrapper::set_park() { pimpl_->set_park(); }

void OnStepProtocolWrapper::unpark() { pimpl_->unpark(); }

void OnStepProtocolWrapper::find_home() { pimpl_->find_home(); }

std::optional<int> OnStepProtocolWrapper::get_last_command_error() { return pimpl_->get_last_command_error(); }

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

namespace detail {
void set_link_loss_hook(std::function<void(LinkLossHookPoint)> hook) {
    std::lock_guard<std::mutex> lock(link_loss_hook_mutex());
    link_loss_hook() = std::move(hook);
}
}  // namespace detail

}  // namespace alpacacore::vendor::onstep

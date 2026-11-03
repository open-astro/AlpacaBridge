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
#include <alpacacore/vendor/ioptron/ioptron_ifr_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/serial_read.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::ioptron {

namespace {

constexpr int kMaxResponseLen = 32;
constexpr int kHandshakeRetries = 3;
constexpr int kHandshakeReadTimeoutMs = 2000;
// Status replies take 5-20 ms on the rig (:RI# is the slow one); a dead unit costs a getter this long.
constexpr int kReplyTimeoutMs = 1500;
// The one-byte ack of :RM came ~60 ms after the command on the rig, later than a 50 ms window: left unread it
// prefixes the next status reply ("1+023217...") and breaks the fixed-width parse, as on the iEAF.
constexpr int kAckTimeoutMs = 1000;
// 0.01 arcsec per unit: the :RI# position and offset fields and the :RM/:RY argument.
constexpr double kRotatorUnitsPerDegree = 360000.0;
constexpr long kRotatorUnitsPerTurn = 360L * 360000L;

// One owner name for the probe and the link (decision 0017 rule 4). The focuser and the rotator are one kind of
// device to the registry: the second driver's scan counts the first one's port as its sibling's.
constexpr const char* kSerialOwner = "iOptron iFR";

#ifndef _WIN32
void configure_ifr_line(struct termios& tty) { util::make_raw_8n1(tty, B115200); }
#endif

// A fixed-width decimal field with an optional leading sign, every other character a digit. The firmware pads
// with zeros after the sign ("+0500000..."); a short reply or a stray byte fails the parse instead of shifting
// the fields after it.
std::optional<long> fixed_field(std::string_view text, std::size_t pos, std::size_t len) {
    if (pos + len > text.size() || len == 0) return std::nullopt;
    std::string_view f = text.substr(pos, len);
    bool negative = false;
    if (f.front() == '+' || f.front() == '-') {
        negative = f.front() == '-';
        f.remove_prefix(1);
    }
    if (f.empty()) return std::nullopt;
    long value = 0;
    for (const char c : f) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return negative ? -value : value;
}

std::string strip_hash(std::string reply) {
    if (!reply.empty() && reply.back() == '#') reply.pop_back();
    return reply;
}

bool parse_device_info(const std::string& resp, IfrDeviceInfo& out) {
    // "%6d%2d%4d": position field, model code, build. The position field reads 000000 on the iFR whatever the
    // focuser position is, so only the model and build are kept.
    const std::string r = strip_hash(resp);
    if (r.size() != 12) return false;
    const auto model = fixed_field(r, 6, 2);
    const auto build = fixed_field(r, 8, 4);
    if (!model || !build || !fixed_field(r, 0, 6)) return false;
    out.model = static_cast<std::int32_t>(*model);
    out.build = static_cast<std::int32_t>(*build);
    return true;
}

double rotator_degrees(long units) { return static_cast<double>(units) / kRotatorUnitsPerDegree; }

long rotator_units(double degrees) {
    long units = std::lround(degrees * kRotatorUnitsPerDegree);
    if (units < 0) units = 0;
    if (units >= kRotatorUnitsPerTurn) units = kRotatorUnitsPerTurn - 1;
    return units;
}

}  // namespace

IfrModel ifr_model_for(std::string_view model) {
    // Travel from the iFR product page (25 mm, 25 mm, 26 mm) at the 0.4 um step the page gives. The 3" was
    // measured on the rig: 62,822 steps between its limit stops, so 62,500 is reachable at both ends.
    if (model == "ifr2") return {20, "iFR 2\"", 62500};
    if (model == "ifr4") return {40, "iFR 4\"", 65000};
    return {30, "iFR 3\"", 62500};
}

bool is_ifr_model(std::int32_t model) {
    // INDI iafscaa.cpp: 02 iEAF, 03 iAFS, 04 iAFS without CAA, 05 iAFS + CAA. The iFR 3" answers 05 (rig,
    // 2026-11-03). Model 4 has no rotator; this driver needs both halves.
    return model == 5;
}

bool probe_ifr_port(const std::string& port_path, IfrDeviceInfo& info) {
#ifndef _WIN32
    // The probe claim waits out another scan's probe of this port (the mount, iEAF and iEFW scans match the same
    // Prolific adapters) and is held until this function returns.
    std::string error;
    util::SerialLink link =
        util::SerialLink::open(port_path, util::SerialClaimKind::Probe, kSerialOwner, configure_ifr_line, &error);
    if (!link) {
        ALPACA_LOG_DEBUG("iOptron", "iFR probe skipped " + port_path + ": " + error);
        return false;
    }
    const int fd = link.fd();
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 1000));
        tcflush(fd, TCIOFLUSH);
        const char* cmd = ":DeviceInfo#";
        if (!util::tty_write_all(fd, cmd, std::strlen(cmd))) continue;
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Terminator;
        spec.terminators = "#";
        spec.max_len = kMaxResponseLen;
        auto reply = util::read_until(
            fd, spec, std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeReadTimeoutMs));
        IfrDeviceInfo parsed;
        if (parse_device_info(reply.bytes, parsed) && is_ifr_model(parsed.model)) {
            info = parsed;
            return true;
        }
    }
#else
    (void)port_path;
    (void)info;
#endif
    return false;
}

#ifndef _WIN32
namespace {
// The raw /dev/ttyUSBn fallback has no by-id name to filter on; keep it to the Prolific PL2303-class bridge the
// iFR carries (067b:23a3, like the iAFS2/3), so the scan does not open every serial device on the box.
bool raw_port_looks_like_ifr_candidate(const std::string& port_path) {
    auto descriptor = util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return util::usb_tty_descriptor_matches(*descriptor, {"067b", "Prolific", "PL2303", "USB-Serial", "USB_Serial"});
}
}  // namespace
#endif

std::vector<IfrPortInfo> enumerate_ifr_ports() {
    std::vector<IfrPortInfo> results;
#ifndef _WIN32
    std::set<std::string> seen;
    struct Candidate {
        std::string path;
        std::string name;
        bool sibling = false;  // a connected iFR session holds it: count it, never open it
    };
    std::vector<Candidate> candidates;
    auto add_candidate = [&candidates](const std::string& path, const std::string& name) {
        switch (util::serial_candidate_use(path, kSerialOwner)) {
            case util::SerialCandidateUse::HeldByOther:
                ALPACA_LOG_DEBUG("iOptron", "iFR scan skipped " + path + ": another connected device holds it");
                return;
            case util::SerialCandidateUse::HeldBySibling:
                candidates.push_back({path, name, true});
                return;
            case util::SerialCandidateUse::Free:
                candidates.push_back({path, name, false});
                return;
        }
    };

    // The iFR's two Type-C ports are two Prolific chips with ONE serial number, so udev gives both the same
    // by-id name and only one symlink survives. The raw-node pass below finds the other; the dedupe set keeps
    // a port from being probed twice.
    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (util::path_exists(serial_by_id)) {
        for (const auto& sym : util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            const bool is_candidate =
                name.find("Prolific") != std::string::npos || name.find("PL2303") != std::string::npos ||
                name.find("067b") != std::string::npos || name.find("USB-Serial") != std::string::npos ||
                name.find("USB_Serial") != std::string::npos;
            if (!is_candidate) continue;
            std::error_code ec;
            const std::string resolved = std::filesystem::canonical(sym.path, ec).string();
            if (ec || !seen.insert(resolved).second) continue;
            add_candidate(resolved, name);
        }
    }
    for (int i = 0; i < 10; ++i) {
        const std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!util::path_exists(port)) continue;
        std::error_code ec;
        const std::string resolved = std::filesystem::canonical(port, ec).string();
        if (ec || seen.count(resolved) != 0) continue;
        if (!raw_port_looks_like_ifr_candidate(resolved)) continue;
        seen.insert(resolved);
        add_candidate(resolved, "");
    }
    // Candidate order is the auto-detect index order: by-id first, then raw nodes, each in listing order.

    // Probe concurrently (issue #218): a port that is not an iFR costs the whole handshake timeout.
    std::vector<IfrDeviceInfo> infos(candidates.size());
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].sibling) continue;
        ALPACA_LOG_INFO("iOptron", "Probing " + candidates[i].path + " for an iFR...");
        workers.emplace_back([&, i] { found[i] = probe_ifr_port(candidates[i].path, infos[i]) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.sibling) {
            ALPACA_LOG_INFO("iOptron", "Counted " + c.path + " without probing: a connected iFR holds it");
            results.push_back({c.path, c.name, IfrDeviceInfo{}});
            continue;
        }
        if (!found[i]) continue;
        ALPACA_LOG_INFO("iOptron", "Found iFR on " + c.path + " (model " + std::to_string(infos[i].model) + ")");
        results.push_back({c.path, c.name, infos[i]});
    }
#endif
    return results;
}

class IfrLink::Impl {
public:
    ~Impl() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        close_locked();
    }

    // Open, handshake and read the firmware. Any throw leaves the port closed (the Impl destructor runs as the
    // half-built IfrLink unwinds, and close_locked() is idempotent).
    IfrDeviceInfo open(const std::string& port) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        port_ = port;
#ifndef _WIN32
        std::string error;
        link_ = util::SerialLink::open(port, util::SerialClaimKind::Link, kSerialOwner, configure_ifr_line, &error);
        if (!link_) throw AlpacaException(error, AlpacaError::NotConnected);
        tcflush(link_.fd(), TCIOFLUSH);
#else
        throw AlpacaException("iOptron iFR driver is Linux-only", AlpacaError::DriverException);
#endif
        IfrDeviceInfo info;
        bool answered = false;
        for (int attempt = 0; attempt < kHandshakeRetries && !answered; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 1000));
            try {
                answered = parse_device_info(query_locked(":DeviceInfo#", kHandshakeReadTimeoutMs), info);
            } catch (const AlpacaException& e) {
                if (!link_open_locked()) throw;  // the port went away during the handshake
                ALPACA_LOG_WARN("iOptron",
                                "iFR handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }
        if (!answered) {
            throw AlpacaException("No iOptron iFR answered on " + port + " (no :DeviceInfo# reply after " +
                                      std::to_string(kHandshakeRetries) + " attempts)",
                                  AlpacaError::NotConnected);
        }
        if (!is_ifr_model(info.model)) {
            throw AlpacaException("The device on " + port + " reported model code " + std::to_string(info.model) +
                                      ", not an iOptron iFR (expected 5). An iEAF or iAFS focuser is configured "
                                      "under the iOptron vendor instead.",
                                  AlpacaError::NotConnected);
        }
        // Firmware for the web UI. A unit that does not answer :FW1# still works; a hangup does not.
        try {
            const std::string fw = strip_hash(query_locked(":FW1#", kReplyTimeoutMs));
            if (fw.size() >= 12) {
                info.focuser_firmware = fw.substr(0, 6);
                info.rotator_firmware = fw.substr(6, 6);
            }
        } catch (const AlpacaException& e) {
            if (!link_open_locked()) throw;
            ALPACA_LOG_WARN("iOptron", std::string("iFR firmware read failed: ") + e.what());
        }
        opened_ = true;
        ALPACA_LOG_INFO("iOptron", "iFR link open on " + port + " (model " + std::to_string(info.model) +
                                       ", focuser firmware " + info.focuser_firmware + ", rotator firmware " +
                                       info.rotator_firmware + ")");
        return info;
    }

    bool link_alive() const noexcept { return !lost_.load(); }

    std::string link_fault() const {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        return fault_;
    }

    bool lose_link_if(const std::function<bool()>& stale, const std::string& reason) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (lost_.load()) return true;
        if (!opened_ || !stale()) return false;
        ALPACA_LOG_ERROR("iOptron", "iFR link lost on " + port_ + ": " + reason + " (port released)");
        set_fault(reason);
        lost_.store(true);
        close_locked();
        return true;
    }

    IfrFocuserStatus focuser_status() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_open_locked();
        const std::string r = strip_hash(query_locked(":FI#", kReplyTimeoutMs));
        const auto pos = fixed_field(r, 0, 7);
        const auto moving = fixed_field(r, 7, 1);
        const auto temp = fixed_field(r, 8, 5);
        const auto dir = fixed_field(r, 13, 1);
        if (r.size() != 14 || !pos || !moving || !temp || !dir) {
            throw AlpacaException("Failed to parse iFR focuser status: " + r, AlpacaError::DriverException);
        }
        IfrFocuserStatus s;
        s.position = static_cast<std::int32_t>(*pos);
        s.moving = *moving != 0;
        s.temperature_c = static_cast<double>(*temp) / 100.0 - 273.15;
        s.direction = static_cast<int>(*dir);
        return s;
    }

    IfrRotatorStatus rotator_status() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_open_locked();
        const std::string r = strip_hash(query_locked(":RI#", kReplyTimeoutMs));
        const auto offset = fixed_field(r, 0, 10);
        const auto moving = fixed_field(r, 10, 1);
        const auto pos = fixed_field(r, 11, 9);
        const auto dir = fixed_field(r, 20, 1);
        const auto size = fixed_field(r, 21, 2);
        if (r.size() != 23 || !offset || !moving || !pos || !dir || !size) {
            throw AlpacaException("Failed to parse iFR rotator status: " + r, AlpacaError::DriverException);
        }
        IfrRotatorStatus s;
        s.offset_deg = rotator_degrees(*offset);
        s.moving = *moving != 0;
        s.position_deg = rotator_degrees(*pos);
        s.reversed = *dir == 0;
        s.size_tenths = static_cast<int>(*size);
        return s;
    }

    IfrAck command(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        ensure_open_locked();
        return ack_locked(cmd);
    }

private:
    bool link_open_locked() const {
#ifndef _WIN32
        return static_cast<bool>(link_);
#else
        return false;
#endif
    }

    void ensure_open_locked() const {
        if (lost_.load() || !link_open_locked()) {
            throw AlpacaException("iFR link lost; Connect again once the unit is back", AlpacaError::NotConnected);
        }
    }

    void close_locked() {
#ifndef _WIN32
        link_.close();  // releases TIOCEXCL and the registry claim; a no-op once closed
#endif
    }

    void set_fault(const std::string& text) {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        fault_ = text;
    }

    // The port is gone: decision 0009 rule 2 loses the link at once, for both devices. Release the port so a
    // replugged unit can be reopened by a fresh session, keep the reason, fail with NotConnected. During open()
    // nothing is latched: the session is never published and its port closes as it unwinds.
    [[noreturn]] void fail_link_locked(const std::string& reason) {
        if (opened_ && !lost_.load()) {
            ALPACA_LOG_ERROR("iOptron", "iFR link lost on " + port_ + ": " + reason + " (port released)");
            set_fault(reason);
            lost_.store(true);
        }
        close_locked();
        throw AlpacaException(reason, AlpacaError::NotConnected);
    }

    void write_locked(const std::string& data) {
#ifndef _WIN32
        if (!util::tty_write_all(link_.fd(), data.c_str(), data.size())) {
            const int err = errno;
            if (err == EIO || err == ENXIO || err == ENODEV) {
                fail_link_locked("iFR serial link lost (errno " + std::to_string(err) + ")");
            }
            throw AlpacaException("iFR write failed: " + util::errno_string(err), AlpacaError::DriverException);
        }
#else
        (void)data;
#endif
    }

    // Flush stale input, send, read to the '#'. The reply keeps its '#'.
    std::string query_locked(const std::string& cmd, int timeout_ms) {
        ALPACA_LOG_TRACE("iOptron", "iFR command: " + cmd);
#ifndef _WIN32
        tcflush(link_.fd(), TCIFLUSH);
        write_locked(cmd);
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Terminator;
        spec.terminators = "#";
        spec.max_len = kMaxResponseLen;
        const auto r = util::read_until(link_.fd(), spec,
                                        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
        switch (r.outcome) {
            case util::ReadOutcome::Terminator:
                ALPACA_LOG_TRACE("iOptron", "iFR response: " + r.bytes);
                return r.bytes;
            case util::ReadOutcome::Overflow:
                throw AlpacaException("iFR response too long to " + cmd, AlpacaError::DriverException);
            case util::ReadOutcome::LinkDead:
                fail_link_locked("iFR serial link lost (errno " + std::to_string(r.err) + ")");
            default:
                throw AlpacaException("iFR did not answer " + cmd, AlpacaError::DriverException);
        }
#else
        (void)cmd;
        (void)timeout_ms;
        throw AlpacaException("iFR did not answer", AlpacaError::DriverException);
#endif
    }

    // Motion and setting commands answer one byte and no '#': '1' accepted, '0' refused. Always consume it, or
    // it prefixes the next status reply. A hangup while waiting is a lost link, not silence.
    IfrAck ack_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iFR command (ack): " + cmd);
#ifndef _WIN32
        tcflush(link_.fd(), TCIFLUSH);
        write_locked(cmd);
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Length;
        spec.length = 1;
        const auto r = util::read_until(link_.fd(), spec,
                                        std::chrono::steady_clock::now() + std::chrono::milliseconds(kAckTimeoutMs));
        if (r.outcome == util::ReadOutcome::Length) {
            if (r.bytes == "1") return IfrAck::Accepted;
            if (r.bytes == "0") return IfrAck::Refused;
            ALPACA_LOG_WARN("iOptron", "iFR unexpected ack byte to " + cmd + ": " + r.bytes);
            return IfrAck::Silent;
        }
        if (r.outcome == util::ReadOutcome::LinkDead) {
            fail_link_locked("iFR serial link lost (errno " + std::to_string(r.err) + ")");
        }
#else
        (void)cmd;
#endif
        ALPACA_LOG_DEBUG("iOptron", "iFR no ack for " + cmd);
        return IfrAck::Silent;
    }

    // The per-link I/O lock: every exchange, the open and the close run under it.
    std::mutex io_mutex_;
    std::string port_;
    bool opened_ = false;  // guarded by io_mutex_; true once open() has succeeded
    std::atomic<bool> lost_{false};
    mutable std::mutex fault_mutex_;  // leaf: the loss reason only, never held across an exchange
    std::string fault_;
#ifndef _WIN32
    util::SerialLink link_;
#endif
};

namespace {

// One live session per canonical port path. The map holds weak pointers: the drivers own the sessions, and a
// session closes its port when the last of them lets go.
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, std::weak_ptr<IfrLink>>& registry() {
    static std::map<std::string, std::weak_ptr<IfrLink>> sessions;
    return sessions;
}

}  // namespace

std::shared_ptr<IfrLink> IfrLink::acquire(const IfrConnectionConfig& config) {
    if (config.serial_port.empty()) {
        throw AlpacaException(
            "iOptron iFR: no serial port to open. Set portPath, or use connectionType 'auto' to scan.",
            AlpacaError::NotConnected);
    }
    std::error_code ec;
    std::string key = std::filesystem::canonical(config.serial_port, ec).string();
    if (ec) key = config.serial_port;  // a missing node: the open below reports it

    // Held across the open, so a focuser and a rotator connecting at the same moment open the port once and
    // the second joins. The open takes the serial-port registry's own lock inside; nothing takes this one
    // under that, so the order is one way only.
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& slot = registry()[key];
    if (auto existing = slot.lock(); existing && existing->link_alive()) {
        return existing;
    }
    IfrConnectionConfig resolved = config;
    resolved.serial_port = key;
    auto session = std::make_shared<IfrLink>(OpenTag{}, resolved);
    slot = session;
    return session;
}

IfrLink::IfrLink(OpenTag, const IfrConnectionConfig& config)
    : impl_(std::make_unique<Impl>()), port_(config.serial_port), info_(impl_->open(config.serial_port)) {}

IfrLink::~IfrLink() = default;

bool IfrLink::link_alive() const noexcept { return impl_->link_alive(); }

std::string IfrLink::link_fault() const { return impl_->link_fault(); }

bool IfrLink::lose_link_if(const std::function<bool()>& stale, const std::string& reason) {
    return impl_->lose_link_if(stale, reason);
}

IfrFocuserStatus IfrLink::focuser_status() { return impl_->focuser_status(); }

IfrAck IfrLink::focuser_move(std::int32_t position) {
    char cmd[24];
    std::snprintf(cmd, sizeof(cmd), ":FM%07u#", static_cast<unsigned>(position < 0 ? 0 : position));
    return impl_->command(cmd);
}

IfrAck IfrLink::focuser_halt() { return impl_->command(":FQ#"); }

IfrRotatorStatus IfrLink::rotator_status() { return impl_->rotator_status(); }

IfrAck IfrLink::rotator_move(double position_deg) {
    char cmd[24];
    std::snprintf(cmd, sizeof(cmd), ":RM%09ld#", rotator_units(position_deg));
    return impl_->command(cmd);
}

IfrAck IfrLink::rotator_halt() { return impl_->command(":RQ#"); }

IfrAck IfrLink::rotator_sync(double position_deg) {
    char cmd[24];
    std::snprintf(cmd, sizeof(cmd), ":RY%09ld#", rotator_units(position_deg));
    return impl_->command(cmd);
}

IfrAck IfrLink::rotator_set_reverse(bool reversed) { return impl_->command(reversed ? ":RR0#" : ":RR1#"); }

void reset_ifr_link_registry_for_testing() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry().clear();
}

}  // namespace alpacacore::vendor::ioptron

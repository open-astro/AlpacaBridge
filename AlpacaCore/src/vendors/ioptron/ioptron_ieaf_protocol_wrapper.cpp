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
#include <alpacacore/vendor/ioptron/ioptron_ieaf_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
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

constexpr int COMMAND_DELAY_MS =
    5;  // read_response() waits for the # terminator; no need to pre-sleep a full reply time
constexpr int MAX_RESPONSE_LEN = 32;
constexpr int HANDSHAKE_RETRIES = 3;
constexpr int HANDSHAKE_READ_TIMEOUT_S = 2;        // Shorter than the 4s runtime timeout (INDI iEAFFOCUS_TIMEOUT)
constexpr std::int32_t IEAF_MAX_POSITION = 99999;  // matches INDI FocusAbsPos max
// Old VMIN=0/VTIME=10 per-character window, now the tty_read() bound.
constexpr int READ_CHAR_TIMEOUT_MS = 1000;

// Names this wrapper's probe and its connected link in the serial-port
// registry (#1202): the iEAF and the iAFS2/3 are one kind of device to it.
constexpr const char* kSerialOwner = "iOptron focuser";

#ifndef _WIN32
// The iEAF runs at a fixed 115200 8N1 raw, whatever the config's baud says.
void configure_ieaf_line(struct termios& tty) { util::make_raw_8n1(tty, B115200); }
#endif

bool parse_device_info(const std::string& resp, IeafDeviceInfo& out) {
    // ":DeviceInfo#" → "%6d%2d%4d#": position, model code, firmware/build.
    int pos = 0, model = 0, fw = 0;
    if (std::sscanf(resp.c_str(), "%6d%2d%4d", &pos, &model, &fw) != 3) {
        return false;
    }
    out.position = pos;
    out.model = model;
    out.firmware = fw;
    return true;
}

}  // namespace

bool is_ieaf_model(std::int32_t model) {
    // Model code 2 = iEAF, 3 = iAFS2/3 (INDI ieaffocus.cpp, INDIGO
    // indigo_focuser_ioptron.c). Same protocol either way.
    return model == 2 || model == 3;
}

// Probe a serial port with the :DeviceInfo# handshake. Returns true (and
// fills `info`) only if the reply parses and reports an iEAF model code.
//
// The iEAF ships with a Prolific PL2303 USB-serial bridge (067b:23d3; the
// iAFS2/3 uses 067b:23a3, "ATEN Serial Bridge") wired
// straight to the focuser MCU — no Arduino-style DTR reset on open, so no
// CH340 HUPCL dance is needed here.
bool probe_ieaf_port(const std::string& port_path, IeafDeviceInfo& info) {
#ifndef _WIN32
    // The probe claim waits out another scan's probe of this port (the mount
    // and the iEFW scans match the same Prolific adapters) and is held until
    // this function returns.
    std::string error;
    util::SerialLink link =
        util::SerialLink::open(port_path, util::SerialClaimKind::Probe, kSerialOwner, configure_ieaf_line, &error);
    if (!link) {
        ALPACA_LOG_DEBUG("iOptron", "iEAF probe skipped " + port_path + ": " + error);
        return false;
    }
    const int fd = link.fd();

    for (int attempt = 0; attempt < 2; ++attempt) {
        // First attempt is quick; second waits out any port-open settling.
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 1000));
        tcflush(fd, TCIOFLUSH);

        const char* cmd = ":DeviceInfo#";
        if (!util::tty_write_all(fd, cmd, std::strlen(cmd))) {
            continue;
        }

        // The '#' ends the reply and is not part of it; a reply of
        // MAX_RESPONSE_LEN kept bytes without one is Overflow and is parsed as is.
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Terminator;
        spec.terminators = "#";
        spec.max_len = MAX_RESPONSE_LEN;
        auto reply = util::read_until(
            fd, spec, std::chrono::steady_clock::now() + std::chrono::seconds(HANDSHAKE_READ_TIMEOUT_S));
        std::string resp = std::move(reply.bytes);
        if (reply.outcome == util::ReadOutcome::Terminator) resp.pop_back();

        IeafDeviceInfo parsed;
        if (parse_device_info(resp, parsed) && is_ieaf_model(parsed.model)) {
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
// The raw /dev/ttyUSBn fallback has no by-id symlink name to filter on;
// restrict it to Prolific PL2303-class adapters (the bridge the iEAF ships
// with) so the scan doesn't open every unrelated serial device on the box.
// Note the iOptron mount USB port also enumerates as a Prolific adapter — the
// :DeviceInfo# probe's model-code check is what tells them apart.
bool raw_port_looks_like_ieaf_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor,
                                                        {"067b", "Prolific", "PL2303", "USB-Serial", "USB_Serial"});
}
}  // namespace
#endif

std::vector<IeafPortInfo> enumerate_ieaf_ports() {
    std::vector<IeafPortInfo> results;

#ifndef _WIN32
    // Canonical paths already probed via by-id, so the raw-node pass below
    // never re-opens a port the by-id pass already tried.
    std::set<std::string> probed;

    struct Candidate {
        std::string path;
        std::string name;  // by-id symlink name, empty for raw nodes
        bool sibling = false;  // a connected iOptron focuser holds it: count it, never open it
    };
    std::vector<Candidate> candidates;
    // A port another connected device holds is never opened. One a connected
    // focuser holds keeps its place in the list (no probe, identity unknown),
    // so "focuser index 1" stays the same unit while index 0 is connected; the
    // driver that picks it is then refused by its link claim, which names the holder.
    auto add_candidate = [&candidates](const std::string& path, const std::string& name) {
        switch (util::serial_candidate_use(path, kSerialOwner)) {
            case util::SerialCandidateUse::HeldByOther:
                ALPACA_LOG_DEBUG("iOptron", "iEAF scan skipped " + path + ": another connected device holds it");
                return;
            case util::SerialCandidateUse::HeldBySibling:
                candidates.push_back({path, name, true});
                return;
            case util::SerialCandidateUse::Free:
                candidates.push_back({path, name, false});
                return;
        }
    };

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;

            // Prolific PL2303 USB-serial bridge (067b:23d3 iEAF, 067b:23a3
            // iAFS2/3). Match on the 067b vendor ID / names, never a product ID.
            bool is_candidate =
                (name.find("Prolific") != std::string::npos) || (name.find("PL2303") != std::string::npos) ||
                (name.find("067b") != std::string::npos) || (name.find("USB-Serial") != std::string::npos) ||
                (name.find("USB_Serial") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            add_candidate(resolved, name);
        }
    }

    // Always also probe raw /dev/ttyUSB* nodes: generic Prolific adapters with
    // no per-device serial number collide in udev's by-id naming, so a second
    // adapter can be silently absent from by-id even when the directory exists.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;

        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_ieaf_candidate(resolved)) continue;
        probed.insert(resolved);
        add_candidate(resolved, "");
    }

    // Probe every candidate concurrently. A non-responsive port (an iOptron
    // mount shares the same Prolific chip class) costs the full handshake
    // timeout, so serial probing scaled linearly with adapter count (issue
    // #218); one thread per port bounds the scan to a single port's worst case.
    std::vector<IeafDeviceInfo> infos(candidates.size());
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.sibling) continue;
        ALPACA_LOG_INFO("iOptron", "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") + " for iEAF...");
        workers.emplace_back([&, i] { found[i] = probe_ieaf_port(candidates[i].path, infos[i]) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.sibling) {
            ALPACA_LOG_INFO("iOptron", "Counted " + c.path + " without probing: a connected iOptron focuser holds it");
            results.push_back({c.path, c.name, IeafDeviceInfo{}});
            continue;
        }
        if (!found[i]) continue;
        const auto& info = infos[i];
        ALPACA_LOG_INFO("iOptron", "Found iEAF on " + c.path + " (model " + std::to_string(info.model) + ", firmware " +
                                       std::to_string(info.firmware) + ")");
        results.push_back({c.path, c.name, info});
    }
#endif

    return results;
}

class IeafProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    IeafDeviceInfo connect(const IeafConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        // A reconnect releases this wrapper's own port first: its link claim
        // would otherwise refuse the new open as held by "the connected iOptron focuser".
        disconnect_locked();
        config_ = config;

        connect_serial();

        // Handshake with retries, using a shorter read timeout so the total
        // connect time stays well under ASCOM clients' ~10s budget.
        IeafDeviceInfo info;
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
                                "iEAF handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }

        config_.serial_timeout_s = saved_timeout;

        if (!success) {
            disconnect_locked();
            throw AlpacaException("iEAF handshake failed after " + std::to_string(HANDSHAKE_RETRIES) + " attempts",
                                  AlpacaError::NotConnected);
        }
        if (!is_ieaf_model(info.model)) {
            disconnect_locked();
            throw AlpacaException("Device on " + config_.serial_port + " reported model code " +
                                      std::to_string(info.model) + ", not an iEAF (expected 2 or 3)",
                                  AlpacaError::NotConnected);
        }

        connected_ = true;
        link_lost_.store(false);
        set_fault("");
        ALPACA_LOG_INFO("iOptron", "iEAF connected (model " + std::to_string(info.model) + ", firmware " +
                                       std::to_string(info.firmware) + ")");
        return info;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
        set_fault("");
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool link_alive() const noexcept { return !link_lost_.load(); }

    std::string link_fault() const {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        return fault_;
    }

    IeafStatus get_status() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        std::string resp = send_command_locked(":FI#");
        // "%7d%1d%5d%1d#": position, moving flag, temperature (Kelvin x 100),
        // direction flag (0 = reversed).
        int pos = 0, moving = 0, temp = 0, dir = 0;
        if (std::sscanf(resp.c_str(), "%7d%1d%5d%1d", &pos, &moving, &temp, &dir) != 4) {
            throw AlpacaException("Failed to parse iEAF status: " + resp, AlpacaError::DriverException);
        }
        IeafStatus status;
        status.position = pos;
        status.moving = moving != 0;
        status.temperature_c = temp / 100.0 - 273.15;
        status.reversed = dir == 0;
        return status;
    }

    void move_to(std::int32_t position) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (position < 0 || position > IEAF_MAX_POSITION) {
            throw AlpacaException("iEAF target position out of range", AlpacaError::InvalidValue);
        }
        char cmd[16];
        // Space-padded 7-wide, matching INDI's ":FM%7u#".
        std::snprintf(cmd, sizeof(cmd), ":FM%7u#", static_cast<unsigned>(position));
        send_command_blind_locked(cmd);
    }

    void halt() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":FQ#");
    }

    void set_zero() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        send_command_blind_locked(":FZ#");
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("iEAF not connected", AlpacaError::NotConnected);
        }
    }

    void connect_serial() {
#ifndef _WIN32
        // SerialLink claims the port (a port another connected device holds is
        // refused, naming it), takes TIOCEXCL and keeps the fd non-blocking.
        std::string error;
        link_ = util::SerialLink::open(config_.serial_port, util::SerialClaimKind::Link, kSerialOwner,
                                       configure_ieaf_line, &error);
        if (!link_) {
            throw AlpacaException(error, AlpacaError::NotConnected);
        }

        tcflush(link_.fd(), TCIOFLUSH);
#else
        throw AlpacaException("iEAF driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    void disconnect_locked() {
        connected_ = false;
#ifndef _WIN32
        link_.close();  // releases TIOCEXCL and the registry claim
#endif
    }

    void set_fault(const std::string& text) {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        fault_ = text;
    }

    // The port is gone (a hangup, or EIO/ENXIO/ENODEV on the fd; EBADF is not one, a write only meets it on an fd we
    // closed): decision 0009 rule 2 loses the link at once. Release the port so a replugged focuser can be reopened,
    // latch link_lost_ for the driver's Connected getter and fail with NotConnected. During connect() nothing is
    // latched: the handshake retries and connect() closes the port itself.
    [[noreturn]] void fail_link_locked(const std::string& reason) {
        if (connected_) {
            ALPACA_LOG_ERROR("iOptron",
                             "iEAF link lost on " + config_.serial_port + ": " + reason + " (port released)");
            set_fault(reason);
            link_lost_.store(true);
            disconnect_locked();
        }
        throw AlpacaException(reason, AlpacaError::NotConnected);
    }

    void tcflush_port() {
#ifndef _WIN32
        if (link_) {
            tcflush(link_.fd(), TCIOFLUSH);
        }
#endif
    }

    void write_data(const std::string& data) {
#ifndef _WIN32
        if (!util::tty_write_all(link_.fd(), data.c_str(), data.length())) {
            const int err = errno;
            if (err == EIO || err == ENXIO || err == ENODEV) {
                fail_link_locked("iEAF serial link lost (errno " + std::to_string(err) + ")");
            }
            throw AlpacaException("Write failed: " + util::errno_string(err), AlpacaError::DriverException);
        }
#else
        (void)data;
#endif
    }

    std::string read_response() {
        std::string response;
#ifndef _WIN32
        util::ReadSpec spec;
        spec.max_len = MAX_RESPONSE_LEN;
        const auto r = util::read_until(
            link_.fd(), spec,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.serial_timeout_s * 1000));
        switch (r.outcome) {
            case util::ReadOutcome::Terminator:
                response = r.bytes;
                break;
            case util::ReadOutcome::Overflow:
                throw AlpacaException("iEAF response too long", AlpacaError::DriverException);
            case util::ReadOutcome::LinkDead:
                fail_link_locked("iEAF serial link lost (errno " + std::to_string(r.err) + ")");
            default:
                throw AlpacaException("iEAF read timeout", AlpacaError::DriverException);
        }
#else
        throw AlpacaException("iEAF read timeout", AlpacaError::DriverException);
#endif

        ALPACA_LOG_TRACE("iOptron", "iEAF response: " + response);
        return response;
    }

    // Flush stale input before every command: a previous blind command or a
    // half-read reply must not shift the fixed-width field parse of the next
    // response (same leaked-byte hazard the mount driver hit — see .github/instructions/ioptron.instructions.md).
    std::string send_command_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iEAF command: " + cmd);
        tcflush_port();
        write_data(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(COMMAND_DELAY_MS));
        return read_response();
    }

    // :FM / :FQ / :FZ answer with a single '1' byte and no '#' terminator
    // (INDI ieaffocus.cpp reads exactly one byte after each). Consume it
    // here: leaving it in the input buffer races the tcflush at the start
    // of the next :FI# — when it lands after the flush it prefixes the reply
    // ("1+013211...") and breaks the fixed-width parse. The ack is optional:
    // a firmware that stays silent just costs us READ_CHAR_TIMEOUT_MS. A
    // hangup is not silence: it loses the link like any other read.
    void send_command_blind_locked(const std::string& cmd) {
        ALPACA_LOG_TRACE("iOptron", "iEAF command (blind): " + cmd);
        tcflush_port();
        write_data(cmd);
#ifndef _WIN32
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Length;
        spec.length = 1;
        const auto r = util::read_until(
            link_.fd(), spec, std::chrono::steady_clock::now() + std::chrono::milliseconds(READ_CHAR_TIMEOUT_MS));
        if (r.outcome == util::ReadOutcome::Length) {
            if (r.bytes != "1") {
                ALPACA_LOG_WARN("iOptron", std::string("iEAF unexpected ack byte: ") + r.bytes);
            }
            return;
        }
        if (r.outcome == util::ReadOutcome::LinkDead) {
            fail_link_locked("iEAF serial link lost (errno " + std::to_string(r.err) + ")");
        }
#endif
        ALPACA_LOG_DEBUG("iOptron", "iEAF no ack for " + cmd);
    }

    mutable std::mutex mutex_;
    IeafConnectionConfig config_;
    bool connected_ = false;
    std::atomic<bool> link_lost_{false};  // set by fail_link_locked(), cleared by the next connect
    // The loss reason for get_link_fault(); its own mutex, never held across an exchange, so a getter cannot wait on
    // the wire while mutex_ is held by one.
    mutable std::mutex fault_mutex_;
    std::string fault_;

#ifndef _WIN32
    util::SerialLink link_;
#endif
};

// --- IeafProtocolWrapper public interface forwarding ---

IeafProtocolWrapper::IeafProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

IeafProtocolWrapper::~IeafProtocolWrapper() = default;

IeafDeviceInfo IeafProtocolWrapper::connect(const IeafConnectionConfig& config) { return impl_->connect(config); }

void IeafProtocolWrapper::disconnect() { impl_->disconnect(); }

bool IeafProtocolWrapper::is_connected() const { return impl_->is_connected(); }

bool IeafProtocolWrapper::link_alive() const noexcept { return impl_->link_alive(); }

std::string IeafProtocolWrapper::link_fault() const { return impl_->link_fault(); }

IeafStatus IeafProtocolWrapper::get_status() { return impl_->get_status(); }

void IeafProtocolWrapper::move_to(std::int32_t position) { impl_->move_to(position); }

void IeafProtocolWrapper::halt() { impl_->halt(); }

void IeafProtocolWrapper::set_zero() { impl_->set_zero(); }

}  // namespace alpacacore::vendor::ioptron

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

// Protocol reference: QHY's published QHYCFW3 serial command table and INDI
// drivers/filter_wheel/qhycfw3.cpp, cross-checked against a 7-slot CFW3
// (firmware 20181114, CP2102 bridge) on 2026-09-15. Summary in
// AlpacaCore/external/QHY/QHYCFW3-USB-protocol.md.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/serial_read.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
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
// Operator-facing owner name for the serial-port registry (#1202), shared by
// the probe and the connected link so the auto-detect scan can tell a
// connected (or held) QHYCFW3 from another vendor's device.
constexpr const char* kSerialOwner = "QHY CFW3 filter wheel";
constexpr int kFirmwareReplyLen = 8;  // "yyyymmdd"
// Replies carry no terminator, so a multi-byte reply (VRS) is complete when
// the line goes quiet. At 9600 baud a byte takes ~1 ms; 50 ms of silence is
// a comfortable end-of-reply mark and far below the arrival wait it paces.
constexpr int kReplyIdleMs = 50;
// The goto wait is sliced so the cancel flag is polled while the wheel turns.
constexpr int kMoveSliceMs = 100;
// Held-port reconnect: how long the line must stay quiet before the handshake
// (a late arrival byte is ~1 ms long; this is generous, not a boot wait).
constexpr int kReconnectSettleMs = 300;
constexpr int kMaxSlot = 15;  // 'F'

std::string printable(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char ch : raw) {
        out += std::isprint(ch) ? static_cast<char>(ch) : '.';
    }
    return out;
}

#ifndef _WIN32
// 9600 8N1 raw. HUPCL is left as the port has it: the wheel resets on the
// DTR edge of the OPEN, and clearing HUPCL did not prevent that (see
// connect()). The fd stays non-blocking (SerialLink); reads go through
// util::tty_read() and writes through util::tty_write_all(), which waits out
// an EAGAIN on an idle USB endpoint instead of failing (the Q-Focuser lesson).
void configure_tty(struct termios& tty) { util::make_raw_8n1(tty, B9600); }

// Read up to `max_len` bytes: wait up to `first_byte_ms` for the first byte,
// then keep collecting until `max_len` is reached or the line has been idle
// for kReplyIdleMs (util::read_until, IdleGap). Sets link_dead when the port
// is gone (a hangup, EOF, or EIO/ENXIO/ENODEV/EBADF) so the caller fails the transaction at once instead
// of re-polling to the deadline.
std::string read_reply(int fd, int first_byte_ms, std::size_t max_len, bool& link_dead) {
    // Bounded by poll() on the non-blocking fd: a retune to VMIN=1 by another
    // program cannot park this read (#1202). A hangup reads as LinkDead, never
    // as a quiet timeout. The reply is raw bytes, so nothing is skipped; the
    // overall deadline is the first-byte window plus one idle window per byte.
    // That cap cannot cut a reply the idle rule would accept: every byte after
    // the first must arrive within kReplyIdleMs of the one before it, so
    // max_len bytes need at most first_byte_ms + (max_len - 1) * kReplyIdleMs,
    // and the closing idle window of the last byte fits in the one spare window.
    link_dead = false;
    // max_len 0 asks for no bytes; read_until would still return one as Overflow.
    if (max_len == 0) return {};
    util::ReadSpec spec;
    spec.stop = util::ReadSpec::Stop::IdleGap;
    spec.idle_gap = std::chrono::milliseconds(kReplyIdleMs);
    spec.skip.clear();
    spec.max_len = max_len;
    spec.first_byte_timeout = std::chrono::milliseconds(first_byte_ms);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(first_byte_ms) +
                          static_cast<std::int64_t>(max_len) * std::chrono::milliseconds(kReplyIdleMs);
    util::ReadResult reply = util::read_until(fd, spec, deadline);
    link_dead = reply.outcome == util::ReadOutcome::LinkDead;
    return std::move(reply.bytes);
}

// Wait for the boot byte the wheel emits once it has homed after the DTR
// reset that opening the port caused. Returns the position it reported, or
// nullopt when nothing arrived (the port did not reset -- a pty, or a bridge
// that kept DTR steady). Any preceding noise is skipped; the LAST valid slot
// character wins so a repeated byte is harmless.
std::optional<int> wait_for_boot_byte(int fd, int timeout_ms, bool& link_dead) {
    link_dead = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::optional<int> position;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const std::string chunk = read_reply(fd, static_cast<int>(remaining), 8, link_dead);
        if (link_dead) return std::nullopt;
        if (chunk.empty()) break;  // deadline with nothing more
        for (char ch : chunk) {
            if (auto slot = cfw3_parse_slot(ch)) position = slot;
        }
        if (position) return position;
    }
    return position;
}
#endif

}  // namespace

std::optional<char> cfw3_slot_to_command(int slot) {
    if (slot < 0 || slot > kMaxSlot) return std::nullopt;
    if (slot < 10) return static_cast<char>('0' + slot);
    return static_cast<char>('A' + (slot - 10));
}

std::optional<int> cfw3_parse_slot(char reply) {
    if (reply >= '0' && reply <= '9') return reply - '0';
    if (reply >= 'A' && reply <= 'F') return 10 + (reply - 'A');
    return std::nullopt;
}

// Probe one port: open, sit out the boot, ask MXP then NOW. VRS is read too
// but optional. Returns true only when both single-character replies parse.
// The probe claim (#1202) is held for exactly the probe: a port a connected
// device holds is refused before it is opened (DEBUG log, "not found").
static bool probe_port(const std::string& port_path, int boot_timeout_ms, Cfw3DeviceInfo& info) {
#ifndef _WIN32
    std::string error;
    util::SerialLink link =
        util::SerialLink::open(port_path, util::SerialClaimKind::Probe, kSerialOwner, configure_tty, &error);
    if (!link) {
        ALPACA_LOG_DEBUG(kLogTag, "Not probing " + port_path + " for QHYCFW3: " + error);
        return false;
    }
    const int fd = link.fd();
    tcflush(fd, TCIOFLUSH);
    bool link_dead = false;
    const auto boot = wait_for_boot_byte(fd, boot_timeout_ms, link_dead);
    if (link_dead) {
        return false;
    }
    auto ask = [&](const char* cmd, std::size_t max_len) -> std::string {
        tcflush(fd, TCIFLUSH);
        if (!util::tty_write_all(fd, cmd, std::char_traits<char>::length(cmd))) return {};
        tcdrain(fd);
        return read_reply(fd, 3000, max_len, link_dead);
    };
    const std::string vrs = ask("VRS", kFirmwareReplyLen);
    const std::string mxp = link_dead ? std::string() : ask("MXP", 1);
    const std::string now = link_dead ? std::string() : ask("NOW", 1);
    link.close();
    if (mxp.size() != 1 || now.size() != 1) return false;
    const auto slots = cfw3_parse_slot(mxp[0]);
    const auto pos = cfw3_parse_slot(now[0]);
    if (!slots || !pos) return false;
    info.firmware = vrs.size() == kFirmwareReplyLen ? vrs : std::string();
    info.slot_count = *slots;
    info.position = *pos;  // NOW is the fresher of the two; the boot byte only proves the reset happened
    (void)boot;
    return true;
#else
    (void)port_path;
    (void)boot_timeout_ms;
    (void)info;
    return false;
#endif
}

#ifndef _WIN32
namespace {
// The raw /dev/ttyUSBn fallback has no by-id name to filter on; restrict it
// to the Silicon Labs CP210x class the wheel ships with so the scan never
// DTR-resets an unrelated adapter (a CH340 mount cable, an FTDI focuser).
bool raw_port_looks_like_cfw3_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor, {"10c4", "Silicon Labs", "CP210"});
}
}  // namespace
#endif

std::vector<Cfw3PortInfo> enumerate_cfw3_ports(int boot_timeout_ms) {
    std::vector<Cfw3PortInfo> results;

#ifndef _WIN32
    std::set<std::string> probed;

    struct Candidate {
        std::string path;
        std::string name;
        bool sibling = false;  // held by a connected (or held-open) QHYCFW3: listed, never probed
    };
    std::vector<Candidate> candidates;

    // #1202: a port another vendor's connected device holds is never opened
    // (the probe DTR-resets whatever is behind it). A port another QHYCFW3
    // holds -- connected, or disconnected with its fd held to skip the boot
    // -- is not probed either but keeps its place in the scan order, so the
    // auto-detect index of every other wheel stays stable; a driver that
    // picks it is refused by its link claim, which names the holder.
    auto add_candidate = [&candidates](const std::string& resolved, const std::string& name) {
        switch (util::serial_candidate_use(resolved, kSerialOwner)) {
            case util::SerialCandidateUse::HeldByOther:
                ALPACA_LOG_DEBUG(kLogTag, "Skipping " + resolved + ": held by another connected device");
                return;
            case util::SerialCandidateUse::HeldBySibling:
                candidates.push_back({resolved, name, true});
                return;
            case util::SerialCandidateUse::Free:
                candidates.push_back({resolved, name, false});
                return;
        }
    };

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            // udev name seen on hardware:
            // usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
            const bool is_candidate = (name.find("Silicon_Labs") != std::string::npos) ||
                                      (name.find("CP210") != std::string::npos) ||
                                      (name.find("10c4") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            probed.insert(resolved);
            add_candidate(resolved, name);
        }
    }

    // Raw /dev/ttyUSB* pass, run unconditionally: two CP2102s with the same
    // (absent) serial number collide on their by-id name and only one gets a
    // symlink. Descriptor-filtered and deduped against the by-id pass.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_cfw3_candidate(resolved)) continue;
        probed.insert(resolved);
        add_candidate(resolved, "");
    }

    const auto to_probe = static_cast<std::size_t>(
        std::count_if(candidates.begin(), candidates.end(), [](const Candidate& c) { return !c.sibling; }));
    if (to_probe != 0) {
        ALPACA_LOG_WARN(kLogTag, "Probing " + std::to_string(to_probe) +
                                     " CP210x port(s) for a QHYCFW3; opening a port resets the device behind "
                                     "it and a wheel takes ~17 s to home");
    }

    // Probe concurrently so the wall time is one boot, not one per adapter.
    std::vector<Cfw3DeviceInfo> infos(candidates.size());
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.sibling) continue;
        ALPACA_LOG_INFO(kLogTag, "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") + " for QHYCFW3...");
        workers.emplace_back([&, i] { found[i] = probe_port(candidates[i].path, boot_timeout_ms, infos[i]) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.sibling) {
            ALPACA_LOG_DEBUG(kLogTag, "Not probing " + c.path + ": another QHYCFW3 holds it");
            results.push_back({c.path, c.name, Cfw3DeviceInfo{}});
            continue;
        }
        if (!found[i]) continue;
        ALPACA_LOG_INFO(kLogTag, "Found QHYCFW3 on " + c.path + " (" + std::to_string(infos[i].slot_count) +
                                     " slots, firmware " + (infos[i].firmware.empty() ? "unknown" : infos[i].firmware) +
                                     ")");
        results.push_back({c.path, c.name, infos[i]});
    }
#else
    (void)boot_timeout_ms;
#endif

    return results;
}

class Cfw3ProtocolWrapper::Impl {
public:
    Impl() = default;
    ~Impl() {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        close_port_locked();
    }

    Cfw3DeviceInfo connect(const Cfw3ConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            throw AlpacaException("QHYCFW3 already connected", AlpacaError::InvalidOperation);
        }
        // Opening the port DTR-resets the wheel and costs its ~17 s boot, and
        // ConformU's Platform 7 Connect() abandons the device after 5 s of
        // Connecting. So the fd is HELD across logical disconnects: the first
        // connect of a process pays the boot, every later one on the same
        // port skips straight to the handshake (~10 ms). A different port, a
        // link loss (which released the fd) or a failed handshake (which
        // closes it) reopens and pays the boot again.
        const bool held = link_.is_open() && opened_port_config_ == config.serial_port;
        config_ = config;
        link_lost_.store(false);
        if (!held) {
            close_port_locked();
            open_port_locked();
        }

        Cfw3DeviceInfo info;
        in_connect_ = true;
        try {
#ifndef _WIN32
            if (held) {
                // A reconnect can land while the wheel is still finishing the
                // move the last session cancelled its wait on (PR #536 review).
                // The wheel is silent until it arrives, then emits one byte; a
                // flush alone leaves that byte free to land mid-handshake and
                // be read as the MXP or NOW answer. Drain until the line has
                // been idle for kReconnectSettleMs, so an arrival that is
                // already due is consumed here, and let the slot-count sanity
                // check below catch one that lands later.
                tcflush(serial_fd_, TCIOFLUSH);
                bool link_dead = false;
                const std::string stale = read_reply(serial_fd_, kReconnectSettleMs, 64, link_dead);
                if (link_dead) fail_link_locked();
                ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 port " + config_.serial_port +
                                              " held open since the last session; no reset, no boot wait" +
                                              (stale.empty() ? "" : "; drained " + printable(stale)));
            } else {
                bool link_dead = false;
                const auto boot = wait_for_boot_byte(serial_fd_, config_.boot_timeout_ms, link_dead);
                if (link_dead) fail_link_locked();
                if (boot) {
                    ALPACA_LOG_INFO(kLogTag,
                                    "QHYCFW3 finished its post-reset homing at slot " + std::to_string(*boot + 1));
                } else {
                    ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 sent no boot byte within " +
                                                  std::to_string(config_.boot_timeout_ms) +
                                                  " ms; assuming the port did not reset it");
                }
            }
#endif
            // VRS is optional: firmware before 201409 answers none of the
            // three queries, and a wheel that answers MXP/NOW but not VRS
            // still works. MXP and NOW are required -- without them there is
            // no slot count to validate against and no position to report.
            const std::string vrs = transact_locked("VRS", kFirmwareReplyLen, config_.reply_timeout_ms);
            if (vrs.size() == kFirmwareReplyLen) {
                info.firmware = vrs;
            } else if (!vrs.empty()) {
                ALPACA_LOG_WARN(kLogTag, "QHYCFW3 VRS reply has an unexpected shape: " + printable(vrs));
            }
            const std::string mxp = transact_locked("MXP", 1, config_.reply_timeout_ms);
            const std::string now = transact_locked("NOW", 1, config_.reply_timeout_ms);
            const auto slots = mxp.size() == 1 ? cfw3_parse_slot(mxp[0]) : std::nullopt;
            const auto pos = now.size() == 1 ? cfw3_parse_slot(now[0]) : std::nullopt;
            if (!slots || !pos) {
                throw AlpacaException(
                    "No QHYCFW3 answered on " + config_.serial_port +
                        ": check that the wheel's mode switch is in USB mode (red LED flash at power-on) and "
                        "that the port is the wheel's CP2102 bridge",
                    AlpacaError::NotConnected);
            }
            // The current slot must exist on the wheel. A count below the
            // position is what an arrival byte read as the MXP answer looks
            // like (a 7-slot wheel answering '2'); refuse rather than run with
            // a wheel that rejects most of its slots. The fd is released by the
            // catch below, so the next connect re-opens and re-homes.
            if (*slots <= 0 || *pos >= *slots) {
                throw AlpacaException("QHYCFW3 handshake is out of step (slot count " + std::to_string(*slots) +
                                          ", position " + std::to_string(*pos + 1) +
                                          "); the wheel may still have been moving. Try again in a few seconds",
                                      AlpacaError::DriverException);
            }
            info.slot_count = *slots;
            info.position = *pos;
        } catch (...) {
            in_connect_ = false;
            close_port_locked();
            throw;
        }
        in_connect_ = false;
        connected_ = true;
        ALPACA_LOG_INFO(kLogTag, "QHYCFW3 connected on " + config_.serial_port + " (" +
                                     std::to_string(info.slot_count) + " slots, firmware " +
                                     (info.firmware.empty() ? "unknown" : info.firmware) + ", at slot " +
                                     std::to_string(info.position + 1) + ")");
        return info;
    }

    // Logical only: the fd stays open so the next connect() does not reset
    // the wheel (see connect()). The port is released by the destructor, a
    // link loss, a failed handshake, or a connect() on a different port.
    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            ALPACA_LOG_INFO(kLogTag, "QHYCFW3 disconnected; " + config_.serial_port +
                                         " stays open (and reserved) so the next connect skips the wheel's "
                                         "~17 s reset. Remove the device to release the port");
        }
#endif
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool link_alive() const noexcept { return !link_lost_.load(); }

    int get_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const std::string now = transact_locked("NOW", 1, config_.reply_timeout_ms);
        const auto pos = now.size() == 1 ? cfw3_parse_slot(now[0]) : std::nullopt;
        if (!pos) {
            throw AlpacaException(
                "QHYCFW3 did not answer NOW" + (now.empty() ? std::string() : " (" + printable(now) + ")"),
                AlpacaError::DriverException);
        }
        return *pos;
    }

    int goto_slot(int slot, const std::atomic<bool>& cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const auto cmd = cfw3_slot_to_command(slot);
        if (!cmd) {
            throw AlpacaException("Filter position out of range for the QHYCFW3 protocol (max 16 slots)",
                                  AlpacaError::InvalidValue);
        }
#ifndef _WIN32
        if (serial_fd_ < 0) fail_link_locked();
        tcflush(serial_fd_, TCIFLUSH);
        write_locked(&*cmd, 1);
        // The reply is the arrival report; the wheel is silent while turning.
        // Slice the wait so a disconnect can cancel it -- the wheel finishes
        // the move on its own, and the next connect's NOW reports where it is.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.move_timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel.load()) {
                throw AlpacaException("QHYCFW3 move wait cancelled", AlpacaError::InvalidOperation);
            }
            bool link_dead = false;
            const std::string reply = read_reply(serial_fd_, kMoveSliceMs, 1, link_dead);
            if (link_dead) fail_link_locked();
            if (reply.empty()) continue;
            const auto arrived = cfw3_parse_slot(reply[0]);
            if (!arrived) {
                ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 ignoring non-slot byte during move: " + printable(reply));
                continue;
            }
            if (*arrived != slot) {
                // A slot byte that is not ours is a late arrival from a move a
                // previous session cancelled its wait on (PR #536 review): the
                // wheel only ever reports the slot it was sent to, so keep
                // waiting for that one rather than publishing a stale slot as
                // this goto's result.
                ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 ignoring arrival byte for slot " + std::to_string(*arrived + 1) +
                                              " while waiting for slot " + std::to_string(slot + 1));
                continue;
            }
            return *arrived;
        }
        throw AlpacaException("QHYCFW3 did not report arrival at slot " + std::to_string(slot + 1) + " within " +
                                  std::to_string(config_.move_timeout_ms) + " ms",
                              AlpacaError::DriverException);
#else
        (void)cancel;
        throw AlpacaException("QHYCFW3 driver is Linux-only", AlpacaError::DriverException);
#endif
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("QHYCFW3 not connected", AlpacaError::NotConnected);
        }
    }

    // Claim, open, lock and configure the port (#1202). A port another
    // connected device holds is refused before it is opened, and the reason
    // (naming the holder) is the connect refusal the client sees. The link,
    // and with it the registry claim and TIOCEXCL, is held across logical
    // disconnects exactly as the fd is (see connect()).
    void open_port_locked() {
#ifndef _WIN32
        std::string error;
        link_ = util::SerialLink::open(config_.serial_port, util::SerialClaimKind::Link, kSerialOwner, configure_tty,
                                       &error);
        if (!link_) {
            throw AlpacaException(error, AlpacaError::NotConnected);
        }
        serial_fd_ = link_.fd();
        opened_port_config_ = config_.serial_port;
        tcflush(serial_fd_, TCIOFLUSH);
#else
        throw AlpacaException("QHYCFW3 driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    // Release the port for real: the alias and the held-port record first,
    // then the link (TIOCNXCL, close, registry claim).
    void close_port_locked() {
#ifndef _WIN32
        serial_fd_ = -1;
        opened_port_config_.clear();
        link_.close();
#endif
    }

    // The port is gone (unplug / re-enumeration): release the fd so a
    // replugged wheel can come back under the same name, latch link_lost_
    // for the driver's Connected getter, and fail with NotConnected. During
    // connect() nothing is latched; connect() closes the port itself.
    [[noreturn]] void fail_link_locked() {
        if (!in_connect_) {
            ALPACA_LOG_ERROR(kLogTag, "QHYCFW3 link lost on " + config_.serial_port + " (port released)");
            link_lost_.store(true);
            connected_ = false;
            close_port_locked();
        }
        throw AlpacaException("QHYCFW3 link lost on " + config_.serial_port, AlpacaError::NotConnected);
    }

#ifndef _WIN32
    void write_locked(const char* data, std::size_t len) {
        if (!util::tty_write_all(serial_fd_, data, len)) {
            const int err = errno;
            if (err == EIO || err == ENXIO || err == ENODEV || err == EBADF) fail_link_locked();
            throw AlpacaException("QHYCFW3 write failed: " + util::errno_string(err), AlpacaError::DriverException);
        }
        tcdrain(serial_fd_);
    }
#endif

    // Send a query and read its reply (up to `max_len` bytes, ending on line
    // idle). Empty when the wheel stayed silent for `timeout_ms`.
    std::string transact_locked(const char* cmd, std::size_t max_len, int timeout_ms) {
#ifndef _WIN32
        if (serial_fd_ < 0) fail_link_locked();
        ALPACA_LOG_TRACE(kLogTag, std::string("QHYCFW3 command: ") + cmd);
        tcflush(serial_fd_, TCIFLUSH);
        write_locked(cmd, std::char_traits<char>::length(cmd));
        bool link_dead = false;
        const std::string reply = read_reply(serial_fd_, timeout_ms, max_len, link_dead);
        if (link_dead) fail_link_locked();
        ALPACA_LOG_TRACE(kLogTag, "QHYCFW3 reply: " + printable(reply));
        return reply;
#else
        (void)cmd;
        (void)max_len;
        (void)timeout_ms;
        throw AlpacaException("QHYCFW3 driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    mutable std::mutex mutex_;
    Cfw3ConnectionConfig config_;
    bool connected_ = false;
    std::atomic<bool> link_lost_{false};
    bool in_connect_ = false;  // guarded by mutex_
#ifndef _WIN32
    util::SerialLink link_;           // held across logical disconnects (see connect())
    int serial_fd_ = -1;              // link_.fd() while the link is open, else -1
    std::string opened_port_config_;  // config path the held link was opened from
#endif
};

Cfw3ProtocolWrapper::Cfw3ProtocolWrapper() : impl_(std::make_unique<Impl>()) {}
Cfw3ProtocolWrapper::~Cfw3ProtocolWrapper() = default;

Cfw3DeviceInfo Cfw3ProtocolWrapper::connect(const Cfw3ConnectionConfig& config) { return impl_->connect(config); }
void Cfw3ProtocolWrapper::disconnect() { impl_->disconnect(); }
bool Cfw3ProtocolWrapper::is_connected() const { return impl_->is_connected(); }
bool Cfw3ProtocolWrapper::link_alive() const noexcept { return impl_->link_alive(); }
int Cfw3ProtocolWrapper::get_position() { return impl_->get_position(); }
int Cfw3ProtocolWrapper::goto_slot(int slot, const std::atomic<bool>& cancel) { return impl_->goto_slot(slot, cancel); }

}  // namespace alpacacore::vendor::qhy

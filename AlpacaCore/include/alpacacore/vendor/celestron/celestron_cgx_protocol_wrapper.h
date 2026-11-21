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

#pragma once

#include <alpacacore/util/serial_link.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::celestron {

enum class CgxAxis : std::uint8_t { RightAscension = 0x10, Declination = 0x11 };

/// Line rate of a direct AUX serial link: 115200 for the CGX/CGX-L mount, 9600
/// for the focus motor's own USB port (issue #896).
enum class AuxLinkSpeed : std::uint8_t { Baud115200, Baud9600 };

/// Per-driver serial link to a Celestron CGX/CGX-L direct AUX interface.
class CelestronCgxProtocol final {
public:
    /// @param owner The device name a refused operator reads while this link
    ///              holds its port (decision 0017): the mount or the focuser.
    explicit CelestronCgxProtocol(std::string owner = "Celestron CGX mount") : owner_(std::move(owner)) {}
    ~CelestronCgxProtocol();

    CelestronCgxProtocol(const CelestronCgxProtocol&) = delete;
    CelestronCgxProtocol& operator=(const CelestronCgxProtocol&) = delete;

    /// @param probe_wait How long the port claim waits out another device's
    ///                   auto-detect probe on the port. A reconnect made under
    ///                   a driver lock passes zero, so it fails at once instead
    ///                   of blocking that lock's getters (#1457).
    void connect_serial(const std::string& port_path, AuxLinkSpeed speed = AuxLinkSpeed::Baud115200,
                        std::chrono::milliseconds probe_wait = util::kSerialProbeClaimWait);
    /// SkyPortal WiFi: AUX over TCP to the adapter at a numeric IPv4 or IPv6 `host` (no name lookup under the I/O
    /// lock) and `port`, closing any link held. Requests go out from source 0x20, whatever source the caller set.
    /// Input that arrives before the socket has been quiet for 200 ms (the adapter's startup backlog) is discarded.
    /// Unlike the serial link, a read-only motor query (position, model, home and slew status, guide status,
    /// version) whose reply is lost is re-sent once, 300 ms after it went out, when the exchange's deadline is
    /// later; nothing else is ever re-sent. The adapter closing the connection is a lost link (link_alive()).
    /// The link owns `host`:`port` process-wide until it closes: a second link to it is refused with InvalidOperation
    /// naming the holder. A motor controller's unsolicited report (0x50, 0x51) is acknowledged; anything but a
    /// low-battery warning fails the exchange as a session error.
    void connect_tcp(const std::string& host, int port, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void disconnect();
    /// Clears a desynchronized link on the port already held: flushes stale input
    /// and accepts exchanges again. Never opens a port, so a racing disconnect
    /// cannot be undone (issue #896); throws NotConnected on a closed link.
    /// is_healthy() stays false until the next reply, as after a same-port connect_serial().
    void resync();
    bool is_connected() const;
    bool is_healthy() const { return healthy_.load(); }
    /// False once an exchange saw the serial port gone (decision 0009 rule 2a): a hangup or end-of-file on the fd,
    /// or EIO/ENXIO/ENODEV/EBADF on a read or write (the util::SerialLink verdict since #2262). The port is then
    /// already released and that exchange threw NotConnected. A timeout is not evidence. Lock-free; the next
    /// connect_serial() clears it.
    bool link_alive() const noexcept { return !link_lost_.load(); }
    /// Why the link was lost, empty while it is alive. Never waits on an exchange.
    std::string link_fault() const;
    std::uint64_t session_generation() const { return session_generation_.load(); }
    /// Lost replies recovered by the one TCP re-send since the last connect (a counter, not a fault). Lock-free.
    std::uint64_t recovered_retries() const { return recovered_retries_.load(); }
    /// A last-resort stop: MC_MOVE_POS(0) and a zero rate written to `axis` without waiting for a reply, also while
    /// the stream is desynchronized. Not proof of a stop. Throws NotConnected on a closed link.
    void send_stop(CgxAxis axis);

    /// Every exchange without a timeout gets the transport's: 500 ms over USB, 1000 ms over TCP (room for the re-send).
    using Timeout = std::optional<std::chrono::milliseconds>;
    AuxFrame exchange(const AuxFrame& request, Timeout timeout = std::nullopt);
    std::uint32_t get_axis_position(CgxAxis axis, Timeout timeout = std::nullopt);
    void set_axis_position(CgxAxis axis, std::uint32_t position, Timeout timeout = std::nullopt);
    void goto_axis_fast(CgxAxis axis, std::uint32_t position, Timeout timeout = std::nullopt);
    void goto_axis_slow(CgxAxis axis, std::uint32_t position, Timeout timeout = std::nullopt);
    bool is_axis_slew_done(CgxAxis axis, Timeout timeout = std::nullopt);
    void start_axis_home(CgxAxis axis, Timeout timeout = std::nullopt);
    bool is_axis_home_done(CgxAxis axis, Timeout timeout = std::nullopt);
    void pulse_guide_axis(CgxAxis axis, int velocity_percent, int duration_centiseconds,
                          Timeout timeout = std::nullopt);
    bool is_aux_guide_active(CgxAxis axis, Timeout timeout = std::nullopt);

private:
    void goto_axis(CgxAxis axis, std::uint32_t position, std::uint8_t command, const char* operation, Timeout timeout);
    void disconnect_locked();
    bool take_report_locked(const AuxFrame& frame, std::chrono::steady_clock::time_point deadline);
    void read_available_locked();
    void drain_input_locked(std::chrono::steady_clock::time_point deadline);
    std::optional<std::vector<std::uint8_t>> buffered_frame_locked();
    [[noreturn]] void lose_link_locked(const std::string& reason);
    [[noreturn]] void fail_socket_locked(const char* operation, int code);
    void drain_backlog_locked();
    std::size_t discard_input_locked();
    void write_frame_locked(const std::vector<std::uint8_t>& bytes, std::chrono::steady_clock::time_point deadline);
    void wait_for_locked(short events, std::chrono::steady_clock::time_point deadline);
    std::vector<std::uint8_t> read_frame_locked(std::chrono::steady_clock::time_point deadline);

    const std::string owner_;
    mutable std::mutex mutex_;
    int serial_fd_ = -1;            // the tty from link_, or the socket while tcp_
    std::atomic<bool> tcp_{false};  // the transport of the last connect: SkyPortal TCP or USB serial
    std::atomic<std::uint64_t> session_generation_{0};
    std::atomic<bool> healthy_{false};
    std::atomic<std::uint64_t> recovered_retries_{0};
    std::string tcp_endpoint_;  // the "host:port" this link claims while its socket is open
    bool desynchronized_ = false;
    util::SerialLink link_;  // the claim, TIOCEXCL and the fd; serial_fd_ is link_.fd() while open
    std::vector<std::uint8_t> receive_buffer_;
    std::atomic<bool> link_lost_{false};  // set by lose_link_locked(), cleared by the next connect_serial()
    mutable std::mutex loss_mutex_;       // leaf: guards loss_reason_ only, never held across an exchange
    std::string loss_reason_;
};

}  // namespace alpacacore::vendor::celestron

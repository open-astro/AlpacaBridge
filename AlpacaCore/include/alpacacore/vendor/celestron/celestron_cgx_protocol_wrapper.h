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

#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::vendor::celestron {

enum class CgxAxis : std::uint8_t { RightAscension = 0x10, Declination = 0x11 };

/// Line rate of a direct AUX serial link: 115200 for the CGX/CGX-L mount, 9600
/// for the focus motor's own USB port (issue #896).
enum class AuxLinkSpeed : std::uint8_t { Baud115200, Baud9600 };

/// Per-driver serial link to a Celestron CGX/CGX-L direct AUX interface.
class CelestronCgxProtocol final {
public:
    CelestronCgxProtocol() = default;
    ~CelestronCgxProtocol();

    CelestronCgxProtocol(const CelestronCgxProtocol&) = delete;
    CelestronCgxProtocol& operator=(const CelestronCgxProtocol&) = delete;

    void connect_serial(const std::string& port_path, AuxLinkSpeed speed = AuxLinkSpeed::Baud115200);
    void disconnect();
    /// Clears a desynchronized link on the port already held: flushes stale input
    /// and accepts exchanges again. Never opens a port, so a racing disconnect
    /// cannot be undone (issue #896); throws NotConnected on a closed link.
    void resync();
    bool is_connected() const;
    bool is_healthy() const { return healthy_.load(); }
    std::uint64_t session_generation() const { return session_generation_.load(); }

    AuxFrame exchange(const AuxFrame& request, std::chrono::milliseconds timeout);
    std::uint32_t get_axis_position(CgxAxis axis, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    void set_axis_position(CgxAxis axis, std::uint32_t position,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    void goto_axis_fast(CgxAxis axis, std::uint32_t position,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    void goto_axis_slow(CgxAxis axis, std::uint32_t position,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    bool is_axis_slew_done(CgxAxis axis, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    void start_axis_home(CgxAxis axis, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    bool is_axis_home_done(CgxAxis axis, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    void pulse_guide_axis(CgxAxis axis, int velocity_percent, int duration_centiseconds,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
    bool is_aux_guide_active(CgxAxis axis, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

private:
    void goto_axis(CgxAxis axis, std::uint32_t position, std::uint8_t command, const char* operation,
                   std::chrono::milliseconds timeout);
    void disconnect_locked();
    void write_frame_locked(const std::vector<std::uint8_t>& bytes, std::chrono::steady_clock::time_point deadline);
    void wait_for_locked(short events, std::chrono::steady_clock::time_point deadline);
    std::vector<std::uint8_t> read_frame_locked(std::chrono::steady_clock::time_point deadline);

    mutable std::mutex mutex_;
    int serial_fd_ = -1;
    std::atomic<std::uint64_t> session_generation_{0};
    std::atomic<bool> healthy_{false};
    bool desynchronized_ = false;
    std::string registered_port_path_;
    std::vector<std::uint8_t> receive_buffer_;
};

}  // namespace alpacacore::vendor::celestron

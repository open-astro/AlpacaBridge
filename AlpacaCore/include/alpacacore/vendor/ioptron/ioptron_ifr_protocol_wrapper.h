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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::vendor::ioptron {

// The iOptron iFR focuser-rotator (2", 3", 4"): one unit, two ASCOM devices (a
// Focuser and a Rotator) over ONE USB-serial link. Protocol summary and sources
// (iOptron Commander 10.0.2, INDI iafscaa.cpp, rig captures on an iFR 3"):
// AlpacaCore/external/iOptron/iFR-protocol.md.

struct IfrConnectionConfig {
    std::string serial_port;  // e.g. "/dev/ttyUSB0"; either of the unit's two Type-C ports
};

/// The body size the user picked (config "model": "ifr2", "ifr3" (default), "ifr4"). The user names the unit,
/// as on the iEAF and iEFW; the firmware's own size register (:RI#, 20/30/40) is only compared at connect and a
/// mismatch is logged. Unknown values read as the 3".
struct IfrModel {
    int size_tenths;        // 20, 30 or 40: what :RI# should report
    const char* label;      // "iFR 3\""
    std::int32_t max_step;  // focuser travel in 0.4 um steps (25 mm on the 2"/3", 26 mm on the 4")
};
IfrModel ifr_model_for(std::string_view model);

/// The :DeviceInfo# handshake ("%6d%2d%4d#": position field, model code, build) plus the :FW1# firmware
/// pair ("<6 focuser><6 CAA>#"), read once when the link opens.
struct IfrDeviceInfo {
    std::int32_t model = 0;        // 5 = iAFS + CAA (the iFR); 2 iEAF, 3 iAFS, 4 iAFS without CAA
    std::int32_t build = 0;        // the handshake's last 4 digits ("0100")
    std::string focuser_firmware;  // "260324"
    std::string rotator_firmware;  // "260324"
};

/// True for the iFR's :DeviceInfo# model code (5). The iEAF driver accepts 2 and 3 only, so the two drivers
/// never claim each other's units.
bool is_ifr_model(std::int32_t model);

/// :FI# -> "%7d%1d%5d%1d#": position (signed), moving, temperature (Kelvin x 100), direction flag.
struct IfrFocuserStatus {
    std::int32_t position = 0;
    bool moving = false;
    double temperature_c = 0.0;
    int direction = 0;  // the firmware's direction flag, toggled by :FR#; the driver never sends :FR#
};

/// :RI# -> "%10d%1d%9d%1d%2d#": sync offset (signed, 0.01 arcsec), moving, position (0.01 arcsec, the
/// synced angle, 0..360), direction (1 normal, 0 reversed), body size in tenths of an inch (20/30/40).
struct IfrRotatorStatus {
    double position_deg = 0.0;  // synced angle: the ASCOM Position
    double offset_deg = 0.0;    // Position - MechanicalPosition, as the firmware stores it
    bool moving = false;
    bool reversed = false;
    int size_tenths = 0;
};

/// A motion or setting command's one-byte answer (no '#'): '1' accepted, '0' refused (the focuser at an end
/// stop or outside its range), nothing at all within the ack wait.
enum class IfrAck : std::uint8_t { Accepted, Refused, Silent };

/// A serial port that answered the handshake as an iFR (or that a connected iFR session holds).
struct IfrPortInfo {
    std::string port_path;
    std::string device_id;  // by-id symlink name, empty for raw nodes
    IfrDeviceInfo info;     // empty when the port was counted without a probe (held by a connected iFR)
};

/// The auto-detect scan: Prolific candidates from /dev/serial/by-id and raw /dev/ttyUSB*, each probed with
/// :DeviceInfo# under a probe claim. A port held by a connected iFR session keeps its place without a probe
/// (decision 0017 rule 3). A unit with both cables plugged in shows as two ports here; the drivers pick by index,
/// so the focuser and the rotator of one unit land on the same port.
std::vector<IfrPortInfo> enumerate_ifr_ports();

/// One port's probe: :DeviceInfo# under a probe claim. True, with @p info filled, only for model code 5.
bool probe_ifr_port(const std::string& port_path, IfrDeviceInfo& info);

/**
 * @brief The shared serial session of one iFR: both drivers talk to the unit through it.
 *
 * acquire() returns the live session on the same port when one exists, and otherwise opens the port
 * (util::SerialLink, owner "iOptron iFR") and runs the handshake. The port closes when the last holder drops
 * its pointer, so the focuser and the rotator connect and disconnect independently while sharing one fd.
 *
 * Every exchange runs under the session's I/O lock, the per-link lock of decision 0016; nothing else is held
 * across the wire. A hangup, or EIO/ENXIO/ENODEV on a write, loses the link at once for both devices
 * (decision 0009 rule 2): the port is released, link_alive() turns false and the reason is kept for
 * link_fault(). The next acquire() then opens a fresh session.
 */
class IfrLink {
public:
    /// Join the live session on @p config's port, or open one. Throws AlpacaException NotConnected with the
    /// operator-facing reason when the port cannot be opened or does not answer as an iFR.
    static std::shared_ptr<IfrLink> acquire(const IfrConnectionConfig& config);

    ~IfrLink();
    IfrLink(const IfrLink&) = delete;
    IfrLink& operator=(const IfrLink&) = delete;

    /// The port as opened (canonical). Fixed for the session's life.
    const std::string& port() const noexcept { return port_; }
    /// The handshake and firmware read at open. Fixed for the session's life.
    const IfrDeviceInfo& info() const noexcept { return info_; }

    /// False once an exchange has seen the port gone. Lock-free.
    bool link_alive() const noexcept;
    /// Why the link was lost ("iFR serial link lost (errno N)"), or empty. Never waits on the wire.
    std::string link_fault() const;

    IfrFocuserStatus focuser_status();
    /// :FM%07u#. The caller range-checks; Refused means the firmware would not move that way.
    IfrAck focuser_move(std::int32_t position);
    /// :FQ#
    IfrAck focuser_halt();

    IfrRotatorStatus rotator_status();
    /// :RM%09u#, the target in synced degrees [0, 360): the firmware applies its own offset and never crosses
    /// 0/360 of the synced frame.
    IfrAck rotator_move(double position_deg);
    /// :RQ#
    IfrAck rotator_halt();
    /// :RY%09u#: Position becomes @p position_deg; the firmware keeps the offset across power loss.
    IfrAck rotator_sync(double position_deg);
    /// :RR0# reversed / :RR1# normal. Mirrors the reported Position.
    IfrAck rotator_set_reverse(bool reversed);

    struct OpenTag {};
    IfrLink(OpenTag, const IfrConnectionConfig& config);  // opens and handshakes; use acquire()

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::string port_;
    IfrDeviceInfo info_;
};

/// Test seam: forget every registered session (each holder keeps its own pointer). For test isolation only.
void reset_ifr_link_registry_for_testing();

}  // namespace alpacacore::vendor::ioptron

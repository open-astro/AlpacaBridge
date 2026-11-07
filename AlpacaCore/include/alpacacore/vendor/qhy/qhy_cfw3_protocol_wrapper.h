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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::qhy {

/**
 * @brief Serial settings for a QHYCFW3 driven over its own USB port.
 *
 * The wheel's USB socket is a Silicon Labs CP2102 UART bridge (USB
 * 10c4:ea60, enumerates as /dev/ttyUSBn) in front of an ATmega328P running
 * QHY's Arduino-class firmware, 9600 8N1. It does not use the QHY camera SDK:
 * that path (SendOrder2QHYCCDCFW on the camera handle) is the integrated CFW
 * driver in qhy_filterwheel_driver.cpp. The wheel's mode switch must be in
 * USB mode (the LED flashes red for a second at power-on); in 4-pin mode the
 * MCU boots and homes on USB power but reads commands from the camera port
 * only.
 */
struct Cfw3ConnectionConfig {
    std::string serial_port;  // e.g. "/dev/ttyUSB0"
    int baud_rate = 9600;     // fixed by the firmware
    // Opening the port asserts DTR, which resets the MCU (Arduino auto-reset);
    // the wheel then homes and emits its position digit ~17 s after the open.
    // Commands sent before that byte are discarded, so connect() waits for it
    // up to this long, and proceeds without it when the port did not reset.
    int boot_timeout_ms = 25000;
    int reply_timeout_ms = 3000;  // VRS/MXP/NOW answer within ~2 ms on hardware
    int move_timeout_ms = 40000;  // a full 16-slot traverse is well under this
};

/**
 * @brief Identity gathered by the connect handshake.
 */
struct Cfw3DeviceInfo {
    std::string firmware;  // VRS reply, yyyymmdd (e.g. "20181114"); empty when the wheel did not answer VRS
    int slot_count = 0;    // MXP reply; the number of positions (7 on a 7-slot wheel, see the header note)
    int position = 0;      // NOW reply, 0-based
};

/**
 * @brief A serial port that answered the CFW3 probe.
 */
struct Cfw3PortInfo {
    std::string port_path;  // e.g. "/dev/ttyUSB0"
    std::string device_id;  // by-id symlink name, empty for raw nodes
    Cfw3DeviceInfo info;
};

/// Slot index -> the goto command character: '0'..'9' then 'A'..'F' (slot 15).
/// Returns nullopt outside 0..15, the protocol's 16-position ceiling.
std::optional<char> cfw3_slot_to_command(int slot);

/// Reply character -> slot index, the inverse of cfw3_slot_to_command().
std::optional<int> cfw3_parse_slot(char reply);

/**
 * @brief Enumerate serial ports that host a QHYCFW3 in USB control mode.
 *
 * Scans /dev/serial/by-id/ for CP210x bridges, then raw /dev/ttyUSB0..9 nodes
 * whose USB descriptor is a Silicon Labs CP210x, and probes each with the
 * boot-wait + MXP handshake. The probe opens the port, which resets the
 * device behind it: every CP2102 on the machine that is not a CFW3 is reset
 * too, and a wheel costs its ~17 s boot. Prefer an explicit port when the
 * machine has other CP210x adapters. Ports held open by another connected
 * device are skipped.
 */
std::vector<Cfw3PortInfo> enumerate_cfw3_ports(int boot_timeout_ms = 25000);

/**
 * @brief Protocol wrapper for the QHYCFW3 USB serial protocol.
 *
 * Every command is bare ASCII with no terminator, and so is every reply:
 *   '0'..'F'  goto that slot; the wheel answers with the same character once
 *             it has ARRIVED (about 1 s per slot travelled), immediately when
 *             it is already there (firmware 201409 and later).
 *   "VRS"     firmware date, 8 characters.
 *   "MXP"     slot count, one character (this firmware answers the count;
 *             QHY's protocol table says highest index -- see the driver notes).
 *   "NOW"     current slot, one character.
 *   "RESET"   re-home; the wheel emits its position digit when done.
 * Protocol reference: QHY's published command table and INDI
 * drivers/filter_wheel/qhycfw3.cpp, both cross-checked against a 7-slot
 * CFW3 (firmware 20181114) on 2026-09-15.
 */
class Cfw3ProtocolWrapper {
public:
    Cfw3ProtocolWrapper();
    ~Cfw3ProtocolWrapper();

    Cfw3ProtocolWrapper(const Cfw3ProtocolWrapper&) = delete;
    Cfw3ProtocolWrapper& operator=(const Cfw3ProtocolWrapper&) = delete;

    /**
     * @brief Open the port, wait out the DTR-reset boot, then VRS/MXP/NOW.
     *
     * The fd is held across disconnect(): a reconnect on the same port skips
     * the open (no DTR pulse, no boot) and goes straight to the handshake,
     * which is what keeps a reconnect inside ConformU's 5 s Connecting
     * budget. Throws NotConnected when the wheel does not answer MXP and NOW
     * (the port is released so the next attempt reopens it).
     */
    Cfw3DeviceInfo connect(const Cfw3ConnectionConfig& config);

    /** @brief Logical disconnect; the port stays open (see connect()). */
    void disconnect();

    bool is_connected() const;

    /**
     * @brief False once a transaction has seen the port die since the last
     * connect(): POLLHUP, EOF, or EIO/ENXIO/ENODEV/EBADF (the #445 shape).
     * The fd is released at that moment and every later command throws
     * NotConnected until a new connect(). Lock-free.
     */
    bool link_alive() const noexcept;

    /** @brief Why the link was lost (one text); empty until a loss, cleared by connect() and disconnect(). */
    std::string link_fault() const;

    /** @brief When the link was lost (stamped once per outage), read and cleared; nullopt after a clean disconnect. */
    std::optional<std::chrono::steady_clock::time_point> consume_link_lost_at();

    /** @brief "NOW" -> current slot, 0-based. */
    int get_position();

    /**
     * @brief Goto `slot` and block until the wheel reports arrival.
     *
     * Holds the wrapper mutex for the whole move (nothing else may be on the
     * wire while the wheel is turning). `cancel` is polled between read
     * slices: when it becomes true the wait ends with InvalidOperation and the
     * wheel keeps turning to the slot on its own. Times out with
     * DriverException after move_timeout_ms. Returns the slot the wheel
     * reported.
     */
    int goto_slot(int slot, const std::atomic<bool>& cancel);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::qhy

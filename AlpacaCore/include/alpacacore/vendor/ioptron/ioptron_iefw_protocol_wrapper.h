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
#include <vector>

namespace alpacacore::vendor::ioptron {

// The iEFW is USB-serial only (same Prolific bridge family as the iEAF /
// iAFS2/3 focusers), fixed 115200 8N1.
struct IefwConnectionConfig {
    std::string serial_port;   // e.g., "/dev/ttyUSB0"
    int serial_timeout_s = 4;  // matches INDI iEFW_TIMEOUT
};

/**
 * @brief Identity returned by the :DeviceInfo# handshake.
 *
 * Response is 12 digits + '#', parsed as "%6d%2d%4d": a position field,
 * the model code (99 = iEFW-15 with 5 slots, 98 = iEFW-18 with 8 slots),
 * and a 4-digit firmware/build number. Any other model code is not an
 * iEFW (the iOptron mounts and iEAF/iAFS focusers share the same handshake
 * and USB-serial chip class, so the model code is what tells them apart).
 */
struct IefwDeviceInfo {
    std::int32_t position = 0;
    std::int32_t model = 0;
    std::int32_t firmware = 0;
};

/** @brief True when a :DeviceInfo# model code belongs to an iEFW (98 or 99). */
bool is_iefw_model(std::int32_t model);

/** @brief Slot count for an iEFW model code: 99 -> 5, 98 -> 8, else 0. */
std::int32_t iefw_slot_count(std::int32_t model);

/** @brief Model name for an iEFW model code: 99 -> "iEFW-15", 98 -> "iEFW-18", else "iEFW". */
std::string iefw_model_name(std::int32_t model);

/**
 * @brief Information about a detected serial port that hosts an iEFW.
 */
struct IefwPortInfo {
    std::string port_path;  // e.g., "/dev/ttyUSB0"
    std::string device_id;  // by-id symlink name, empty for raw nodes
    IefwDeviceInfo info;    // populated after successful probe
};

/**
 * @brief Enumerate serial ports that host iOptron iEFW filter wheels.
 *
 * Scans /dev/serial/by-id/ for Prolific-class USB-serial adapters, then
 * raw /dev/ttyUSB0..9 nodes, probing each with :DeviceInfo#. Only ports
 * that answer with an iEFW model code (98 or 99) are returned.
 */
std::vector<IefwPortInfo> enumerate_iefw_ports();

/**
 * @brief Protocol wrapper for the iOptron iEFW filter wheel.
 *
 * Serial protocol at 115200 8N1 (INDI drivers/filter_wheel/ioptron_wheel.cpp):
 *   :DeviceInfo#      -> 12 digits '#'   identity (see IefwDeviceInfo)
 *   :FW1#             -> 12 chars '#'    firmware string
 *   :WP#              -> "nn#" or "-1#"  current slot (0-based), -1 while moving
 *   :WMnn#            -> '1'             move to slot nn (0-based, 2 digits)
 *   :WFnn#            -> "snnnnn#"       focus offset stored in the wheel for slot nn
 *   :WOnnsnnnnn#      -> '1'             store focus offset for slot nn
 * Move/offset-set commands answer with a single '1' byte and no '#'.
 */
class IefwProtocolWrapper {
public:
    IefwProtocolWrapper();
    ~IefwProtocolWrapper();

    IefwProtocolWrapper(const IefwProtocolWrapper&) = delete;
    IefwProtocolWrapper& operator=(const IefwProtocolWrapper&) = delete;

    /**
     * @brief Connect and handshake with :DeviceInfo#.
     * @return Device identity (model must be 98 or 99, else this throws)
     */
    IefwDeviceInfo connect(const IefwConnectionConfig& config);

    void disconnect();

    bool is_connected() const;

    /** @brief Firmware string from :FW1# (12 characters). */
    std::string get_firmware();

    /** @brief Current slot (0-based) from :WP#, or -1 while the wheel is moving. */
    std::int32_t get_position();

    /** @brief Start a move to slot (0-based). Command :WMnn#; completion is observed by polling get_position(). */
    void move_to(std::int32_t slot);

    /** @brief Read the focus offset the wheel stores for a slot (:WFnn#). */
    std::int32_t get_stored_offset(std::int32_t slot);

    /** @brief Store a focus offset in the wheel for a slot (:WOnnsnnnnn#). */
    void set_stored_offset(std::int32_t slot, std::int32_t offset);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::ioptron

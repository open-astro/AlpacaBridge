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

#include <string>

namespace alpacacore::rig {

/**
 * @brief Per-board identifier shared with the OpenAstro OS images.
 *
 * The images (openastro-raspberrypi, openastro-touptek-stellavita,
 * rk-flashtool, ...) suffix the WiFi AP SSID with the last four hex digits
 * of the wlan0 MAC (e.g. OpenAstro-915D) so several boards at one site don't
 * collide. AlpacaBridge derives the same four characters so the identity a
 * user sees in NINA / ASCOM Remote matches the network they joined.
 *
 * Resolution order (first hit wins, cached for the process lifetime):
 *   1. ALPACACORE_RIG_ID environment variable (tests, scripted installs)
 *   2. last 4 hex of /sys/class/net/wlan0/address
 *   3. last 4 hex of the first non-loopback interface with a non-zero MAC
 *   4. first 4 hex of /etc/machine-id
 *   5. "0000"
 *
 * Always returns exactly four uppercase hex characters.
 */
const std::string& rig_id();

/**
 * @brief "XXXX: <name>" -- the DeviceName prefix form.
 *
 * A prefix (not a suffix) so the distinguishing part survives NINA's
 * fixed-width device dropdown, which truncates long names at the tail.
 */
std::string prefix_device_name(const std::string& name);

/**
 * @brief "<unique_id>-XXXX" -- scopes a driver's per-process UniqueID to
 * this board so two boards running the same driver on the same LAN never
 * report the same Alpaca UniqueID.
 */
std::string scope_unique_id(const std::string& unique_id);

} // namespace alpacacore::rig

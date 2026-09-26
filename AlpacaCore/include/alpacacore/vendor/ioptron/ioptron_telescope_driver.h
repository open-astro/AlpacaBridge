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

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>

#include <memory>
#include <optional>

namespace alpacacore::vendor::ioptron {

/**
 * @brief Factory function to create iOptron telescope driver.
 *
 * @param device_number Alpaca device number
 * @param connection_info Connection information (serial port or network)
 * @return Unique pointer to telescope driver
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope(
    int device_number,
    const ConnectionInfo& connection_info);

/**
 * @brief Factory function to create iOptron telescope driver with site defaults.
 *
 * @param device_number Alpaca device number
 * @param connection_info Connection information (serial port or network)
 * @param site_latitude_deg Optional site latitude in degrees
 * @param site_longitude_deg Optional site longitude in degrees
 * @param site_elevation_m Optional site elevation in meters
 * @return Unique pointer to telescope driver
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_with_site(
    int device_number,
    const ConnectionInfo& connection_info,
    std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect);

/**
 * @brief Create a driver whose endpoint is resolved at connect time.
 *
 * The resolver runs inside set_connected(true) and returns the endpoint to
 * open, or throws the refusal the client should see. The auto-detect
 * factories below are thin wrappers over it (#659); tests inject a resolver
 * that returns a fake's endpoint or throws.
 */
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg = std::nullopt, std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt, std::optional<bool> sync_time_on_connect = std::nullopt);

/// The serial scan behind create_ioptron_telescope_auto(); throws when nothing answers.
ConnectionInfo resolve_ioptron_serial_auto(int mount_index);
/// The network sweep behind create_ioptron_telescope_auto_network(); throws when nothing answers.
ConnectionInfo resolve_ioptron_network_auto(int mount_index);

// Auto-detect (serial). The scan runs at connect time, so construction
// succeeds even while the mount is absent (#659).
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto(
    int device_number,
    int mount_index = 0,
    std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt,
    std::optional<bool> sync_time_on_connect = std::nullopt);

// Auto-detect (Wi-Fi). The subnet sweep runs at connect time (#659).
std::unique_ptr<TelescopeDriver> create_ioptron_telescope_auto_network(
    int device_number,
    int mount_index = 0,
    std::optional<double> site_latitude_deg = std::nullopt,
    std::optional<double> site_longitude_deg = std::nullopt,
    std::optional<double> site_elevation_m = std::nullopt,
    std::optional<bool> sync_time_on_connect = std::nullopt);

} // namespace alpacacore::vendor::ioptron

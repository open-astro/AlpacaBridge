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

namespace alpacacore::units {

/**
 * @brief Unit conversion utilities.
 *
 * AlpacaCore uses the following units:
 * - Exposure: seconds
 * - Angles: degrees
 * - RA: hours
 * - Dec: degrees
 * - Pixel size: microns
 * - Time: UTC, std::chrono
 * - Wavelengths: nm
 */

/**
 * @brief Convert degrees to radians.
 */
constexpr double deg_to_rad(double degrees) {
    return degrees * (3.14159265358979323846 / 180.0);
}

/**
 * @brief Convert radians to degrees.
 */
constexpr double rad_to_deg(double radians) {
    return radians * (180.0 / 3.14159265358979323846);
}

/**
 * @brief Convert hours to degrees (for RA).
 */
constexpr double hours_to_deg(double hours) {
    return hours * 15.0;
}

/**
 * @brief Convert degrees to hours (for RA).
 */
constexpr double deg_to_hours(double degrees) {
    return degrees / 15.0;
}

/**
 * @brief Convert microns to meters.
 */
constexpr double microns_to_meters(double microns) {
    return microns * 1e-6;
}

/**
 * @brief Convert meters to microns.
 */
constexpr double meters_to_microns(double meters) {
    return meters * 1e6;
}

} // namespace alpacacore::units


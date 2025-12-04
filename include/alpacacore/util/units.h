// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaCore/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

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


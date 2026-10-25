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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>

#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

namespace alpacacore::util {

/**
 * @file
 * @brief Pointing arithmetic every mount driver shares: sidereal time, angle
 *        wraps, the shortest RA delta, the pier-side rule and the RA/Dec and
 *        site range checks.
 *
 * Free functions, no class, no clock, no I/O, no virtuals (decision 0013 s5).
 * The caller passes the UTC instant and the longitude it already holds; which
 * UTC feeds a driver is decision 0001's business, not this header's.
 * Sexagesimal wire formatting stays in util/sexagesimal.h.
 *
 * The validators throw AlpacaException with AlpacaError::InvalidValue and one
 * wording for every driver, so a client sees the same text from every mount.
 */

constexpr double kPointingHoursToDegrees = 15.0;

/// Wrap into [0, 24).
inline double wrap_hours(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }
    // A tiny negative remainder plus 24.0 rounds to exactly 24.0.
    if (wrapped >= 24.0) {
        wrapped -= 24.0;
    }
    return wrapped;
}

/// Wrap into [0, 360).
inline double wrap_degrees(double degrees) {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    // A tiny negative remainder plus 360.0 rounds to exactly 360.0.
    if (wrapped >= 360.0) {
        wrapped -= 360.0;
    }
    return wrapped;
}

/// Wrap an hour angle into [-12, 12).
inline double wrap_hour_angle(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < -12.0) {
        wrapped += 24.0;
    }
    if (wrapped >= 12.0) {
        wrapped -= 24.0;
    }
    return wrapped;
}

/// `target - current` wrapped into [-12, 12] hours: the short way round.
inline double shortest_ra_delta_hours(double target_hours, double current_hours) {
    double delta = target_hours - current_hours;
    while (delta > 12.0) {
        delta -= 24.0;
    }
    while (delta < -12.0) {
        delta += 24.0;
    }
    return delta;
}

/// Greenwich mean sidereal time in [0, 360) degrees (Meeus), from the UTC instant.
/// Sub-second resolution matters: whole-second truncation stepped LST (and so RA) in 15 arcsec jumps (#1298).
inline double gmst_degrees(std::chrono::system_clock::time_point utc) {
    const double days_since_epoch = std::chrono::duration<double>(utc.time_since_epoch()).count() / 86400.0;
    const double jd = 2440587.5 + days_since_epoch;
    const double t = (jd - 2451545.0) / 36525.0;
    const double gmst =
        280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - (t * t * t) / 38710000.0;
    return wrap_degrees(gmst);
}

/// Local mean sidereal time in [0, 24) hours for an east-positive longitude.
inline double local_sidereal_hours(std::chrono::system_clock::time_point utc, double longitude_degrees) {
    return wrap_degrees(gmst_degrees(utc) + longitude_degrees) / kPointingHoursToDegrees;
}

/// Destination pier side from the hour angle: 0 (east) when `ha_hours >= 0`, else 1 (west).
constexpr int pier_side_for_hour_angle(double ha_hours) { return ha_hours >= 0.0 ? 0 : 1; }

/// Throws InvalidValue unless `hours` is finite and in [0, 24).
inline void validate_ra(double hours, std::string_view label) {
    if (!std::isfinite(hours) || hours < 0.0 || hours >= 24.0) {
        throw AlpacaException(std::string(label) + " must be in range 0 to <24 hours", AlpacaError::InvalidValue);
    }
}

/// Throws InvalidValue unless `degrees` is finite and in [-90, 90].
inline void validate_dec(double degrees, std::string_view label) {
    if (!std::isfinite(degrees) || degrees < -90.0 || degrees > 90.0) {
        throw AlpacaException(std::string(label) + " must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
    }
}

/// RA first, then Dec; `label` names the operation ("SlewToCoordinates").
inline void validate_ra_dec(double ra_hours, double dec_degrees, std::string_view label) {
    validate_ra(ra_hours, std::string(label) + ": RightAscension");
    validate_dec(dec_degrees, std::string(label) + ": Declination");
}

inline void validate_latitude(double degrees) {
    if (!std::isfinite(degrees) || degrees < -90.0 || degrees > 90.0) {
        throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
    }
}

inline void validate_longitude(double degrees) {
    if (!std::isfinite(degrees) || degrees < -180.0 || degrees > 180.0) {
        throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees", AlpacaError::InvalidValue);
    }
}

/// Latitude first, then longitude.
inline void validate_site(double latitude_degrees, double longitude_degrees) {
    validate_latitude(latitude_degrees);
    validate_longitude(longitude_degrees);
}

}  // namespace alpacacore::util

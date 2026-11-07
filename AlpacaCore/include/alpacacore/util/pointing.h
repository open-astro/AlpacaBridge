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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

namespace alpacacore::util {

/**
 * @file
 * @brief Pointing arithmetic every mount driver shares: sidereal time, angle
 *        wraps, the shortest RA delta, the pier-side rules (the hour-angle
 *        rule and the German equatorial model of a controller that keeps its
 *        side past the meridian) and the RA/Dec and site range checks.
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

// ── German equatorial pier-side model ───────────────────────────────────────────────────────────────────────
// What a GEM controller does when it picks the pier side for a GOTO, for the controllers that keep their
// current side past the meridian (OnStepX "Best": it stays on the side it is on while that side can reach the
// target inside its limits, #2179). Vendor-neutral: the caller reads the controller's settings and passes them.
// Angles are degrees, hour angles hours; "instrument axes" are the mount's two axes in the controller's own frame.

/// Which side the controller prefers for a GOTO.
enum class GemPierPreference { East, West, Best, Auto };

/// How far each side can point: the meridian limits and the RA-axis (axis 1) limits.
struct GemPierLimits {
    double east_limit_hours;   ///< The EAST side reaches hour angles >= -east_limit_hours (past the meridian).
    double west_limit_hours;   ///< The WEST side reaches hour angles <= +west_limit_hours.
    double axis1_min_degrees;  ///< RA-axis minimum (the instrument's axis 1 is the hour angle on the east side).
    double axis1_max_degrees;  ///< RA-axis maximum.
};

struct GemPierModel {
    GemPierPreference preference;
    bool flips_enabled;  ///< False: the controller stays on the east side.
    GemPierLimits limits;
    bool southern;  ///< Site latitude below zero.
};

/// Where the mount points now. `side`: 0 east, 1 west, -1 none (taken as east). `at_home`: at the home position.
struct GemPointing {
    int side;
    bool at_home;
    double ha_hours;
    double dec_degrees;
};

namespace detail {

constexpr double kGemAxisEpsilon = 1e-9;

struct GemAxes {
    double axis1;
    double axis2;
};

/// The instrument axes for `side` (1 west, anything else east) at an hour angle and declination.
inline GemAxes gem_axes(int side, double ha_hours, double dec_degrees, bool southern) {
    const double ha_degrees = wrap_hour_angle(ha_hours) * kPointingHoursToDegrees;
    if (side == 1) {
        return {ha_degrees + 180.0, (southern ? -180.0 : 180.0) - dec_degrees};
    }
    return {ha_degrees, dec_degrees};
}

/// The axis-1 reading of `side` at the target that lies inside the side's range, shifted by 0 or +/-360 degrees;
/// of the readings that do, the one nearest `near_axis1`. False when none does.
inline bool gem_reachable_axis1(const GemPierLimits& limits, int side, double axis1, double near_axis1, double& out) {
    double low = 0.0;
    double high = 0.0;
    if (side == 1) {
        low = limits.axis1_min_degrees + 180.0;
        high = std::min(limits.axis1_max_degrees, limits.west_limit_hours * kPointingHoursToDegrees) + 180.0;
    } else {
        low = std::max(limits.axis1_min_degrees, -limits.east_limit_hours * kPointingHoursToDegrees);
        high = limits.axis1_max_degrees;
    }
    bool found = false;
    for (const double shift : {-360.0, 0.0, 360.0}) {
        const double candidate = axis1 + shift;
        if (candidate < low - kGemAxisEpsilon || candidate > high + kGemAxisEpsilon) {
            continue;
        }
        if (!found || std::fabs(candidate - near_axis1) < std::fabs(out - near_axis1)) {
            out = candidate;
            found = true;
        }
    }
    return found;
}

}  // namespace detail

/// True when the side (0 east, 1 west) can point at the hour angle inside the meridian and RA-axis limits.
inline bool gem_pier_side_reaches(const GemPierLimits& limits, int side, double target_ha_hours) {
    const double axis1 = detail::gem_axes(side, target_ha_hours, 0.0, false).axis1;
    double reached = 0.0;
    return detail::gem_reachable_axis1(limits, side, axis1, axis1, reached);
}

/// The pier side (0 east, 1 west) the controller picks for a GOTO to the target, or -1 when no side reaches it
/// (the controller refuses the GOTO). Mirrors OnStepX `Goto::setTarget` and `validateTarget`:
///  - flips disabled: east only;
///  - at home the side follows the target hour angle (west for negative), with "best" on whatever the preference;
///  - Best starts from the current side, East/West from that side, Auto from west past 6 h of hour angle either way;
///  - the other side is taken when only it reaches, or, for Best, when it needs strictly less axis travel.
inline int gem_destination_pier_side(const GemPierModel& model, const GemPointing& current, double target_ha_hours,
                                     double target_dec_degrees) {
    const double target_ha = wrap_hour_angle(target_ha_hours);
    const int current_side = current.side == 1 ? 1 : 0;
    detail::GemAxes now{};
    if (current.at_home) {
        now = {90.0, model.southern ? -90.0 : 90.0};
    } else {
        now = detail::gem_axes(current_side, current.ha_hours, current.dec_degrees, model.southern);
    }

    // Reach and distance for one side.
    struct Option {
        bool reaches = false;
        double distance = 0.0;
    };
    const auto evaluate = [&](int side) {
        const detail::GemAxes axes = detail::gem_axes(side, target_ha, target_dec_degrees, model.southern);
        Option option;
        double axis1 = 0.0;
        option.reaches = detail::gem_reachable_axis1(model.limits, side, axes.axis1, now.axis1, axis1);
        if (option.reaches) {
            option.distance = std::max(std::fabs(axis1 - now.axis1), std::fabs(axes.axis2 - now.axis2));
        }
        return option;
    };

    if (!model.flips_enabled) {
        return evaluate(0).reaches ? 0 : -1;
    }

    int selected = 0;
    bool best = false;
    if (current.at_home) {
        selected = target_ha < 0.0 ? 1 : 0;
        best = true;
    } else {
        switch (model.preference) {
            case GemPierPreference::Best:
                selected = current_side;
                best = true;
                break;
            case GemPierPreference::East:
                selected = 0;
                break;
            case GemPierPreference::West:
                selected = 1;
                break;
            case GemPierPreference::Auto:
                selected = std::fabs(wrap_hour_angle(current.ha_hours)) > 6.0 ? 1 : 0;
                break;
        }
    }
    const int other = 1 - selected;
    const Option chosen = evaluate(selected);
    const Option alternative = evaluate(other);
    if (alternative.reaches && !chosen.reaches) {
        return other;
    }
    if (best && alternative.reaches && alternative.distance < chosen.distance) {
        return other;
    }
    return chosen.reaches ? selected : -1;
}

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

/// Great-circle separation in arcseconds between two points given as longitude-like and latitude-like degrees
/// (RA in degrees and Dec, or azimuth and altitude).
inline double angular_separation_arcsec(double lon1_deg, double lat1_deg, double lon2_deg, double lat2_deg) {
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double dlat = (lat2_deg - lat1_deg) * kDegToRad;
    const double dlon = (lon2_deg - lon1_deg) * kDegToRad;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) + std::cos(lat1_deg * kDegToRad) *
                                                                   std::cos(lat2_deg * kDegToRad) * std::sin(dlon / 2) *
                                                                   std::sin(dlon / 2);
    return 2.0 * std::asin(std::min(1.0, std::sqrt(a))) / kDegToRad * 3600.0;
}

/// Latitude first, then longitude.
inline void validate_site(double latitude_degrees, double longitude_degrees) {
    validate_latitude(latitude_degrees);
    validate_longitude(longitude_degrees);
}

}  // namespace alpacacore::util

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

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace alpacacore::util {

// Per-device motion limits (open-astro#436). Vendor-neutral and pure: no I/O,
// no clock, no driver state. The axis-to-alt/az conversion stays in the
// driver; this type only answers "is this allowed?" and "did this cross?".
//
// Both limits are OFF by default (a disengaged optional), so a default
// MotionLimits changes no behaviour: existing installs and ConformU runs (which
// slew to arbitrary targets by design) see no refusal.
struct MotionLimits {
    // Minimum altitude in degrees above the horizon. A goto whose target sits
    // below it is refused with InvalidValue; the live guard (436b) stops motion
    // that carries the tube from at-or-above to below it.
    std::optional<double> min_altitude_deg;

    // Meridian limit in minutes of hour angle past the meridian, measured on
    // the RA axis as the counterweight-up angle (|a1| - 90 deg, 4 min per
    // deg), so pointing-model error does not move it. Used by the live guard
    // only: #432 already keeps every goto target on the counterweight-down
    // side, so check_target() never consults it.
    std::optional<double> meridian_limit_minutes;

    bool altitude_limit_enabled() const { return min_altitude_deg.has_value(); }
    bool meridian_limit_enabled() const { return meridian_limit_minutes.has_value(); }
    bool enabled() const { return altitude_limit_enabled() || meridian_limit_enabled(); }

    // The floor at a given azimuth: a constant for now, but the azimuth
    // argument is part of the contract so a horizon profile can replace the
    // constant later without touching a call site. Disengaged when the
    // altitude limit is off.
    std::optional<double> min_altitude(double azimuth_deg) const {
        (void)azimuth_deg;
        return min_altitude_deg;
    }
};

// One sample of where the tube points, in the terms the limits are stated in.
struct MotionSample {
    double altitude_deg = 0.0;
    double azimuth_deg = 0.0;
    // |a1| - 90 deg: positive when the counterweight is above horizontal.
    double counterweight_up_deg = 0.0;
};

enum class LimitCrossing { None, AltitudeFloor, Meridian };

// Goto target check. Returns a disengaged optional when the target is allowed
// (limits off, or altitude at or above the floor), otherwise the operator-
// facing refusal: "target altitude 12.3 deg is below the Minimum altitude
// limit (15 deg) set in the device settings". The altitude is printed with one
// decimal, the limit with at most one and no trailing ".0", so a
// setting of 15 reads "(15 deg)" and 12.5 reads "(12.5 deg)".
inline std::optional<std::string> check_target(const MotionLimits& limits, double altitude_deg, double azimuth_deg) {
    const std::optional<double> floor = limits.min_altitude(azimuth_deg);
    if (!floor || !(altitude_deg < *floor)) return std::nullopt;

    // Ties round away from zero (printf alone rounds -5.25 to -5.2).
    const auto one_decimal = [](double v) { return std::round(v * 10.0) / 10.0; };
    char altitude[32];
    std::snprintf(altitude, sizeof altitude, "%.1f", one_decimal(altitude_deg));
    char limit[32];
    std::snprintf(limit, sizeof limit, "%.1f", one_decimal(*floor));
    std::string limit_text = limit;
    if (limit_text.size() > 2 && limit_text.compare(limit_text.size() - 2, 2, ".0") == 0) {
        limit_text.resize(limit_text.size() - 2);
    }
    return "target altitude " + std::string(altitude) + " deg is below the Minimum altitude limit (" + limit_text +
           " deg) set in the device settings";
}

// Live-guard decision (436b), edge-triggered: only a move from inside a limit
// to outside it counts, so motion that starts outside a limit is never
// stopped and a mount below the floor can always be driven back up. The
// altitude floor is reported before the meridian when both cross in one step.
inline LimitCrossing crossed(const MotionLimits& limits, const MotionSample& before, const MotionSample& after) {
    if (const auto floor = limits.min_altitude(after.azimuth_deg)) {
        if (before.altitude_deg >= *floor && after.altitude_deg < *floor) return LimitCrossing::AltitudeFloor;
    }
    if (limits.meridian_limit_minutes) {
        constexpr double kMinutesPerDegree = 4.0;  // 15 deg per hour of hour angle
        const double limit = *limits.meridian_limit_minutes;
        if (before.counterweight_up_deg * kMinutesPerDegree <= limit &&
            after.counterweight_up_deg * kMinutesPerDegree > limit) {
            return LimitCrossing::Meridian;
        }
    }
    return LimitCrossing::None;
}

}  // namespace alpacacore::util

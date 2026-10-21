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

#include <chrono>
#include <cstdint>
#include <utility>

namespace alpacacore::vendor::celestron {

inline constexpr std::uint32_t kCgxEncoderStepsPerRevolution = 1U << 24;

/// Encode degrees as a wrapped 24-bit fraction of one encoder revolution.
std::uint32_t encode_cgx_encoder_degrees(double degrees);

/// Return the shortest signed count difference in [-half-turn, half-turn).
std::int32_t signed_cgx_encoder_delta(std::uint32_t position, std::uint32_t reference);

/// Compute local apparent sidereal time from UTC and east-positive longitude.
double cgx_local_sidereal_time_hours(std::chrono::system_clock::time_point utc, double longitude_degrees);

enum class CgxHemisphere : std::uint8_t { North, South };
enum class CgxPointingSide : std::uint8_t { Normal, BeyondPole };

struct CgxSkyPosition {
    double right_ascension_hours = 0.0;
    double declination_degrees = 0.0;
    CgxPointingSide pointing_side = CgxPointingSide::Normal;
};

/// Convert CGX/CGX-L RA/Dec motor counts to a sky position after FindHome.
CgxSkyPosition cgx_encoder_position_to_sky(std::uint32_t ra_encoder, std::uint32_t dec_encoder,
                                           double local_sidereal_time_hours, CgxHemisphere hemisphere,
                                           std::uint32_t ra_home_encoder = kCgxEncoderStepsPerRevolution / 4,
                                           std::uint32_t dec_home_encoder = kCgxEncoderStepsPerRevolution / 2);

/// Convert a target to motor counts on the explicitly selected pointing branch.
std::pair<std::uint32_t, std::uint32_t> cgx_sky_position_to_encoders(
    double right_ascension_hours, double declination_degrees, double local_sidereal_time_hours,
    CgxHemisphere hemisphere, CgxPointingSide pointing_side,
    std::uint32_t ra_home_encoder = kCgxEncoderStepsPerRevolution / 4,
    std::uint32_t dec_home_encoder = kCgxEncoderStepsPerRevolution / 2);

}  // namespace alpacacore::vendor::celestron

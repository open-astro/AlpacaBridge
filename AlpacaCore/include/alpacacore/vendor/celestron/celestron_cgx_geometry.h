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
#include <optional>
#include <string>
#include <utility>

namespace alpacacore::vendor::celestron {

inline constexpr std::uint32_t kCgxEncoderStepsPerRevolution = 1U << 24;

/// The labels FindHome writes at the home pose (counterweight down, pointing at the pole): a quarter turn on both
/// axes (#2640). The controller's GOTO never crosses label 0xC00000, the half turn opposite these: a GOTO whose
/// shorter way crosses it goes the long way round. With Dec home at the half turn, that label was sky Dec 0 on the
/// Normal side, so no GOTO there crossed the equator (rig 2026-11-24: 4 of 4 crossings went the long way, in both
/// directions and from two physical angles). With Dec home at a quarter turn, the label is half a turn from home
/// either way round, the Dec axis pointing at Dec -90, which neither side of the pole reaches.
inline constexpr std::uint32_t kCgxRaHomeEncoder = kCgxEncoderStepsPerRevolution / 4;
inline constexpr std::uint32_t kCgxDecHomeEncoder = kCgxEncoderStepsPerRevolution / 4;

/// Dec counts either side of the home label that still read as the home pose, the Normal side (#2640). The pointing
/// side flips at the Dec home, so a Dec resting one count short of it read BeyondPole, with RA 12 h off, on a mount
/// standing at home (rig 2026-11-23: Dec 89.99997854 deg, SideOfPier 1, after a FindHome). Four counts (0.3") is
/// twice the two-count label tolerance; a pose this close to the pole is the same sky point on either side.
inline constexpr std::int32_t kCgxHomeSideBandCounts = 4;

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
                                           std::uint32_t ra_home_encoder = kCgxRaHomeEncoder,
                                           std::uint32_t dec_home_encoder = kCgxDecHomeEncoder);

/// Convert a target to motor counts on the explicitly selected pointing branch.
std::pair<std::uint32_t, std::uint32_t> cgx_sky_position_to_encoders(
    double right_ascension_hours, double declination_degrees, double local_sidereal_time_hours,
    CgxHemisphere hemisphere, CgxPointingSide pointing_side, std::uint32_t ra_home_encoder = kCgxRaHomeEncoder,
    std::uint32_t dec_home_encoder = kCgxDecHomeEncoder);

/// The pre-GOTO pose guard (#2640): why an encoder target would put the telescope somewhere unsafe, or nullopt. It
/// decodes the counts again on `pointing_side`, independent of how they were computed: the Dec axis must stay on that
/// side of the pole (within kCgxHomeSideBandCounts), the RA axis within six hours of its counterweight-down home (the
/// meridian limit of a side), and the pose at or above `minimum_altitude_degrees` at `latitude_degrees`. The pose
/// does not depend on the time: the hour angle comes from the RA axis.
std::optional<std::string> cgx_encoder_target_refusal(std::uint32_t ra_encoder, std::uint32_t dec_encoder,
                                                      CgxHemisphere hemisphere, CgxPointingSide pointing_side,
                                                      double latitude_degrees, double minimum_altitude_degrees = 0.0,
                                                      std::uint32_t ra_home_encoder = kCgxRaHomeEncoder,
                                                      std::uint32_t dec_home_encoder = kCgxDecHomeEncoder);

}  // namespace alpacacore::vendor::celestron

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

#include <alpacacore/util/pointing.h>
#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace alpacacore::vendor::celestron {

namespace {

std::uint32_t wrap_encoder_count(std::int64_t count) {
    constexpr auto revolution = static_cast<std::int64_t>(kCgxEncoderStepsPerRevolution);
    count %= revolution;
    if (count < 0) count += revolution;
    return static_cast<std::uint32_t>(count);
}

double hemisphere_sign(CgxHemisphere hemisphere) {
    switch (hemisphere) {
        case CgxHemisphere::North:
            return 1.0;
        case CgxHemisphere::South:
            return -1.0;
    }
    throw std::invalid_argument("Celestron CGX hemisphere is invalid");
}

double equation_of_equinoxes_arcseconds(double centuries) {
    const double mean_sun = std::remainder(280.4665 + 36000.7698 * centuries, 360.0) * std::numbers::pi / 180.0;
    const double mean_moon = std::remainder(218.3165 + 481267.8813 * centuries, 360.0) * std::numbers::pi / 180.0;
    const double node_degrees = 125.04452 - 1934.136261 * centuries + 0.0020708 * centuries * centuries +
                                centuries * centuries * centuries / 450000.0;
    const double node = std::remainder(node_degrees, 360.0) * std::numbers::pi / 180.0;
    const double nutation_longitude_arcseconds = -17.20 * std::sin(node) - 1.32 * std::sin(2.0 * mean_sun) -
                                                 0.23 * std::sin(2.0 * mean_moon) + 0.21 * std::sin(2.0 * node);
    const double mean_obliquity = (23.439291 - 0.0130042 * centuries) * std::numbers::pi / 180.0;
    return nutation_longitude_arcseconds * std::cos(mean_obliquity);
}

}  // namespace

std::uint32_t encode_cgx_encoder_degrees(double degrees) {
    if (!std::isfinite(degrees)) {
        throw std::invalid_argument("Celestron CGX encoder angle must be finite");
    }
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    const auto count =
        static_cast<std::uint32_t>(std::round(wrapped * static_cast<double>(kCgxEncoderStepsPerRevolution) / 360.0));
    return count == kCgxEncoderStepsPerRevolution ? 0U : count;
}

std::int32_t signed_cgx_encoder_delta(std::uint32_t position, std::uint32_t reference) {
    if (position >= kCgxEncoderStepsPerRevolution || reference >= kCgxEncoderStepsPerRevolution) {
        throw std::invalid_argument("Celestron CGX encoder count is outside 24-bit range");
    }
    std::int32_t delta = static_cast<std::int32_t>(position) - static_cast<std::int32_t>(reference);
    constexpr auto half_turn = static_cast<std::int32_t>(kCgxEncoderStepsPerRevolution / 2);
    if (delta >= half_turn) {
        delta -= static_cast<std::int32_t>(kCgxEncoderStepsPerRevolution);
    }
    if (delta < -half_turn) {
        delta += static_cast<std::int32_t>(kCgxEncoderStepsPerRevolution);
    }
    return delta;
}

double cgx_local_sidereal_time_hours(std::chrono::system_clock::time_point utc, double longitude_degrees) {
    if (!std::isfinite(longitude_degrees) || longitude_degrees < -180.0 || longitude_degrees > 180.0) {
        throw std::invalid_argument("Celestron CGX longitude must be finite and in [-180, 180]");
    }
    const double seconds = std::chrono::duration<double>(utc.time_since_epoch()).count();
    const double centuries = (2440587.5 + seconds / 86400.0 - 2451545.0) / 36525.0;
    const double gast_degrees = util::gmst_degrees(utc) + equation_of_equinoxes_arcseconds(centuries) / 3600.0;
    const double lst_degrees = util::wrap_degrees(gast_degrees + longitude_degrees);
    return lst_degrees / 15.0;
}

CgxSkyPosition cgx_encoder_position_to_sky(std::uint32_t ra_encoder, std::uint32_t dec_encoder,
                                           double local_sidereal_time_hours, CgxHemisphere hemisphere,
                                           std::uint32_t ra_home_encoder, std::uint32_t dec_home_encoder) {
    // The quarter-turn RA and half-turn Dec home references match the
    // reference driver's northern model. Southern geometry is its polar
    // mirror; confirm encoder direction and switch orientation on a southern
    // CGX before claiming hardware support there.
    if (!std::isfinite(local_sidereal_time_hours)) {
        throw std::invalid_argument("Celestron CGX local sidereal time must be finite");
    }
    const double h = hemisphere_sign(hemisphere);
    const double ra_axis_hours = h * static_cast<double>(signed_cgx_encoder_delta(ra_encoder, ra_home_encoder)) * 24.0 /
                                 static_cast<double>(kCgxEncoderStepsPerRevolution);
    const auto dec_axis_counts = signed_cgx_encoder_delta(dec_encoder, dec_home_encoder);
    const double dec_axis_degrees =
        static_cast<double>(dec_axis_counts) * 360.0 / static_cast<double>(kCgxEncoderStepsPerRevolution);
    const CgxPointingSide side = dec_axis_counts < 0 ? CgxPointingSide::BeyondPole : CgxPointingSide::Normal;
    const double hour_angle = 6.0 + ra_axis_hours + (side == CgxPointingSide::BeyondPole ? 12.0 : 0.0);
    const double declination = h * (90.0 - std::abs(dec_axis_degrees));
    return {util::wrap_hours(local_sidereal_time_hours - hour_angle), declination, side};
}

std::pair<std::uint32_t, std::uint32_t> cgx_sky_position_to_encoders(
    double right_ascension_hours, double declination_degrees, double local_sidereal_time_hours,
    CgxHemisphere hemisphere, CgxPointingSide pointing_side, std::uint32_t ra_home_encoder,
    std::uint32_t dec_home_encoder) {
    if (!std::isfinite(right_ascension_hours) || right_ascension_hours < 0.0 || right_ascension_hours >= 24.0 ||
        !std::isfinite(declination_degrees) || declination_degrees < -90.0 || declination_degrees > 90.0 ||
        !std::isfinite(local_sidereal_time_hours)) {
        throw std::invalid_argument("Celestron CGX sky coordinate or local sidereal time is invalid");
    }
    const double h = hemisphere_sign(hemisphere);
    const double sky_hour_angle = util::wrap_hour_angle(local_sidereal_time_hours - right_ascension_hours);
    const double mechanical_hour_angle = sky_hour_angle - (pointing_side == CgxPointingSide::BeyondPole ? 12.0 : 0.0);
    const double ra_delta_hours = util::wrap_hour_angle(mechanical_hour_angle - 6.0);
    const auto ra_delta_counts = static_cast<std::int64_t>(
        std::llround(h * ra_delta_hours * static_cast<double>(kCgxEncoderStepsPerRevolution) / 24.0));

    const double dec_delta_degrees = 90.0 - h * declination_degrees;
    const double dec_direction = pointing_side == CgxPointingSide::BeyondPole ? -1.0 : 1.0;
    const auto dec_delta_counts = static_cast<std::int64_t>(
        std::llround(dec_direction * dec_delta_degrees * static_cast<double>(kCgxEncoderStepsPerRevolution) / 360.0));

    return {wrap_encoder_count(static_cast<std::int64_t>(ra_home_encoder) + ra_delta_counts),
            wrap_encoder_count(static_cast<std::int64_t>(dec_home_encoder) + dec_delta_counts)};
}

}  // namespace alpacacore::vendor::celestron

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

#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "catch2_compat.h"

using namespace std::chrono_literals;

TEST_CASE("Celestron CGX geometry - encodes 24-bit encoder fractions", "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::encode_cgx_encoder_degrees;

    CHECK(encode_cgx_encoder_degrees(0.0) == 0U);
    CHECK(encode_cgx_encoder_degrees(90.0) == 0x400000U);
    CHECK(encode_cgx_encoder_degrees(180.0) == 0x800000U);
    CHECK(encode_cgx_encoder_degrees(270.0) == 0xc00000U);
    CHECK(encode_cgx_encoder_degrees(360.0) == 0U);
    CHECK(encode_cgx_encoder_degrees(-90.0) == 0xc00000U);
}

TEST_CASE("Celestron CGX geometry - chooses shortest signed encoder delta across wrap",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::signed_cgx_encoder_delta;

    CHECK(signed_cgx_encoder_delta(0x000000U, 0xffffffU) == 1);
    CHECK(signed_cgx_encoder_delta(0xffffffU, 0x000000U) == -1);
    CHECK(signed_cgx_encoder_delta(0x800000U, 0x000000U) == -0x800000);
}

TEST_CASE("Celestron CGX geometry - computes local apparent sidereal time east and west of Greenwich",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_local_sidereal_time_hours;
    const auto j2000 = std::chrono::system_clock::time_point{std::chrono::seconds{946728000}};

    ALPACA_REQUIRE_APPROX(cgx_local_sidereal_time_hours(j2000, 0.0), 18.69713616);
    ALPACA_REQUIRE_APPROX(cgx_local_sidereal_time_hours(j2000, 90.0), 0.69713616);
    ALPACA_REQUIRE_APPROX(cgx_local_sidereal_time_hours(j2000, -90.0), 12.69713616);
}

TEST_CASE("Celestron CGX geometry - maps northern normal and beyond-pole encoder states",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_encoder_position_to_sky;
    using alpacacore::vendor::celestron::cgx_sky_position_to_encoders;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::CgxPointingSide;

    const auto normal = cgx_sky_position_to_encoders(6.0, 30.0, 6.0, CgxHemisphere::North, CgxPointingSide::Normal);
    CHECK(normal.first == 0x000000U);
    CHECK(normal.second == 0xaaaaabU);
    const auto normal_sky = cgx_encoder_position_to_sky(normal.first, normal.second, 6.0, CgxHemisphere::North);
    ALPACA_REQUIRE_APPROX(normal_sky.right_ascension_hours, 6.0);
    CHECK(std::abs(normal_sky.declination_degrees - 30.0) < 2.2e-5);
    CHECK(normal_sky.pointing_side == CgxPointingSide::Normal);

    const auto beyond = cgx_sky_position_to_encoders(6.0, 30.0, 6.0, CgxHemisphere::North, CgxPointingSide::BeyondPole);
    CHECK(beyond.first == 0x800000U);
    CHECK(beyond.second == 0x555555U);
    const auto beyond_sky = cgx_encoder_position_to_sky(beyond.first, beyond.second, 6.0, CgxHemisphere::North);
    ALPACA_REQUIRE_APPROX(beyond_sky.right_ascension_hours, 6.0);
    CHECK(std::abs(beyond_sky.declination_degrees - 30.0) < 2.2e-5);
    CHECK(beyond_sky.pointing_side == CgxPointingSide::BeyondPole);
}

TEST_CASE("Celestron CGX geometry - mirrors the polar reference in the southern hemisphere",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_encoder_position_to_sky;
    using alpacacore::vendor::celestron::cgx_sky_position_to_encoders;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::CgxPointingSide;

    const auto normal = cgx_sky_position_to_encoders(6.0, -30.0, 6.0, CgxHemisphere::South, CgxPointingSide::Normal);
    CHECK(normal.first == 0x800000U);
    CHECK(normal.second == 0xaaaaabU);
    const auto normal_sky = cgx_encoder_position_to_sky(normal.first, normal.second, 6.0, CgxHemisphere::South);
    ALPACA_REQUIRE_APPROX(normal_sky.right_ascension_hours, 6.0);
    CHECK(std::abs(normal_sky.declination_degrees + 30.0) < 2.2e-5);
    CHECK(normal_sky.pointing_side == CgxPointingSide::Normal);

    const auto beyond =
        cgx_sky_position_to_encoders(6.0, -30.0, 6.0, CgxHemisphere::South, CgxPointingSide::BeyondPole);
    CHECK(beyond.first == 0x000000U);
    CHECK(beyond.second == 0x555555U);
    const auto beyond_sky = cgx_encoder_position_to_sky(beyond.first, beyond.second, 6.0, CgxHemisphere::South);
    ALPACA_REQUIRE_APPROX(beyond_sky.right_ascension_hours, 6.0);
    CHECK(std::abs(beyond_sky.declination_degrees + 30.0) < 2.2e-5);
    CHECK(beyond_sky.pointing_side == CgxPointingSide::BeyondPole);
}

TEST_CASE("Celestron CGX geometry - encoder tracking sign keeps RA fixed in both hemispheres",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_encoder_position_to_sky;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::kCgxEncoderStepsPerRevolution;

    constexpr auto home = kCgxEncoderStepsPerRevolution / 4;
    constexpr double lst_before = 8.0;
    constexpr double elapsed_hours = 0.01;
    const auto encoder_delta = static_cast<std::uint32_t>(
        std::llround(elapsed_hours * static_cast<double>(kCgxEncoderStepsPerRevolution) / 24.0));
    const auto north_before =
        cgx_encoder_position_to_sky(home, kCgxEncoderStepsPerRevolution / 2, lst_before, CgxHemisphere::North);
    const auto north_after = cgx_encoder_position_to_sky(home + encoder_delta, kCgxEncoderStepsPerRevolution / 2,
                                                         lst_before + elapsed_hours, CgxHemisphere::North);
    const auto south_before =
        cgx_encoder_position_to_sky(home, kCgxEncoderStepsPerRevolution / 2, lst_before, CgxHemisphere::South);
    const auto south_after = cgx_encoder_position_to_sky(home - encoder_delta, kCgxEncoderStepsPerRevolution / 2,
                                                         lst_before + elapsed_hours, CgxHemisphere::South);

    ALPACA_REQUIRE_APPROX(north_after.right_ascension_hours, north_before.right_ascension_hours);
    ALPACA_REQUIRE_APPROX(south_after.right_ascension_hours, south_before.right_ascension_hours);
}

TEST_CASE("Celestron CGX geometry - pins RA wrap and pole-side boundaries with encoder fixtures",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_encoder_position_to_sky;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::CgxPointingSide;
    using alpacacore::vendor::celestron::kCgxEncoderStepsPerRevolution;

    constexpr std::uint32_t ra_home = kCgxEncoderStepsPerRevolution / 4;
    constexpr std::uint32_t dec_home = kCgxEncoderStepsPerRevolution / 2;
    for (const auto hemisphere : {CgxHemisphere::North, CgxHemisphere::South}) {
        const bool north = hemisphere == CgxHemisphere::North;
        const auto just_before_zero = cgx_encoder_position_to_sky(north ? ra_home + 1 : ra_home - 1, dec_home, 6.0,
                                                                  hemisphere, ra_home, dec_home);
        CHECK(just_before_zero.right_ascension_hours > 23.9999);
        CHECK(just_before_zero.right_ascension_hours < 24.0);
        CHECK(just_before_zero.pointing_side == CgxPointingSide::Normal);

        const auto at_pole = cgx_encoder_position_to_sky(ra_home, dec_home, 6.0, hemisphere, ra_home, dec_home);
        CHECK(at_pole.declination_degrees == (north ? 90.0 : -90.0));
        CHECK(at_pole.pointing_side == CgxPointingSide::Normal);

        const auto across_pole = cgx_encoder_position_to_sky(ra_home, dec_home - 1, 6.0, hemisphere, ra_home, dec_home);
        CHECK(std::abs(across_pole.declination_degrees) < 90.0);
        CHECK(std::abs(across_pole.declination_degrees) > 89.9999);
        CHECK(across_pole.pointing_side == CgxPointingSide::BeyondPole);

        const auto opposite_pole = cgx_encoder_position_to_sky(ra_home, 0, 6.0, hemisphere, ra_home, dec_home);
        CHECK(opposite_pole.declination_degrees == (north ? -90.0 : 90.0));
        CHECK(opposite_pole.right_ascension_hours == 12.0);
    }
}

TEST_CASE("Celestron CGX geometry - rejects invalid sky coordinates and encoder counts",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_encoder_position_to_sky;
    using alpacacore::vendor::celestron::cgx_sky_position_to_encoders;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::CgxPointingSide;

    CHECK_THROWS_AS(cgx_sky_position_to_encoders(24.0, 0.0, 0.0, CgxHemisphere::North, CgxPointingSide::Normal),
                    std::invalid_argument);
    CHECK_THROWS_AS(cgx_sky_position_to_encoders(0.0, 90.1, 0.0, CgxHemisphere::South, CgxPointingSide::BeyondPole),
                    std::invalid_argument);
    CHECK_THROWS_AS(cgx_encoder_position_to_sky(0x1000000U, 0, 0.0, CgxHemisphere::North), std::invalid_argument);
}

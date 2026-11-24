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
#include <string>
#include <tuple>
#include <utility>

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
    CHECK(normal.second == 0x6aaaabU);
    const auto normal_sky = cgx_encoder_position_to_sky(normal.first, normal.second, 6.0, CgxHemisphere::North);
    ALPACA_REQUIRE_APPROX(normal_sky.right_ascension_hours, 6.0);
    CHECK(std::abs(normal_sky.declination_degrees - 30.0) < 2.2e-5);
    CHECK(normal_sky.pointing_side == CgxPointingSide::Normal);

    const auto beyond = cgx_sky_position_to_encoders(6.0, 30.0, 6.0, CgxHemisphere::North, CgxPointingSide::BeyondPole);
    CHECK(beyond.first == 0x800000U);
    CHECK(beyond.second == 0x155555U);
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
    CHECK(normal.second == 0x6aaaabU);
    const auto normal_sky = cgx_encoder_position_to_sky(normal.first, normal.second, 6.0, CgxHemisphere::South);
    ALPACA_REQUIRE_APPROX(normal_sky.right_ascension_hours, 6.0);
    CHECK(std::abs(normal_sky.declination_degrees + 30.0) < 2.2e-5);
    CHECK(normal_sky.pointing_side == CgxPointingSide::Normal);

    const auto beyond =
        cgx_sky_position_to_encoders(6.0, -30.0, 6.0, CgxHemisphere::South, CgxPointingSide::BeyondPole);
    CHECK(beyond.first == 0x000000U);
    CHECK(beyond.second == 0x155555U);
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

    constexpr auto home = alpacacore::vendor::celestron::kCgxRaHomeEncoder;
    constexpr auto dec_home = alpacacore::vendor::celestron::kCgxDecHomeEncoder;
    constexpr double lst_before = 8.0;
    constexpr double elapsed_hours = 0.01;
    const auto encoder_delta = static_cast<std::uint32_t>(
        std::llround(elapsed_hours * static_cast<double>(kCgxEncoderStepsPerRevolution) / 24.0));
    const auto north_before = cgx_encoder_position_to_sky(home, dec_home, lst_before, CgxHemisphere::North);
    const auto north_after =
        cgx_encoder_position_to_sky(home + encoder_delta, dec_home, lst_before + elapsed_hours, CgxHemisphere::North);
    const auto south_before = cgx_encoder_position_to_sky(home, dec_home, lst_before, CgxHemisphere::South);
    const auto south_after =
        cgx_encoder_position_to_sky(home - encoder_delta, dec_home, lst_before + elapsed_hours, CgxHemisphere::South);

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

        // #2640: a few counts short of the Dec home is still the home pose; the side flips past the band.
        const auto inside_band =
            cgx_encoder_position_to_sky(ra_home, dec_home - alpacacore::vendor::celestron::kCgxHomeSideBandCounts, 6.0,
                                        hemisphere, ra_home, dec_home);
        CHECK(inside_band.pointing_side == CgxPointingSide::Normal);
        CHECK(std::abs(inside_band.right_ascension_hours) < 1e-9);

        const auto across_pole =
            cgx_encoder_position_to_sky(ra_home, dec_home - alpacacore::vendor::celestron::kCgxHomeSideBandCounts - 1,
                                        6.0, hemisphere, ra_home, dec_home);
        CHECK(std::abs(across_pole.declination_degrees) < 90.0);
        CHECK(std::abs(across_pole.declination_degrees) > 89.999);
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

TEST_CASE("Celestron CGX geometry - the pose guard refuses targets below the horizon or past a side's limits",
          "[celestron][cgx][geometry][unit]") {
    using alpacacore::vendor::celestron::cgx_encoder_target_refusal;
    using alpacacore::vendor::celestron::cgx_sky_position_to_encoders;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::CgxPointingSide;
    using alpacacore::vendor::celestron::encode_cgx_encoder_degrees;
    using alpacacore::vendor::celestron::kCgxEncoderStepsPerRevolution;
    constexpr std::uint32_t ra_home = alpacacore::vendor::celestron::kCgxRaHomeEncoder;
    constexpr std::uint32_t dec_home = alpacacore::vendor::celestron::kCgxDecHomeEncoder;
    const auto refusal = [](std::pair<std::uint32_t, std::uint32_t> target, CgxHemisphere hemisphere,
                            CgxPointingSide side, double latitude) {
        return cgx_encoder_target_refusal(target.first, target.second, hemisphere, side, latitude);
    };
    // LST 12 h: RA 11.5 is HA +0.5 h (Normal), RA 12.5 HA -0.5 h (BeyondPole).
    for (const auto& [hemisphere, latitude, sign] :
         {std::tuple{CgxHemisphere::North, 30.0, 1.0}, std::tuple{CgxHemisphere::South, -30.0, -1.0}}) {
        INFO("latitude " << latitude);
        for (const auto& [ra, side] :
             {std::pair{11.5, CgxPointingSide::Normal}, std::pair{12.5, CgxPointingSide::BeyondPole}}) {
            // About 49 degrees up: allowed. Dec 70 the wrong way: about 10 degrees below the horizon.
            const auto up = cgx_sky_position_to_encoders(ra, -10.0 * sign, 12.0, hemisphere, side);
            CHECK_FALSE(refusal(up, hemisphere, side, latitude).has_value());
            const auto down = cgx_sky_position_to_encoders(ra, -70.0 * sign, 12.0, hemisphere, side);
            const auto below = refusal(down, hemisphere, side, latitude);
            REQUIRE(below.has_value());
            CHECK(below->find("below the 0.0 deg floor") != std::string::npos);
            // The same counts on the other side are past its pole limit.
            const auto other = side == CgxPointingSide::Normal ? CgxPointingSide::BeyondPole : CgxPointingSide::Normal;
            const auto past_pole = refusal(up, hemisphere, other, latitude);
            REQUIRE(past_pole.has_value());
            CHECK(past_pole->find("past the pole") != std::string::npos);
        }
        // The home pose is allowed on both sides; RA seven hours from home is past the meridian limit.
        CHECK_FALSE(refusal({ra_home, dec_home}, hemisphere, CgxPointingSide::Normal, latitude).has_value());
        CHECK_FALSE(refusal({ra_home, dec_home}, hemisphere, CgxPointingSide::BeyondPole, latitude).has_value());
        const auto past_meridian = refusal({ra_home + encode_cgx_encoder_degrees(105.0), dec_home + 1000}, hemisphere,
                                           CgxPointingSide::Normal, latitude);
        REQUIRE(past_meridian.has_value());
        CHECK(past_meridian->find("meridian") != std::string::npos);
    }
}

TEST_CASE("Celestron CGX geometry - no Dec target on either side reaches the controller's GOTO seam",
          "[celestron][cgx][geometry][unit]") {
    // #2640: the controller's Dec GOTO never crosses label 0xC00000 and goes the long way round instead. For every Dec
    // target from pole to pole, on both sides and in both hemispheres, the shorter way from home must not pass it.
    using alpacacore::vendor::celestron::cgx_sky_position_to_encoders;
    using alpacacore::vendor::celestron::CgxHemisphere;
    using alpacacore::vendor::celestron::CgxPointingSide;
    using alpacacore::vendor::celestron::kCgxDecHomeEncoder;
    using alpacacore::vendor::celestron::signed_cgx_encoder_delta;
    constexpr std::uint32_t kSeam = 0xC00000U;
    const auto seam_from_home = signed_cgx_encoder_delta(kSeam, kCgxDecHomeEncoder);
    for (const auto hemisphere : {CgxHemisphere::North, CgxHemisphere::South}) {
        for (const auto side : {CgxPointingSide::Normal, CgxPointingSide::BeyondPole}) {
            for (double dec = -89.0; dec <= 89.0; dec += 1.0) {
                INFO("hemisphere " << static_cast<int>(hemisphere) << " side " << static_cast<int>(side) << " dec "
                                   << dec);
                const auto target = cgx_sky_position_to_encoders(6.0, dec, 6.0, hemisphere, side).second;
                const auto from_home = signed_cgx_encoder_delta(target, kCgxDecHomeEncoder);
                const bool crosses = from_home > 0 ? seam_from_home > 0 && seam_from_home <= from_home
                                                   : seam_from_home < 0 && seam_from_home >= from_home;
                CHECK_FALSE(crosses);
            }
        }
    }
}

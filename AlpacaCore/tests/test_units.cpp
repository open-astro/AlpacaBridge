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

#include "catch2_compat.h"

#include <alpacacore/util/units.h>
#include <cmath>

TEST_CASE("Unit conversions", "[units]") {
    using namespace alpacacore::units;
    
    SECTION("Degrees to radians") {
        REQUIRE(std::abs(deg_to_rad(180.0) - 3.14159265358979323846) < 1e-9);
        REQUIRE(std::abs(deg_to_rad(90.0) - 1.57079632679489661923) < 1e-9);
    }
    
    SECTION("Radians to degrees") {
        REQUIRE(std::abs(rad_to_deg(3.14159265358979323846) - 180.0) < 1e-9);
        REQUIRE(std::abs(rad_to_deg(1.57079632679489661923) - 90.0) < 1e-9);
    }
    
    SECTION("Hours to degrees (RA)") {
        REQUIRE(std::abs(hours_to_deg(24.0) - 360.0) < 1e-9);
        REQUIRE(std::abs(hours_to_deg(12.0) - 180.0) < 1e-9);
    }
    
    SECTION("Degrees to hours (RA)") {
        REQUIRE(std::abs(deg_to_hours(360.0) - 24.0) < 1e-9);
        REQUIRE(std::abs(deg_to_hours(180.0) - 12.0) < 1e-9);
    }
    
    SECTION("Microns to meters") {
        REQUIRE(std::abs(microns_to_meters(1000000.0) - 1.0) < 1e-9);
    }
    
    SECTION("Meters to microns") {
        REQUIRE(std::abs(meters_to_microns(1.0) - 1000000.0) < 1e-9);
    }
}


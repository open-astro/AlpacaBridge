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

#if __has_include(<catch2/catch_all.hpp>)
#include <catch2/catch_all.hpp>
#elif __has_include(<catch2/catch.hpp>)
#include <catch2/catch.hpp>
#else
#error "Catch2 headers not found. Install Catch2 v2 or v3."
#endif

#include <cmath>

#ifndef ALPACA_TEST_APPROX_MARGIN
#define ALPACA_TEST_APPROX_MARGIN 1e-6
#endif

#define ALPACA_REQUIRE_APPROX(actual, expected)                                             \
    REQUIRE(std::fabs((actual) - (expected)) <= ALPACA_TEST_APPROX_MARGIN)

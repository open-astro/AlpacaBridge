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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <string>

TEST_CASE("AlpacaException stores message and code", "[error_handling]") {
    using namespace alpacacore;

    AlpacaException invalid("bad value", AlpacaError::InvalidValue);
    REQUIRE(std::string(invalid.what()) == "bad value");
    REQUIRE(invalid.error_code() == AlpacaError::InvalidValue);

    AlpacaException default_error("driver error");
    REQUIRE(std::string(default_error.what()) == "driver error");
    REQUIRE(default_error.error_code() == AlpacaError::DriverException);
}

TEST_CASE("AlpacaException asked_too_early is a flagged InvalidOperation", "[error_handling]") {
    using namespace alpacacore;

    // Issue #1167: the router logs this one at DEBUG; the client sees an ordinary InvalidOperation.
    const AlpacaException early = AlpacaException::asked_too_early("Image not ready");
    CHECK(std::string(early.what()) == "Image not ready");
    CHECK(early.error_code() == AlpacaError::InvalidOperation);
    CHECK(early.is_asked_too_early());

    // A copy (what a catch by value or a rethrow holds) keeps the flag.
    const AlpacaException copy = early;
    CHECK(copy.is_asked_too_early());

    // An InvalidOperation built the usual way is not flagged, so a fault stays at ERROR.
    const AlpacaException fault("Exposure stopped during image retrieval", AlpacaError::InvalidOperation);
    CHECK_FALSE(fault.is_asked_too_early());
    CHECK_FALSE(AlpacaException("driver error").is_asked_too_early());
}

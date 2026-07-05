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

// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

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

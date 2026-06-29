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

#include <alpacacore/util/version_format.h>

#include <string>

#include "catch2_compat.h"

TEST_CASE("normalize_dotted_version renders comma/space versions as dotted", "[util][version]") {
    using alpacacore::util::normalize_dotted_version;

    // ZWO ASIGetSDKVersion()-style output.
    REQUIRE(normalize_dotted_version("1, 7, 7, 0") == "1.7.7.0");
    REQUIRE(normalize_dotted_version("1,7,7,0") == "1.7.7.0");

    // Already-dotted strings pass through unchanged.
    REQUIRE(normalize_dotted_version("3.10.0") == "3.10.0");

    // Edge cases.
    REQUIRE(normalize_dotted_version("").empty());
    REQUIRE(normalize_dotted_version("42") == "42");
}

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

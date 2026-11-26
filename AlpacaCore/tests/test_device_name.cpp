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

#include <alpacacore/util/device_name.h>

#include "catch2_compat.h"

using alpacacore::util::without_qualifiers;

// Decision 0024 rules 1 and 2: no '(' is left for a Name, whatever the SDK or device put around it.
TEST_CASE("Device name - without_qualifiers leaves no bracket", "[util][device_name][unit]") {
    CHECK(without_qualifiers("Sony Alpha-A7 III (Control)") == "Sony Alpha-A7 III");
    CHECK(without_qualifiers("EQ6 (belt) Pro") == "EQ6 Pro");
    CHECK(without_qualifiers("EQ6(belt)") == "EQ6");
    CHECK(without_qualifiers("EQ6 (belt mod") == "EQ6");
    CHECK(without_qualifiers("(unnamed)").empty());
    CHECK(without_qualifiers("ASI2600MC Pro") == "ASI2600MC Pro");
}

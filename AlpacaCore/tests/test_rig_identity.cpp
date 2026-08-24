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

#include <alpacacore/util/rig_identity.h>
#include "catch2_compat.h"
#include <cctype>

TEST_CASE("rig_id is four uppercase hex characters") {
    const std::string& id = alpacacore::rig::rig_id();
    REQUIRE(id.size() == 4);
    for (unsigned char c : id) {
        CHECK(std::isxdigit(c));
        CHECK(!std::islower(c));
    }
    // Cached: same object every call
    CHECK(&alpacacore::rig::rig_id() == &id);
}

TEST_CASE("rig helpers prefix DeviceName and suffix UniqueID") {
    const std::string& id = alpacacore::rig::rig_id();
    CHECK(alpacacore::rig::prefix_device_name("Gemini Flat Panel") == id + ": Gemini Flat Panel");
    CHECK(alpacacore::rig::scope_unique_id("GEMINI_FLATPANEL_0") == "GEMINI_FLATPANEL_0-" + id);
}

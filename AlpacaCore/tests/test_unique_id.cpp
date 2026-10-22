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

#include <alpacacore/util/unique_id.h>

#include <set>
#include <string>

#include "catch2_compat.h"

using alpacacore::util::generate_unique_id;
using alpacacore::util::is_unique_id;

TEST_CASE("Unique id - generate makes distinct version 4 UUIDs", "[unique-id][unit]") {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string id = generate_unique_id();
        INFO(id);
        CHECK(id.size() == 36);
        CHECK(is_unique_id(id));
        CHECK(id[14] == '4');
        seen.insert(id);
    }
    CHECK(seen.size() == 200);
}

TEST_CASE("Unique id - only the canonical lowercase version 4 form is accepted", "[unique-id][unit]") {
    CHECK(is_unique_id("123e4567-e89b-42d3-a456-426614174000"));
    CHECK(is_unique_id("00000000-0000-4000-8000-000000000000"));
    CHECK_FALSE(is_unique_id(""));
    CHECK_FALSE(is_unique_id("ZWO_UID_00112233445566ff"));
    CHECK_FALSE(is_unique_id("SkyWatcher_0"));
    CHECK_FALSE(is_unique_id("123E4567-E89B-42D3-A456-426614174000"));  // uppercase
    CHECK_FALSE(is_unique_id("123e4567-e89b-12d3-a456-426614174000"));  // version 1
    CHECK_FALSE(is_unique_id("123e4567-e89b-42d3-c456-426614174000"));  // wrong variant
    CHECK_FALSE(is_unique_id("123e4567e89b42d3a456426614174000"));      // no hyphens
    CHECK_FALSE(is_unique_id("123e4567-e89b-42d3-a456-42661417400"));   // short
}

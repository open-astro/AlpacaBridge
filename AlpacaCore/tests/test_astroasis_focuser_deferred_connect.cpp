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

// Astroasis Oasis focuser: auto-detect resolves at connect time, not at
// construction (issue #659). There is no hidapi fake, so only the refusal
// case runs here: construction succeeds with no focuser on the bus, and the
// scan's message is the connect error. The reuse and re-scan cases are
// covered by test_connection_resolver.cpp on the shared helper.

#ifndef _WIN32

#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"

namespace {

using Info = std::string;

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::astroasis::create_astroasis_focuser_deferred(0, std::move(resolver));
    };
}

}  // namespace

TEST_CASE("Astroasis focuser auto-detect - a failed scan refuses the connect, not construction",
          "[astroasis][focuser][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "No Astroasis Oasis Focuser detected");
}

TEST_CASE("Astroasis focuser auto-detect - the production factory constructs with no hardware",
          "[astroasis][focuser][unit]") {
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = alpacacore::vendor::astroasis::create_astroasis_focuser_by_index(0, 0));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
}

#endif  // _WIN32

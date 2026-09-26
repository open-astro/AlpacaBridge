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

// iOptron iEAF focuser: auto-detect resolves at connect time, not at construction
// (issue #659). The persisted device is constructed at server start-up, when
// the hardware is often not there yet; the scan now runs inside the connect,
// its refusal is the connect error the client sees, and the resolved
// endpoint is reused on the next connect. Cases live in
// deferred_connect_cases.h; this file supplies the fake and the seam.

#ifndef _WIN32

#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"
#include "fake_ioptron_ieaf.h"

namespace {

using Info = alpacacore::vendor::ioptron::IeafConnectionConfig;

using Fake = alpacacore::test::FakeIoptronIeaf;

Info endpoint(const std::string& path) {
    Info config;
    config.serial_port = path;
    config.model = "ieaf";
    return config;
}

Info spawn(std::unique_ptr<Fake>& fake) {
    // No ok() check as the socket fakes have: PtyPair throws from the fake's
    // constructor when the pty cannot be set up (fake_pty_write.h, #387).
    auto next = std::make_unique<Fake>();
    fake = std::move(next);
    return endpoint(fake->slave_path());
}

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::ioptron::create_ieaf_focuser_deferred(0, std::move(resolver), "ieaf");
    };
}

}  // namespace

TEST_CASE("iOptron iEAF focuser auto-detect - a failed scan refuses the connect, not construction",
          "[ioptron][focuser][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "nothing answered the auto-detect probe");
}

TEST_CASE("iOptron iEAF focuser auto-detect - resolves at connect and reuses the endpoint",
          "[ioptron][focuser][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_reuses_endpoint<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("iOptron iEAF focuser auto-detect - re-scans when the resolved endpoint dies", "[ioptron][focuser][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_re_resolves<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("iOptron iEAF focuser auto-detect - the production factory constructs with no hardware",
          "[ioptron][focuser][unit]") {
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = alpacacore::vendor::ioptron::create_ieaf_focuser_by_index(0, 0, "ieaf"));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
}

#endif  // _WIN32

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

// iOptron telescope: auto-detect resolves at connect time, not at construction
// (issue #659). The persisted device is constructed at server start-up, when
// the hardware is often not there yet; the scan now runs inside the connect,
// its refusal is the connect error the client sees, and the resolved
// endpoint is reused on the next connect. Cases live in
// deferred_connect_cases.h; this file supplies the fake and the seam.

#ifndef _WIN32

#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"
#include "fake_ioptron_mount.h"

namespace {

using Info = alpacacore::vendor::ioptron::ConnectionInfo;

using Fake = alpacacore::test::FakeIoptronMount;

Info endpoint(int port) {
    Info conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    conn.response_timeout_ms = 2000;  // the driver's per-character read pacing, see test_ioptron_async_slew_failure.cpp
    return conn;
}

// Construct the replacement BEFORE the old fake dies so the two never share a port.
Info spawn(std::unique_ptr<Fake>& fake) {
    auto next = std::make_unique<Fake>("0012", 0.0);
    REQUIRE(next->ok());
    fake = std::move(next);
    return endpoint(fake->port());
}

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::ioptron::create_ioptron_telescope_deferred(0, std::move(resolver), 39.7392,
                                                                              -104.9903, 1609.0);
    };
}

}  // namespace

TEST_CASE("iOptron telescope auto-detect - a failed scan refuses the connect, not construction",
          "[ioptron][telescope][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "nothing answered the auto-detect probe");
}

TEST_CASE("iOptron telescope auto-detect - resolves at connect and reuses the endpoint", "[ioptron][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_reuses_endpoint<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("iOptron telescope auto-detect - re-scans when the resolved endpoint dies", "[ioptron][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_re_resolves<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("iOptron telescope auto-detect - the production factory constructs with no hardware",
          "[ioptron][telescope][unit]") {
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(
            driver = alpacacore::vendor::ioptron::create_ioptron_telescope_auto(0, 0, 39.7392, -104.9903, 1609.0));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = alpacacore::vendor::ioptron::create_ioptron_telescope_auto_network(0, 0, 39.7392,
                                                                                                    -104.9903, 1609.0));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
}

#endif  // _WIN32

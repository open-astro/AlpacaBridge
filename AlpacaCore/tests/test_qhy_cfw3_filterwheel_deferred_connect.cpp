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

// QHYCFW3 filter wheel: auto-detect resolves at connect time, not at
// construction (issue #659). Doubly important here: the probe DTR-resets
// every CP210x device on the box, and a wheel takes ~17 s to boot after the
// SBC powers it, so a scan at server start-up both missed it and reset its
// neighbours. Cases live in deferred_connect_cases.h; this file supplies the
// fake and the seam.

#ifndef _WIN32

#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"
#include "fake_qhy_cfw3.h"

namespace {

using Info = alpacacore::vendor::qhy::Cfw3ConnectionConfig;
using Fake = alpacacore::test::FakeQhyCfw3;

// A pty has no DTR, so no boot byte arrives; the short boot timeout lets the
// connect proceed the way test_qhy_cfw3_filterwheel.cpp does.
Info endpoint(const std::string& path) {
    Info config;
    config.serial_port = path;
    config.boot_timeout_ms = 150;
    config.reply_timeout_ms = 500;
    config.move_timeout_ms = 2000;
    return config;
}

// Construct the replacement BEFORE the old fake dies so the two never share a pty.
Info spawn(std::unique_ptr<Fake>& fake) {
    // No ok() check as the socket fakes have: PtyPair throws from the fake's
    // constructor when the pty cannot be set up (fake_pty_write.h, #387).
    auto next = std::make_unique<Fake>();
    fake = std::move(next);
    return endpoint(fake->slave_path());
}

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel_deferred(0, std::move(resolver));
    };
}

}  // namespace

TEST_CASE("QHYCFW3 filter wheel auto-detect - a failed scan refuses the connect, not construction",
          "[qhy][filterwheel][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "nothing answered the auto-detect probe");
}

TEST_CASE("QHYCFW3 filter wheel auto-detect - resolves at connect and reuses the endpoint",
          "[qhy][filterwheel][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_reuses_endpoint<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("QHYCFW3 filter wheel auto-detect - re-scans when the resolved endpoint dies", "[qhy][filterwheel][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_re_resolves<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("QHYCFW3 filter wheel auto-detect - the production factory constructs with no hardware",
          "[qhy][filterwheel][unit]") {
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel_by_index(0, 0));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
}

#endif  // _WIN32

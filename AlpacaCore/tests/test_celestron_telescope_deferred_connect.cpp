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

// Celestron telescope: auto-detect resolves at connect time, not at construction
// (issue #659). The persisted device is constructed at server start-up, when
// the hardware is often not there yet; the scan now runs inside the connect,
// its refusal is the connect error the client sees, and the resolved
// endpoint is reused on the next connect. Cases live in
// deferred_connect_cases.h; this file supplies the fake and the seam.

#ifndef _WIN32

#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"
#include "fake_mount_server.h"

namespace {

using Info = alpacacore::vendor::celestron::ConnectionInfo;

using Fake = alpacacore::test::FakeMountServer;

// Enough of the NexStar hand controller for connect: a parseable position
// pair, alignment complete, not slewing, and an ack for everything else.
std::string celestron_responder(const std::string& chunk) {
    if (chunk.empty()) return "0#";
    switch (chunk[0]) {
        case 'e':
        case 'E':
        case 'z':
        case 'Z':
            return "12AB0500,20000500#";
        case 'J':
            return "1#";
        case 'L':
            return "0#";
        case 'M':
        case 'T':
            return "#";
        case 'P':
            return std::string("\xFF#");
        default:
            return "0#";
    }
}

Info endpoint(int port) {
    Info info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

Info spawn(std::unique_ptr<Fake>& fake) {
    auto next = std::make_unique<Fake>(celestron_responder);
    REQUIRE(next->ok());
    fake = std::move(next);
    return endpoint(fake->port());
}

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::celestron::create_celestron_telescope_deferred(0, std::move(resolver));
    };
}

}  // namespace

TEST_CASE("Celestron telescope auto-detect - a failed scan refuses the connect, not construction",
          "[celestron][telescope][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "nothing answered the auto-detect probe");
}

TEST_CASE("Celestron telescope auto-detect - resolves at connect and reuses the endpoint",
          "[celestron][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_reuses_endpoint<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("Celestron telescope auto-detect - re-scans when the resolved endpoint dies",
          "[celestron][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_re_resolves<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("Celestron telescope auto-detect - the production factory constructs with no hardware",
          "[celestron][telescope][unit]") {
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = alpacacore::vendor::celestron::create_celestron_telescope_auto(0, 0));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
}

#endif  // _WIN32

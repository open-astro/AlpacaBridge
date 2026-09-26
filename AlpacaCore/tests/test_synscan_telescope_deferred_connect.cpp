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

// SynScan telescope: auto-detect resolves at connect time, not at construction
// (issue #659). The persisted device is constructed at server start-up, when
// the hardware is often not there yet; the scan now runs inside the connect,
// its refusal is the connect error the client sees, and the resolved
// endpoint is reused on the next connect. Cases live in
// deferred_connect_cases.h; this file supplies the fake and the seam.

#ifndef _WIN32

#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"
#include "fake_mount_server.h"

namespace {

using Info = alpacacore::vendor::synscan::ConnectionInfo;

using Fake = alpacacore::test::FakeMountServer;

// Enough of the hand controller for connect: the echo test, the firmware
// query and a parseable position pair.
std::string synscan_responder(const std::string& chunk) {
    if (chunk.empty()) return "0#";
    switch (chunk[0]) {
        case 'K':
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        case 'V':
            return "042A00#";
        case 'e':
        case 'E':
        case 'z':
        case 'Z':
            return "12AB0500,20000500#";
        default:
            return "0#";
    }
}

Info endpoint(int port) {
    Info info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

Info spawn(std::unique_ptr<Fake>& fake) {
    auto next = std::make_unique<Fake>(synscan_responder);
    REQUIRE(next->ok());
    fake = std::move(next);
    return endpoint(fake->port());
}

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::synscan::create_synscan_telescope_deferred(
            0, std::move(resolver), alpacacore::vendor::synscan::SynScanVersion::V4);
    };
}

}  // namespace

TEST_CASE("SynScan telescope auto-detect - a failed scan refuses the connect, not construction",
          "[synscan][telescope][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "nothing answered the auto-detect probe");
}

TEST_CASE("SynScan telescope auto-detect - resolves at connect and reuses the endpoint", "[synscan][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_reuses_endpoint<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("SynScan telescope auto-detect - re-scans when the resolved endpoint dies", "[synscan][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_re_resolves<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("SynScan telescope auto-detect - a port that opens but does not echo is stale and re-scans",
          "[synscan][telescope][unit]") {
    // The identity gate (review of #660, round 6). connect() succeeds on any
    // open port, so a resolved endpoint another device now owns still opens;
    // only the echo test tells a handset from a port with nothing listening,
    // and its failure is what turns a reused endpoint into a fresh scan. This
    // fake accepts the TCP connect and answers every query except the echo,
    // so the shared re-resolve case (whose old fake is GONE, refused connect)
    // never reaches it.
    auto silent = std::make_unique<Fake>([](const std::string& chunk) -> std::string {
        if (!chunk.empty() && chunk[0] == 'K') return "";  // no echo, ever
        return synscan_responder(chunk);
    });
    REQUIRE(silent->ok());
    int calls = 0;
    Info current = endpoint(silent->port());
    auto driver = make_driver()([&calls, &current]() -> Info {
        ++calls;
        return current;
    });

    // First connect: the scan resolves the silent endpoint, the TCP connect
    // succeeds, the echo gate refuses with the client-facing message. The
    // endpoint is now "resolved" (the post-scan failure propagates unchanged).
    REQUIRE_THROWS_WITH(driver->set_connected(true),
                        Catch::Matchers::ContainsSubstring("did not answer the echo test"));
    CHECK(calls == 1);
    CHECK_FALSE(driver->get_connected());

    // Second connect: the retry of the resolved endpoint opens again and again
    // fails the echo. That is StaleEndpoint, so the helper scans once more and
    // reaches the handset that now answers. With a plain AlpacaException from
    // the gate (the pre-#660 shape) no re-scan happens and this connect fails.
    std::unique_ptr<Fake> echoing;
    current = spawn(echoing);
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(calls == 2);
    CHECK(driver->get_connected());
    REQUIRE(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("SynScan telescope auto-detect - the production factory constructs with no hardware",
          "[synscan][telescope][unit]") {
    {
        // Construction must not scan: a scan at start-up is the #659 bug. The
        // resolver runs only inside Connected=true, which this case never issues.
        std::unique_ptr<alpacacore::AlpacaDriver> driver;
        REQUIRE_NOTHROW(driver = alpacacore::vendor::synscan::create_synscan_telescope_auto(
                            0, 0, alpacacore::vendor::synscan::SynScanVersion::V4));
        REQUIRE(driver != nullptr);
        CHECK_FALSE(driver->get_connected());
    }
}

#endif  // _WIN32

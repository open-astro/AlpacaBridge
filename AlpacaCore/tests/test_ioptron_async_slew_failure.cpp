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

// open-astro#575, iOptron: the async slew dispatch runs on its own thread with
// allow_soft_fail=true, so a mount that answers ":MS1"/":MS2" with "0" (target
// rejected, e.g. below the altitude limit) is logged as "treating as no-op"
// and Slewing drops to false -- the client is told nothing. That soft-fail is
// exactly the "work fails after initiation" case of the completion-property
// contract: the next Slewing read must return an Alpaca error instead
// (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_ioptron_mount.h"

namespace {

using Clock = std::chrono::steady_clock;

alpacacore::vendor::ioptron::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    // NOT 200 like the other four vendors' #575 tests: iOptron's
    // read_response() sleeps 10ms after every character it reads, so the
    // fake's fixed-width :GEP/:GLS replies (21-24 bytes) alone cost
    // ~200-240ms even with an instant round trip on the wire (confirmed via
    // strace: the fake server's recv() of the request completes and its
    // reply is sent well under 1ms; the 200ms is spent entirely in this
    // driver's own per-character poll loop). 200ms made ensure_not_parked's
    // synchronous status refresh -- called before the #575 code path is even
    // reached -- time out on every run. test_ioptron_telescope.cpp's
    // loopback_endpoint() sees the same cost and avoids it by leaving this
    // at the 5000ms default; matching that here instead of the 200ms used by
    // the other vendors' fakes (whose replies are shorter / read
    // differently).
    conn.response_timeout_ms = 2000;
    return conn;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// Threw only for the driver-specific DriverException (0x500) the #575 contract
// requires; any other code is ThrewOther, so a changed code fails the tests.
enum class SlewingRead : std::uint8_t { True, False, Threw, ThrewOther };

SlewingRead read_slewing(alpacacore::TelescopeDriver& driver, std::string* message = nullptr) {
    try {
        return driver.get_slewing() ? SlewingRead::True : SlewingRead::False;
    } catch (const alpacacore::AlpacaException& ex) {
        if (message) *message = ex.what();
        return ex.error_code() == alpacacore::AlpacaError::DriverException ? SlewingRead::Threw
                                                                           : SlewingRead::ThrewOther;
    }
}

}  // namespace

TEST_CASE("iOptron async - a GOTO the mount rejects surfaces through Slewing instead of a silent no-op (#575)",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 1; }, 5000));  // the dispatch ran on the task

    // Today: "Slew rejected by mount - treating as no-op for async slew" in
    // the log, Slewing=false to the client. Must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // A retried initiator the mount accepts clears it.
    mount.set_reject_goto(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 2; }, 5000));
    const SlewingRead after_retry = read_slewing(*driver);
    CHECK((after_retry == SlewingRead::True || after_retry == SlewingRead::False));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 15000));
    driver->set_connected(false);
}

TEST_CASE("iOptron async - AbortSlew clears a stored slew failure (#575)", "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

#endif  // _WIN32

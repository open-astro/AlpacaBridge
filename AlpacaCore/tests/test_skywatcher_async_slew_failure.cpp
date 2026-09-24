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

// open-astro#575: an async slew that fails AFTER the initiator returned used
// to be logged and forgotten -- the next Slewing read answered false, which a
// client cannot tell apart from a successful arrival. The ASCOM completion
// property contract (.claude/skills/ascom-alpaca-protocol/references/
// async-and-errors.md, "Completion property") requires the next Slewing read
// to return an Alpaca error instead, kept until a later valid command
// (AbortSlew, a new slew initiator, disconnect) clears it.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;

namespace {

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0);
    driver->set_connected(true);
    return driver;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
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

TEST_CASE("SkyWatcher async - a slew that fails after dispatch surfaces through Slewing (#575)",
          "[skywatcher][async][slewfailure]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    // The goto starts RA first; refusing that ":J" with "!2" (Motor not
    // stopped) makes dispatch_predicted_goto_locked() throw INSIDE the slew
    // task, after slew_to_coordinates_async() has already returned.
    mount.reject_start_motion(1, 1);
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));

    // Today the task logs "Async slew failed" and Slewing settles to FALSE --
    // indistinguishable from a landed goto. It must instead become an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);  // names the ASCOM operation

    // Preserved until a client-visible clearing command: a second poller
    // (web UI, ConformU's 500 ms loop) must see the same failure, not false.
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // AbortSlew is a valid later command: it clears the stored failure and
    // Slewing reads normally again.
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);

    // And a fresh async slew (the fake accepts this one) is a clean start:
    // Slewing=true with no stale error in the way, landing as usual.
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 30000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a new slew initiator replaces the stored failure (#575)",
          "[skywatcher][async][slewfailure]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    mount.reject_start_motion(1, 1);
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    // No AbortSlew in between: the next initiator itself clears the failure
    // (a client that retries the goto must not be told the OLD goto failed).
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 30000));
    driver->set_connected(false);
}

// GUARD, not a proof: this case is GREEN before the fix and stays green
// after it. It pins the regression risk named in the #575 design: AbortSlew
// cancels the slew task by making wait_for_slew_complete() throw "Slew wait
// cancelled" into the SAME catch block that must now record real failures.
// A fix that records the cancellation as a failure turns this red.
TEST_CASE("SkyWatcher async - AbortSlew's own cancellation is not reported as a slew failure (#575)",
          "[skywatcher][async][slewfailure]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    REQUIRE(read_slewing(*driver) == SlewingRead::True);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    driver->abort_slew();  // joins the slew task: its catch block has run by now
    CHECK(read_slewing(*driver) == SlewingRead::False);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

#endif  // _WIN32

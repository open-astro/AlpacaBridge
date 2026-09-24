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

// open-astro#575, ZWO: the GOTO setup thread sends ":MS" after the initiator
// returned; a mount that rejects it ("Mount rejected GOTO request") was logged
// as "GOTO failed" and Slewing simply dropped to false once slew_force_until_
// expired. The next Slewing read must return an Alpaca error instead
// (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeZwoState {
    std::atomic<bool> reject_goto{false};
    std::atomic<int> goto_count{0};
    // Delay the ":Sr" ack (still inside the 250 ms reply timeout) so a test
    // can land AbortSlew while the GOTO setup thread is mid-setup.
    std::atomic<int> sr_delay_ms{0};
};

// LX200-style AM5 dialect: ":Sr"/":Sd"/":SMTI" are acked with a bare "1",
// ":MS" answers "0" (accepted) or a non-zero code (rejected), ":GAT" reports
// tracking, and "0#" is a validly terminated reply for everything else.
alpacacore::test::FakeMountServer::Responder zwo_responder(const std::shared_ptr<FakeZwoState>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.find(":MS") != std::string::npos) {
            st->goto_count.fetch_add(1);
            return st->reject_goto.load() ? "2" : "0";
        }
        if (chunk.find(":Sr") != std::string::npos && st->sr_delay_ms.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(st->sr_delay_ms.load()));
        }
        if (chunk.find(":Sr") != std::string::npos || chunk.find(":Sd") != std::string::npos ||
            chunk.find(":SMTI") != std::string::npos) {
            return "1";
        }
        if (chunk.find(":GAT") != std::string::npos) {
            return "1#";
        }
        return "0#";
    };
}

alpacacore::vendor::zwo::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    conn.response_timeout_ms = 250;
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

TEST_CASE("ZWO async - a GOTO the mount rejects surfaces through Slewing (#575)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() >= 1; }, 5000));  // ":MS" went out on the GOTO thread

    // Today: "GOTO failed: Mount rejected GOTO request" in the log, then
    // Slewing=false after the 5 s force window. Must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // Only Slewing reports the failure: the rejected GOTO's pending
    // post-slew adjustment must not turn a position read into that error.
    // (This fake serves no :GR/:GD, so the read may still fail to parse.)
    const auto position_read_error = [&](const std::function<void()>& read) {
        try {
            read();
        } catch (const std::exception& ex) {
            return std::string(ex.what());
        }
        return std::string();
    };
    CHECK(position_read_error([&] { (void)driver->get_right_ascension(); }).find("SlewToTargetAsync") ==
          std::string::npos);
    CHECK(position_read_error([&] { (void)driver->get_declination(); }).find("SlewToTargetAsync") == std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // AbortSlew is a valid later command: it clears the stored failure.
    REQUIRE_NOTHROW(driver->abort_slew());
    const SlewingRead after_abort = read_slewing(*driver);
    CHECK((after_abort == SlewingRead::True || after_abort == SlewingRead::False));
    driver->set_connected(false);
}

TEST_CASE("ZWO async - AbortSlew during GOTO setup is not reported as a slew failure (#575)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The mount would reject the GOTO, but the client aborts while the setup
    // thread is still writing the target: the abort cancels the setup, so the
    // rejection that would have followed must never be recorded.
    st->reject_goto.store(true);
    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE_NOTHROW(driver->abort_slew());
    // Outlast the setup (site sync + delayed ":Sr" + ":Sd" + ":MS").
    CHECK_FALSE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 2000));
    CHECK(st->goto_count.load() == 0);  // the aborted GOTO was never sent
    driver->set_connected(false);
}

TEST_CASE("ZWO async - a MoveAxis during GOTO setup owns the error slot (#575)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // A newer initiator (here MoveAxis) lands while the GOTO setup thread is
    // still writing the target; the superseded GOTO's later rejection is not
    // the jog's failure and must not make Slewing throw during the jog.
    st->reject_goto.store(true);
    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE_NOTHROW(driver->move_axis(0, 1.0));
    CHECK_FALSE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 2000));
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

#endif  // _WIN32

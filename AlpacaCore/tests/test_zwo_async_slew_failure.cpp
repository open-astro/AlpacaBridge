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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"

namespace {

using Clock = std::chrono::steady_clock;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZwoMount;
using alpacacore::test::timed;

alpacacore::vendor::zwo::ConnectionInfo endpoint(int port, int response_timeout_ms) {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    conn.response_timeout_ms = response_timeout_ms;
    return conn;
}

// Real-time bound on a wire event (a command reaching the mount, a driver thread storing its error), never a driver
// timer: those advance the FakeTaskClock.
bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return pred();
}

// The AM5 under test, on the clock the driver reads: GOTO, park and the status flags are the mount's, so a test
// advances the clock instead of sleeping.
struct Rig {
    FakeTaskClock clock;
    FakeZwoMount mount{clock, "AM5"};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit Rig(int response_timeout_ms = 250) {
        driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(mount.port(), response_timeout_ms), clock);
    }
    ~Rig() {
        mount.release();
        driver.reset();
    }
    void connect(std::chrono::milliseconds budget = std::chrono::seconds(10)) {
        REQUIRE(mount.ok());
        REQUIRE(alpacacore::test::settle_connected(*driver, true, budget));
    }
    // One poll step: wait for the poll loop to park on the clock, advance, let the woken waiters finish.
    void tick(std::chrono::nanoseconds duration = std::chrono::milliseconds(250)) {
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(duration);
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    }
    // A GOTO's setup pauses the poll loop until the mount has answered ":MS" or the GOTO is cancelled; the paused
    // loop re-checks every 50 ms of clock time. Step it until it has read status again.
    void await_poll_resumed() {
        const int base = mount.count(":GU");
        for (int i = 0; i < 2000 && !mount.wait_for_count(":GU", base + 1, std::chrono::milliseconds(1)); ++i) {
            tick(std::chrono::milliseconds(50));
        }
        REQUIRE(mount.count(":GU") > base);
    }
};

// A command received exactly as written ("" prefix matching would also count ":Qe" for ":Q").
int count_exact(const FakeZwoMount& mount, const std::string& command) {
    const auto all = mount.commands();
    return static_cast<int>(std::count(all.begin(), all.end(), command));
}

// Every command only the GOTO setup thread sends (site read, site/time write, target write): open-astro#720
// requires that none arrives after the call that fences the GOTO has returned.
int setup_count(const FakeZwoMount& mount) {
    int total = 0;
    for (const char* setup : {":GMGE", ":SMGE", ":SMTI", ":Sr", ":Sd"}) total += mount.count(setup);
    return total;
}

// The jog commands ":Me" / ":Mw" / ":Mn" / ":Ms".
int jog_count(const FakeZwoMount& mount) {
    int total = 0;
    for (const char* jog : {":Me", ":Mw", ":Mn", ":Ms"}) total += mount.count(jog);
    return total;
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

// ConformU's STANDARD response target for an initiator.
constexpr auto kInitiatorBudget = std::chrono::milliseconds(1000);

// Whether a ":MS" followed the initiator: the setup is a straight run of wire exchanges, so a GOTO that was going to
// go out does within this real-time bound.
bool goto_sent_within_setup_window(const FakeZwoMount& mount) {
    return wait_until([&] { return mount.count(":MS") > 0; }, 500);
}

// open-astro#720: the setup commands received once the fencing call returned, after the same bound, so a setup
// thread the call did not join has had time to send whatever it was going to send.
int setup_sent_after_return(const FakeZwoMount& mount, int at_return) {
    static_cast<void>(wait_until([&] { return setup_count(mount) > at_return; }, 500));
    return setup_count(mount) - at_return;
}

}  // namespace

TEST_CASE("ZWO rejected Sync preserves an active guide pulse and its position offset",
          "[zwo][telescope][async][sync-fence]") {
    const int targets = GENERATE(0, 1, 2);
    const int direction = GENERATE(0, 2);
    Rig rig;
    rig.connect();
    if (targets == 1) rig.driver->set_target_right_ascension(6.0);
    if (targets == 2) rig.driver->set_target_declination(30.0);

    const double ra = rig.driver->get_right_ascension();
    const double dec = rig.driver->get_declination();
    rig.driver->pulse_guide(direction, 2000);
    REQUIRE(rig.mount.wait_for_count(":Mg", 1, std::chrono::seconds(5)));
    REQUIRE(rig.driver->get_is_pulse_guiding());
    const double guided_ra = rig.driver->get_right_ascension();
    const double guided_dec = rig.driver->get_declination();
    REQUIRE((direction == 0 ? guided_dec - dec : guided_ra - ra) > 0.0);
    const int writes = setup_count(rig.mount);

    try {
        rig.driver->sync_to_target();
        FAIL("Sync with incomplete targets must fail");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::ValueNotSet);
    }

    CHECK(rig.driver->get_is_pulse_guiding());
    CHECK(rig.driver->get_right_ascension() == guided_ra);
    CHECK(rig.driver->get_declination() == guided_dec);
    CHECK(setup_count(rig.mount) == writes);
    CHECK(rig.mount.count(":SMMC") == 0);
    CHECK(rig.mount.count(":CM") == 0);
}

TEST_CASE("ZWO Sync cancels a pending GOTO retry before changing the target", "[zwo][telescope][async][sync-fence]") {
    const bool target_form = GENERATE(false, true);
    const bool fallback = GENERATE(false, true);
    CAPTURE(target_form, fallback);
    Rig rig;
    rig.connect();
    rig.mount.reject_goto("e3", 1);
    if (fallback) rig.mount.override_reply(":SMMC", "e2");
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE(rig.mount.wait_for_count(":Q", 1, std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));

    if (target_form) {
        REQUIRE_NOTHROW(rig.driver->sync_to_target());
    } else {
        REQUIRE_NOTHROW(rig.driver->sync_to_coordinates(6.0, -15.0));
    }
    REQUIRE(rig.mount.count(":CM") == (fallback ? 1 : 0));
    // The old retry must not start a slew using the target registers Sync just replaced.
    rig.tick(std::chrono::milliseconds(300));
    CHECK_FALSE(rig.mount.wait_for_count(":MS", 2, std::chrono::milliseconds(500)));
    CHECK_FALSE(rig.mount.slewing());
}

TEST_CASE("ZWO GOTO retries retain their captured target after target property writes",
          "[zwo][telescope][async][sync-fence]") {
    Rig rig;
    rig.connect();
    rig.mount.reject_goto("e3", 1);
    rig.driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE(rig.mount.wait_for_count(":Q", 1, std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    rig.driver->set_target_right_ascension(6.0);
    rig.driver->set_target_declination(-15.0);
    rig.tick(std::chrono::milliseconds(300));
    REQUIRE(rig.mount.wait_for_count(":MS", 2, std::chrono::seconds(5)));
    REQUIRE_NOTHROW(rig.driver->command_string(":GVP", false));
    rig.tick(std::chrono::seconds(6));
    CHECK(rig.mount.ra_hours() == 5.5);
    CHECK(rig.mount.dec_degrees() == 20.0);
}

TEST_CASE("ZWO target property writes wait for a complete Sync or GOTO target transaction",
          "[zwo][telescope][async][sync-fence]") {
    const bool sync_owner = GENERATE(false, true);
    const bool ra_write = GENERATE(false, true);
    CAPTURE(sync_owner, ra_write);
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e2");
    rig.mount.hold_reply(":Sr");
    std::future<void> sync;
    if (sync_owner) {
        sync = std::async(std::launch::async, [&] { rig.driver->sync_to_coordinates(5.5, 20.0); });
    } else {
        rig.driver->slew_to_coordinates_async(5.5, 20.0);
    }
    const bool held = rig.mount.wait_until_held(std::chrono::seconds(5));
    auto setter = std::async(std::launch::async, [&] {
        if (ra_write) {
            rig.driver->set_target_right_ascension(7.0);
        } else {
            rig.driver->set_target_declination(-30.0);
        }
    });
    // Getters stay responsive, but a competing setter cannot publish halfway through the transaction.
    const bool replaced = wait_until(
        [&] {
            return ra_write ? rig.driver->get_target_right_ascension() != 5.5
                            : rig.driver->get_target_declination() != 20.0;
        },
        50);
    rig.mount.release();  // release before any fatal assertion or future join
    setter.get();
    if (sync.valid()) {
        sync.get();
    } else {
        REQUIRE(rig.mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
        rig.driver->abort_slew();
    }
    REQUIRE(held);
    CHECK_FALSE(replaced);
    std::string ra, dec;
    for (const auto& command : rig.mount.commands()) {
        if (command.starts_with(":Sr")) ra = command;
        if (command.starts_with(":Sd")) dec = command;
        if (command == (sync_owner ? ":CM" : ":MS")) {
            CHECK(ra == ":Sr05:30:00");
            CHECK(dec == ":Sd+20:00:00");
        }
    }
    CHECK(rig.mount.count(sync_owner ? ":CM" : ":MS") == 1);
}

TEST_CASE("ZWO failed Sync target transactions stop and release the next command",
          "[zwo][telescope][async][sync-fence]") {
    const std::string rejected = GENERATE(std::string(":Sr"), std::string(":Sd"), std::string(":CM"));
    CAPTURE(rejected);
    Rig rig;
    rig.connect();
    rig.mount.override_reply(":SMMC", "e2");
    rig.mount.override_reply(rejected, "e2", 1);
    try {
        rig.driver->sync_to_coordinates(6.0, -15.0);
        FAIL("Expected the rejected Sync transaction to throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidValue);
    }
    CHECK(rig.mount.count(":Sd") == (rejected == ":Sr" ? 0 : 1));
    CHECK(rig.mount.count(":CM") == (rejected == ":CM" ? 1 : 0));
    REQUIRE_NOTHROW(rig.driver->sync_to_coordinates(5.5, 20.0));
    CHECK(rig.mount.count(":CM") == (rejected == ":CM" ? 2 : 1));
}

TEST_CASE("ZWO telescope - all four equatorial slews are refused while Tracking is false",
          "[zwo][telescope][async][tracking-guard]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    mount.set_tracking(false);
    rig.connect();
    REQUIRE_FALSE(driver->get_tracking());

    using alpacacore::test::require_alpaca_error;
    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_alpaca_error([&] { driver->slew_to_coordinates(5.5, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(5.5, 95.0); }, alpacacore::AlpacaError::InvalidValue);

    driver->set_target_right_ascension(5.5);  // the target forms need one; the setters write it to the mount
    driver->set_target_declination(20.0);
    const int targets_before = mount.count(":Sr") + mount.count(":Sd");
    require_alpaca_error([&] { driver->slew_to_coordinates(5.5, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(5.5, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

    // Neither a target write nor a GOTO reached the mount, and the driver is not slewing.
    CHECK_FALSE(wait_until(
        [&] { return mount.count(":MS") != 0 || mount.count(":Sr") + mount.count(":Sd") != targets_before; }, 500));
    CHECK(read_slewing(*driver) == SlewingRead::False);

    // With the mount tracking the same call goes through.
    mount.set_tracking(true);
    rig.tick();
    REQUIRE(driver->get_tracking());
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(wait_until([&] { return mount.count(":MS") >= 1; }, 5000));
    driver->set_connected(false);
}

// ":GU" may drop the tracking flag while a GOTO runs: a retarget of the driver's own GOTO is not refused for
// it, past the 5 s force window too. A commanded Tracking=false is still off.
TEST_CASE("ZWO telescope - a retarget of its own GOTO in flight is not refused for Tracking",
          "[zwo][telescope][async][tracking-guard]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();
    REQUIRE(driver->get_tracking());

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS") >= 1; }, 5000));
    REQUIRE_NOTHROW(driver->command_string(":GVP", false));  // Finish the setup exchange before advancing.
    mount.hold_running(true);                                // a GOTO that reports no tracking and never settles
    mount.set_tracking(false);
    rig.tick(std::chrono::milliseconds(5500));  // past the 5 s Slewing force window
    REQUIRE_FALSE(driver->get_tracking());
    CHECK(read_slewing(*driver) == SlewingRead::True);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(6.0, 25.0));
    CHECK(wait_until([&] { return mount.count(":MS") >= 2; }, 5000));

    // Tracking=false during the GOTO is a commanded off: the next slew is refused, nothing more sent.
    driver->set_tracking(false);
    try {
        driver->slew_to_coordinates_async(6.5, 25.0);
        FAIL("Expected an AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
    CHECK_FALSE(wait_until([&] { return mount.count(":MS") > 2; }, 500));
    driver->set_connected(false);
}

TEST_CASE("ZWO slew completion waits for the final coordinate pair", "[zwo][telescope][async][arrival]") {
    const bool reject_final = GENERATE(false, true);
    CAPTURE(reject_final);
    for (const bool delay_ra : {true, false}) {
        CAPTURE(delay_ra);
        Rig rig;
        auto& mount = rig.mount;
        auto& driver = rig.driver;
        const std::string delayed = delay_ra ? ":GR" : ":GD";
        mount.set_position(20.0 + 47.0 / 60.0, 64.0 + 59.0 / 60.0);
        mount.set_moving(true);  // under way before the connect, as from the hand controller
        rig.connect(std::chrono::seconds(5));
        REQUIRE(driver->get_slewing());
        REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
        // The mount arrives. The delayed coordinate read stays in flight until released, and may then be refused.
        const int gu_at_arrival = mount.count(":GU");
        mount.hold_reply(delayed);
        if (reject_final) mount.override_reply(delayed, "bad");
        mount.set_position(20.0 + 47.0 / 60.0 + 56.0 / 3600.0, 65.0);
        mount.set_moving(false);
        rig.clock.advance(std::chrono::milliseconds(250));
        REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));
        // GU says stopped, but GR/GD are still in flight: completion must stay hidden.
        CHECK(mount.count(":GU") == gu_at_arrival + 1);
        CHECK(driver->get_slewing());
        mount.release();
        REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
        if (reject_final) {
            // The next GU proves the previous coordinate read failed and the poll retried.
            rig.tick();
            REQUIRE(mount.count(":GU") >= gu_at_arrival + 2);
            CHECK(driver->get_slewing());
            mount.override_reply(delayed, "", 0);
            rig.tick();
        }
        REQUIRE_FALSE(driver->get_slewing());
        CHECK(std::abs(driver->get_right_ascension() - (20.0 + 47.0 / 60.0 + 56.0 / 3600.0)) < 1e-9);
        CHECK(std::abs(driver->get_declination() - 65.0) < 1e-9);
        CHECK(alpacacore::test::settle_connected(*driver, false));
    }
}

TEST_CASE("ZWO own GOTO remains retargetable until arrival is published unless Tracking is commanded off",
          "[zwo][telescope][async][arrival][goto-ownership]") {
    const bool reject_ra = GENERATE(false, true);
    const bool command_tracking_off = GENERATE(false, true);
    CAPTURE(reject_ra, command_tracking_off);
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    const std::string rejected = reject_ra ? ":GR" : ":GD";
    rig.connect(std::chrono::seconds(5));
    REQUIRE(driver->get_tracking());
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS") == 1; }, 5000));
    rig.await_poll_resumed();
    mount.hold_running(true);  // the GOTO keeps reporting motion, and no tracking
    mount.set_tracking(false);
    const int pairs_before = mount.count(":GD");
    rig.tick();
    REQUIRE(wait_until([&] { return mount.count(":GD") > pairs_before; }, 5000));  // a complete pair while moving
    // The force window alone would permit a retarget even if GU cleared ownership too early.
    rig.tick(std::chrono::milliseconds(5500));
    mount.override_reply(rejected, "bad");  // the final pair of the stop is refused
    mount.hold_running(false);              // the GOTO lands at the next status read
    const int gu_before_stop = mount.count(":GU");
    rig.clock.advance(std::chrono::milliseconds(250));
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(mount.count(":GU") > gu_before_stop);
    REQUIRE_FALSE(driver->get_tracking());
    REQUIRE(driver->get_slewing());

    const auto require_refused_without_writes = [&] {
        const int gotos = mount.count(":MS");
        const int targets = mount.count(":Sr") + mount.count(":Sd");
        try {
            driver->slew_to_coordinates_async(6.5, 25.0);
            FAIL("Expected Tracking-off retarget to be refused");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        }
        CHECK_FALSE(wait_until(
            [&] { return mount.count(":MS") != gotos || mount.count(":Sr") + mount.count(":Sd") != targets; }, 500));
    };

    if (command_tracking_off) {
        // Explicit Tracking=false revokes the exception even when the mount already reports it off.
        REQUIRE_NOTHROW(driver->set_tracking(false));
        require_refused_without_writes();
    } else {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(6.0, 25.0));
        REQUIRE(wait_until([&] { return mount.count(":MS") == 2; }, 5000));
        REQUIRE_NOTHROW(driver->command_string(":GVP", false));
        CHECK(driver->get_slewing());
        mount.override_reply(rejected, "", 0);
        rig.await_poll_resumed();
        rig.tick(std::chrono::milliseconds(5500));
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 8000));
        CHECK(std::abs(driver->get_right_ascension() - 6.0) < 1e-9);  // the retarget's landing, as the mount reports
        CHECK(std::abs(driver->get_declination() - 25.0) < 1e-9);
        REQUIRE_FALSE(driver->get_tracking());
        require_refused_without_writes();
    }
    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO replacement GOTO cannot complete during a delayed setup retry", "[zwo][telescope][async][goto-setup]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.reject_goto("e3", 2);
    driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));  // poll and first GOTO's retry

    mount.hold_reply(":Sr");
    driver->slew_to_coordinates_async(6.0, 25.0);  // cancels the older retry
    const bool held = mount.wait_until_held(std::chrono::seconds(5));
    CHECK(held);
    const int pairs_before = mount.count(":GD");
    mount.set_position(13.0, 46.0);
    rig.clock.advance(std::chrono::milliseconds(5500));
    mount.release();  // the replacement now reaches e3 and waits another 300 virtual ms
    REQUIRE(mount.wait_for_count(":MS", 2, std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    // An older worker must not release this setup's poll pause. If polling does run, stopped telemetry
    // still cannot complete a GOTO that has not been accepted by the mount.
    CHECK(mount.count(":GD") == pairs_before);
    CHECK(read_slewing(*driver) == SlewingRead::True);
    CHECK(mount.count(":MS") == 2);  // both rejected; no accepted GOTO yet

    rig.tick(std::chrono::milliseconds(300));
    REQUIRE(mount.wait_for_count(":MS", 3, std::chrono::seconds(5)));
    REQUIRE_NOTHROW(driver->command_string(":GVP", false));
    rig.await_poll_resumed();
    rig.tick(std::chrono::milliseconds(5500));
    CHECK_FALSE(driver->get_slewing());
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 1e-9);
    CHECK(std::abs(driver->get_declination() - 25.0) < 1e-9);
    driver->set_connected(false);
}

TEST_CASE("ZWO GOTO stays incomplete when Connected refresh polls during setup",
          "[zwo][telescope][async][goto-setup]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.reject_goto("e3", 1);
    driver->set_target_right_ascension(6.0);
    driver->set_target_declination(25.0);
    mount.hold_reply(":Sr");
    driver->slew_to_target_async();
    const bool held = mount.wait_until_held(std::chrono::seconds(5));
    CHECK(held);
    rig.clock.advance(std::chrono::milliseconds(5500));
    mount.release();
    REQUIRE(mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    mount.set_position(13.0, 46.0);
    const int pairs_before = mount.count(":GD");
    driver->set_connected(true);  // deliberately refreshes stopped telemetry while the retry is pending
    REQUIRE(mount.count(":GD") > pairs_before);
    REQUIRE(std::abs(driver->get_right_ascension() - 13.0) < 1e-9);
    CHECK(mount.count(":MS") == 1);  // rejected, not accepted
    CHECK(read_slewing(*driver) == SlewingRead::True);

    rig.tick(std::chrono::milliseconds(300));
    REQUIRE(mount.wait_for_count(":MS", 2, std::chrono::seconds(5)));
    REQUIRE_NOTHROW(driver->command_string(":GVP", false));
    rig.await_poll_resumed();
    rig.tick(std::chrono::milliseconds(5500));
    CHECK_FALSE(driver->get_slewing());
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 1e-9);
    CHECK(std::abs(driver->get_declination() - 25.0) < 1e-9);
    driver->set_connected(false);
}

TEST_CASE("ZWO blocking slew cannot return success during a delayed setup retry", "[zwo][telescope][goto-setup]") {
    const bool use_target = GENERATE(false, true);
    CAPTURE(use_target);
    // ":SMTI" stays held across real-time rendezvous below. Past a 250 ms reply timeout ":Sr", ":Sd" and ":MS" time
    // out behind it, and the wrapper takes an empty ":MS" reply as acceptance, so no e3 retry happens (#1873).
    Rig rig{5000};
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();
    // The first poll has parked on the clock, so it cannot queue behind the held ":SMTI" and miss the rendezvous.
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    driver->set_target_right_ascension(6.0);
    driver->set_target_declination(25.0);
    mount.reject_goto("e3", 1);
    mount.hold_reply(":SMTI");
    auto slew = std::async(std::launch::async, [&] {
        if (use_target)
            driver->slew_to_target();
        else
            driver->slew_to_coordinates(6.0, 25.0);
    });
    // No fatal assertions until the synchronous caller has been released and joined.
    CHECK(mount.wait_until_held(std::chrono::seconds(5)));
    CHECK(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    rig.clock.advance(std::chrono::milliseconds(5500));
    mount.release();
    CHECK(mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    CHECK(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    CHECK(rig.clock.wait_for_waiters(3, std::chrono::seconds(5)));
    CHECK_NOTHROW(driver->set_connected(true));
    rig.clock.advance(std::chrono::milliseconds(200));  // synchronous reader wakes; retry is still pending
    CHECK(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    CHECK(mount.count(":MS") == 1);
    CHECK(slew.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    rig.clock.advance(std::chrono::milliseconds(100));
    CHECK(mount.wait_for_count(":MS", 2, std::chrono::seconds(5)));
    CHECK_NOTHROW(driver->command_string(":GVP", false));
    for (int step = 0; step < 2 && slew.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready; ++step) {
        CHECK(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
        rig.clock.advance(step == 0 ? std::chrono::milliseconds(5500) : std::chrono::milliseconds(250));
        CHECK(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    }
    const bool completed = slew.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    driver->set_connected(false);
    if (!completed) {
        static_cast<void>(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
        rig.clock.advance(std::chrono::seconds(1));
    }
    CHECK(completed);
    REQUIRE_NOTHROW(slew.get());
}

TEST_CASE("ZWO moving status remains visible when coordinate reads fail", "[zwo][telescope][async][arrival]") {
    Rig rig;
    rig.connect(std::chrono::seconds(5));
    REQUIRE_FALSE(rig.driver->get_slewing());
    rig.mount.set_moving(true);
    rig.mount.override_reply(":GR", "bad");
    rig.tick();
    CHECK(rig.driver->get_slewing());
    CHECK(alpacacore::test::settle_connected(*rig.driver, false));
}

TEST_CASE("ZWO Unpark during GOTO setup cancels the pending retry", "[zwo][telescope][async][goto-setup]") {
    Rig rig;
    rig.connect();
    rig.mount.reject_goto("e3", 1);
    rig.driver->slew_to_coordinates_async(6.0, 25.0);
    REQUIRE(rig.mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
    rig.driver->unpark();
    rig.tick(std::chrono::milliseconds(5500));
    CHECK(rig.mount.count(":MS") == 1);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(alpacacore::test::settle_connected(*rig.driver, false));
}

TEST_CASE("ZWO slow setup retries complete after an accepted GOTO", "[zwo][telescope][async][goto-setup]") {
    const std::string rejection = GENERATE("e3", "e5", "e7");
    CAPTURE(rejection);
    Rig rig;
    rig.connect();
    rig.mount.reject_goto(rejection, 1);
    rig.mount.hold_reply(":Sr");
    rig.driver->slew_to_coordinates_async(6.0, 25.0);
    const bool held = rig.mount.wait_until_held(std::chrono::seconds(5));
    CHECK(held);
    rig.clock.advance(std::chrono::milliseconds(5500));
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount.count(":MS") == 0);
    rig.mount.release();
    REQUIRE(rig.mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    if (rejection == "e3") {
        REQUIRE(rig.clock.wait_for_waiters(2, std::chrono::seconds(5)));
        rig.tick(std::chrono::milliseconds(300));
    }
    REQUIRE(rig.mount.wait_for_count(":MS", 2, std::chrono::seconds(5)));
    REQUIRE_NOTHROW(rig.driver->command_string(":GVP", false));
    rig.await_poll_resumed();
    rig.tick(std::chrono::milliseconds(5500));
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(std::abs(rig.driver->get_right_ascension() - 6.0) < 1e-9);
    CHECK(std::abs(rig.driver->get_declination() - 25.0) < 1e-9);
    CHECK(alpacacore::test::settle_connected(*rig.driver, false));
}

TEST_CASE("ZWO slow setup keeps startup protection after MS acceptance", "[zwo][telescope][async][goto-setup]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    rig.mount.set_slew_duration(std::chrono::seconds(1));
    rig.mount.hold_reply(":Sr");
    rig.driver->slew_to_coordinates_async(6.0, 25.0);
    REQUIRE(rig.mount.wait_until_held(std::chrono::seconds(5)));
    rig.tick(std::chrono::milliseconds(5500));  // the initiator's original force window expires during setup
    rig.mount.release();
    REQUIRE(rig.mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE_NOTHROW(rig.driver->command_string(":GVP", false));

    // Model one stopped status reply before the mount reports its accepted GOTO as moving.
    const int pairs_before = rig.mount.count(":GD");
    rig.mount.override_reply(":GU", "NG", 1);
    rig.await_poll_resumed();
    REQUIRE(rig.mount.count(":GD") > pairs_before);
    CHECK(rig.driver->get_slewing());

    rig.tick(std::chrono::milliseconds(4500));
    CHECK_FALSE(rig.mount.slewing());  // even a fast arrival stays protected until the new five-second deadline
    CHECK(rig.driver->get_slewing());
    rig.tick(std::chrono::milliseconds(500));
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(std::abs(rig.driver->get_right_ascension() - 6.0) < 1e-9);
    CHECK(std::abs(rig.driver->get_declination() - 25.0) < 1e-9);
    CHECK(alpacacore::test::settle_connected(*rig.driver, false));
}

TEST_CASE("ZWO horizon rejection never retries with mirrored longitude", "[zwo][telescope][async][longitude]") {
    const bool west = GENERATE(false, true);
    CAPTURE(west);
    const double longitude = west ? -100.5 : 100.5;
    const std::string wire_site = west ? "+30*00:00&+100*30:00" : "+30*00:00&-100*30:00";
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    mount.override_reply(":GMGE", wire_site);  // the mount keeps reporting this site, whatever is written to it
    mount.reject_goto("e5");
    rig.connect();
    const auto site_writes = [&](bool wrong_only) {
        int total = 0;
        for (const auto& command : mount.commands()) {
            if (!command.starts_with(":SMGE")) continue;
            if (!wrong_only || command != ":SMGE" + wire_site) ++total;
        }
        return total;
    };
    const int writes_before = site_writes(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::string error;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &error) == SlewingRead::Threw; }, 10000));
    CHECK(error.find("e5") != std::string::npos);
    CHECK(mount.count(":MS") == 2);
    CHECK(site_writes(false) >= writes_before + 2);
    CHECK(site_writes(true) == 0);
    CHECK(std::abs(driver->get_site_longitude() - longitude) < 1e-9);
    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO async - a GOTO the mount rejects surfaces through Slewing (#575)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.reject_goto("2");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS") >= 1; }, 5000));  // ":MS" went out on the GOTO thread

    // Today: "GOTO failed: Mount rejected GOTO request" in the log, then
    // Slewing=false after the 5 s force window. Must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // Only Slewing reports the failure: a rejected GOTO must not turn a
    // position read into that error.
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
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    // The mount would reject the GOTO, but the client aborts while the setup
    // thread is still writing the target: the abort cancels the setup, so the
    // rejection that would have followed must never be recorded.
    mount.reject_goto("2");
    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // open-astro#720: abort while the ":Sr" is in flight. The fence lets that
    // one round trip finish and nothing after it, and nothing at all once
    // AbortSlew has returned.
    REQUIRE(wait_until([&] { return mount.count(":Sr") > 0; }, 5000));
    REQUIRE_NOTHROW(driver->abort_slew());
    const int setup_at_return = setup_count(mount);
    // Outlast the setup (site sync + delayed ":Sr" + ":Sd" + ":MS").
    CHECK_FALSE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 500));
    CHECK(mount.count(":MS") == 0);  // the aborted GOTO was never sent
    CHECK(setup_sent_after_return(mount, setup_at_return) == 0);
    CHECK(mount.count(":Sd") == 0);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - a MoveAxis during GOTO setup owns the error slot (#575)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    // A newer initiator (here MoveAxis) lands while the GOTO setup thread is
    // still writing the target; the superseded GOTO's later rejection is not
    // the jog's failure and must not make Slewing throw during the jog.
    mount.reject_goto("2");
    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->move_axis(0, 1.0); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 500));
    // open-astro#720: the jog fences the pending GOTO, so its ":MS" never
    // lands on top of the jog.
    CHECK(mount.count(":MS") == 0);
    CHECK(jog_count(mount) == 1);
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

// open-astro#720: every initiator that commands the mount fences a GOTO body
// that is still writing the target (revoke its claim, wait for the call in
// flight) before it sends its own command. The GOTO here would be accepted, so
// a ":MS" on the wire is the stale GOTO overriding the newer command.
TEST_CASE("ZWO async - Park during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":Sr") > 0; }, 5000));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->park(); }));
    const int setup_at_return = setup_count(mount);
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(mount));
    CHECK(mount.count(":MS") == 0);
    CHECK(mount.count(":hP") == 1);
    CHECK(setup_sent_after_return(mount, setup_at_return) == 0);
    CHECK(mount.count(":Sd") == 0);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - FindHome during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->find_home(); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(mount));
    CHECK(mount.count(":MS") == 0);
    CHECK(mount.count(":hC") == 1);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - MoveAxis stop during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->move_axis(0, 0.0); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(mount));
    CHECK(mount.count(":MS") == 0);
    CHECK(count_exact(mount, ":Q") == 1);
    driver->set_connected(false);
}

// open-astro#720: the cancelled GOTO never slewed, so it must not leave its
// 5 s Slewing force window behind or report the abandoned target as position.
TEST_CASE("ZWO async - a GOTO cancelled during setup leaves no slew bookkeeping (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(goto_sent_within_setup_window(mount));
    // Still inside the GOTO's force window, but nothing is moving.
    rig.await_poll_resumed();
    CHECK(read_slewing(*driver) == SlewingRead::False);
    rig.tick(std::chrono::milliseconds(5500));  // past the force window
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK(std::fabs(driver->get_right_ascension() - 12.0) < 1e-6);
    CHECK(std::fabs(driver->get_declination() - 45.0) < 1e-6);
    driver->set_connected(false);
}

// open-astro#720: the ":Sr" the canceller waits for comes back rejected, so the
// cancelled GOTO leaves through its outer catch; that exit abandons it too.
TEST_CASE("ZWO async - a cancelled GOTO whose setup then fails leaves no slew bookkeeping (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    mount.override_reply(":Sr", "0");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // Cancel while the ":Sr" is in flight, not at an earlier checkpoint.
    REQUIRE(wait_until([&] { return mount.count(":Sr") > 0; }, 5000));
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(goto_sent_within_setup_window(mount));
    rig.await_poll_resumed();
    CHECK(read_slewing(*driver) == SlewingRead::False);
    rig.tick(std::chrono::milliseconds(5500));  // past the force window
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK(std::fabs(driver->get_right_ascension() - 12.0) < 1e-6);
    CHECK(std::fabs(driver->get_declination() - 45.0) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - Disconnect during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":Sr", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":Sr") > 0; }, 5000));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->set_connected(false); }));
    const int setup_at_return = setup_count(mount);
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(mount));
    CHECK(mount.count(":MS") == 0);
    CHECK(setup_sent_after_return(mount, setup_at_return) == 0);
    CHECK(mount.count(":Sd") == 0);
}

// open-astro#720: the cancel lands while the site/time sync's ":SMTI" is in
// flight, so the GOTO must stop there: no target write follows the sync,
// before or after Connected=false returns.
TEST_CASE("ZWO async - Disconnect during the GOTO's site/time sync sends no target (#720)",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.delay_next(":SMTI", std::chrono::milliseconds(150));
    const int smti_before = mount.count(":SMTI");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":SMTI") > smti_before; }, 5000));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->set_connected(false); }));
    const int setup_at_return = setup_count(mount);
    CHECK(elapsed < kInitiatorBudget);
    CHECK(setup_sent_after_return(mount, setup_at_return) == 0);
    CHECK(mount.count(":Sr") == 0);
    CHECK(mount.count(":Sd") == 0);
    CHECK(mount.count(":MS") == 0);
}

TEST_CASE("ZWO async - position reports mount feedback after GOTO", "[zwo][telescope][async][position]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    // The mount lands 1 degree short in Dec: the driver reports where the mount is, not the target it was given.
    mount.set_land_short_by(3600.0);
    CHECK(std::abs(driver->get_right_ascension() - 12.0) < 1e-6);
    CHECK(std::abs(driver->get_declination() - 45.0) < 1e-6);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(6.0, 22.0));
    REQUIRE(wait_until([&] { return mount.count(":MS") == 1; }, 5000));
    REQUIRE_NOTHROW(driver->command_string(":GVP", false));
    rig.await_poll_resumed();
    rig.tick(std::chrono::milliseconds(5500));
    REQUIRE_FALSE(driver->get_slewing());

    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 1e-6);
    CHECK(std::abs(driver->get_declination() - 21.0) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("ZWO blocking slew - position reports mount feedback after GOTO", "[zwo][telescope][position]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    auto& clock = rig.clock;
    rig.connect();

    mount.set_land_short_by(3600.0);
    auto slew = std::async(std::launch::async, [&] { driver->slew_to_coordinates(4.0, -12.0); });
    const bool sent = wait_until([&] { return mount.count(":MS") == 1; }, 5000);
    CHECK(sent);
    CHECK_NOTHROW(driver->command_string(":GVP", false));
    try {
        rig.await_poll_resumed();
    } catch (...) {
        driver->set_connected(false);
        static_cast<void>(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::seconds(1));
        throw;
    }
    bool settled = true;
    for (int step = 0; step < 2 && slew.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready; ++step) {
        // The telemetry worker and synchronous slew each park on the fake clock.
        if (!clock.wait_for_waiters(2, std::chrono::seconds(5))) {
            if (slew.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) break;
            settled = false;
            break;
        }
        clock.advance(step == 0 ? std::chrono::milliseconds(5500) : std::chrono::milliseconds(250));
        if (!clock.wait_for_woken_settled(std::chrono::seconds(5))) {
            settled = false;
            break;
        }
    }
    const bool completed = slew.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!completed) {
        driver->set_connected(false);
        static_cast<void>(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::seconds(1));  // Release the synchronous sleep before joining on failure.
    }
    CHECK(settled);
    CHECK(completed);
    REQUIRE_NOTHROW(slew.get());

    CHECK(std::abs(driver->get_right_ascension() - 4.0) < 1e-6);
    CHECK(std::abs(driver->get_declination() - -13.0) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - redundant Connected=true preserves a stored slew failure",
          "[zwo][telescope][async][slewfailure]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.override_reply(":Sr", "0");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 5000));
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // Platform 7 may resend Connected=true while already connected. That is
    // only a refresh, not a successful motion recovery.
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // A full successful reconnect is a valid recovery and clears the old error.
    mount.override_reply(":Sr", "", 0);
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const SlewingRead after_reconnect = read_slewing(*driver);
    CHECK((after_reconnect == SlewingRead::True || after_reconnect == SlewingRead::False));
    driver->set_connected(false);
}

// Operation slot, GOTO family: a new GOTO replaces the previous body without waiting for it. The previous body sits
// in a mount transaction that has not been answered; the HTTP thread must not wait for that answer.
// Falsified by: zwo_telescope_driver.cpp slew_to_target_async() calling goto_slot_.cancel_all_and_join() before
// goto_slot_.start(), so the second call returns only after the held ":Sr" is answered or times out.
TEST_CASE("ZWO async - a second GOTO returns while the first body is blocked in a mount transaction",
          "[zwo][telescope][async][operation-slot]") {
    Rig rig(5000);
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.hold_reply(":Sr");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));

    auto second = std::async(std::launch::async, [&] { driver->slew_to_coordinates_async(6.5, 30.0); });
    // Real-time bound on a thread handoff, not a driver timer: a join before the start waits for the 5 s link timeout.
    const bool returned = second.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready;
    mount.release();  // before any REQUIRE can unwind, so the future's destructor never waits on the held reply
    second.wait();
    CHECK(returned);

    // The replaced body ends without sending a target or recording a failure; only the second GOTO reaches ":MS".
    REQUIRE(mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    CHECK(wait_until([&] { return read_slewing(*driver) == SlewingRead::True; }, 5000));
    CHECK(mount.count(":MS") == 1);
    CHECK(read_slewing(*driver) != SlewingRead::Threw);
    driver->set_connected(false);
}

// Decision "disconnect sends no :Q#": Connected=false leaves an accepted GOTO running on the mount. A PIN: it passes
// on the thread-based code and must keep passing on the slot-based teardown.
// Falsified by: the disconnect teardown calling protocol_.abort_motion() (a ":Q" on Connected=false).
TEST_CASE("ZWO async - Disconnect during a slew sends no stop and leaves the mount slewing (PIN)",
          "[zwo][telescope][async][operation-slot]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_for_count(":MS", 1, std::chrono::seconds(5)));
    REQUIRE(mount.slewing());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(count_exact(mount, ":Q") == 0);
    CHECK(mount.slewing());
}

// open-astro#720 through the send gate: Park or a jog that lands while the GOTO body waits for the mount's ":Sr" reply
// waits for that one exchange, then sends its own command, and nothing the old body would send follows it. A PIN: the
// join on the thread-based code gives the same order and the gate must keep it.
// Falsified by: zwo_telescope_driver.cpp send_goto_locked(): dropping both ownership terms of the gate (goto_claim_ ==
// claim and ctx.stop_reason() == StopReason::None, leaving connected_), so the fenced body still sends ":Sd" and
// ":MS". Either term alone refuses ":Sd": the fence zeroes the claim and also cancels the slot.
TEST_CASE("ZWO async - Park or a jog during a held GOTO target write waits one exchange and sends after it (PIN)",
          "[zwo][telescope][async][operation-slot]") {
    const bool park = GENERATE(true, false);
    CAPTURE(park);
    Rig rig(5000);
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    const auto is_fence = [&](const std::string& command) {
        return park ? command == ":hP" : (command.starts_with(":Me") || command.starts_with(":Mw"));
    };
    const auto fence_sent = [&] {
        const auto all = mount.commands();
        return std::any_of(all.begin(), all.end(), is_fence);
    };

    mount.hold_reply(":Sr");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));
    auto fence = std::async(std::launch::async, [&] {
        if (park) {
            driver->park();
        } else {
            driver->move_axis(0, 1.0);
        }
    });
    // The fencing command queues behind the exchange in flight and never reaches the mount before it ends.
    const bool sent_while_held = wait_until(fence_sent, 200);
    mount.release();  // before any REQUIRE can unwind, so the future's destructor never waits on the held reply
    const bool returned = fence.wait_for(kInitiatorBudget) == std::future_status::ready;
    fence.wait();
    CHECK_FALSE(sent_while_held);
    CHECK(returned);
    const int setup_at_return = setup_count(mount);
    CHECK(setup_sent_after_return(mount, setup_at_return) == 0);

    const auto all = mount.commands();
    const auto fence_at = std::find_if(all.begin(), all.end(), is_fence);
    REQUIRE(fence_at != all.end());
    const auto sr_at =
        std::find_if(all.begin(), all.end(), [](const std::string& command) { return command.starts_with(":Sr"); });
    CHECK(sr_at < fence_at);
    CHECK(mount.count(":Sd") == 0);
    CHECK(mount.count(":MS") == 0);
    if (!park) REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

namespace {

// A latch a test seam blocks on. The wait has a real-time bound, so a regression fails the case instead of hanging it.
struct Latch {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;

    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        static_cast<void>(cv.wait_for(lock, std::chrono::seconds(2), [&] { return open; }));
    }
    bool wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

std::thread refuse_thread(std::function<void()>) {
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
}

}  // namespace

// open-astro#720, the drain: a GOTO call the send gate has counted (here ":Sr", stopped by the seam after the count
// and before the send) lands before the fencing command, which waits for it instead of overtaking it.
// Falsified by: zwo_telescope_driver.cpp fence_goto(): removing the goto_send_cv_ drain (the wait for
// goto_sends_in_flight_ == 0), so ":hP" reaches the mount while the counted ":Sr" is still unsent.
TEST_CASE("ZWO async - a fence waits for the GOTO call the send gate has counted",
          "[zwo][telescope][async][operation-slot]") {
    Latch latch;
    Rig rig(5000);
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    alpacacore::vendor::zwo::set_goto_send_hook_for_testing(*driver, [&](std::string_view label) {
        if (label == ":Sr") latch.block();
    });
    rig.connect();

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(latch.wait_entered());
    auto park = std::async(std::launch::async, [&] { driver->park(); });
    const bool sent_while_counted = mount.wait_for_count(":hP", 1, std::chrono::milliseconds(200));
    latch.release();  // before any REQUIRE can unwind, so the future's destructor never waits on the latch
    const bool returned = park.wait_for(kInitiatorBudget) == std::future_status::ready;
    park.wait();
    CHECK_FALSE(sent_while_counted);
    CHECK(returned);

    const auto all = mount.commands();
    const auto sr_at = std::find_if(all.begin(), all.end(), [](const std::string& c) { return c.starts_with(":Sr"); });
    const auto park_at = std::find(all.begin(), all.end(), std::string(":hP"));
    REQUIRE(sr_at != all.end());
    REQUIRE(park_at != all.end());
    CHECK(sr_at < park_at);
    CHECK(mount.count(":Sd") == 0);
    CHECK(mount.count(":MS") == 0);
    driver->set_connected(false);
}

// decision 0006 "Thread refusal": a second GOTO whose body the slot cannot start supersedes the first (held on ":Sr")
// and publishes the stopped state itself: Slewing false, and the poll it paused reads status again.
// Falsified by: zwo_telescope_driver.cpp slew_to_target_async(): removing the poll_pause_ release in the refused-start
// catch, so the poll stays paused and ":GU" never grows.
TEST_CASE("ZWO async - a refused GOTO start publishes the stopped state", "[zwo][telescope][async][operation-slot]") {
    Rig rig(5000);
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    int spawns = 0;
    alpacacore::vendor::zwo::set_goto_spawn_for_testing(*driver, [&](std::function<void()> body) {
        return ++spawns == 1 ? std::thread(std::move(body)) : refuse_thread(std::move(body));
    });
    rig.connect();

    mount.hold_reply(":Sr");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));
    bool refused = false;
    try {
        driver->slew_to_coordinates_async(6.5, 30.0);
    } catch (const alpacacore::AlpacaException& ex) {
        refused = ex.error_code() == alpacacore::AlpacaError::DriverException;
    }
    mount.release();
    CHECK(refused);

    const int status = mount.count(":GU");
    rig.tick();
    CHECK(mount.wait_for_count(":GU", status + 1, std::chrono::seconds(5)));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK(mount.count(":MS") == 0);
    driver->set_connected(false);
}

// Round 4 finding 1 (b): a redundant Connected=true that lands while a GOTO's start is being refused leaves the
// GOTO's claim alone, so the refusal still publishes the stopped state.
// Falsified by: zwo_telescope_driver.cpp set_connected(): the connect reset (reset_session_state_for_connect) setting
// goto_claim_ = 0, so the refusal skips its publish and Slewing stays true.
TEST_CASE("ZWO async - a redundant Connected=true during a refused GOTO start keeps the stopped-state publish",
          "[zwo][telescope][async][operation-slot]") {
    Latch latch;
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    alpacacore::vendor::zwo::set_goto_spawn_for_testing(*driver, [&](std::function<void()> body) {
        latch.block();
        return refuse_thread(std::move(body));
    });
    rig.connect();

    auto slew = std::async(std::launch::async, [&] { driver->slew_to_coordinates_async(5.5, 20.0); });
    REQUIRE(latch.wait_entered());
    driver->set_connected(true);
    latch.release();
    bool refused = false;
    try {
        slew.get();
    } catch (const alpacacore::AlpacaException& ex) {
        refused = ex.error_code() == alpacacore::AlpacaError::DriverException;
    }
    CHECK(refused);

    const int status = mount.count(":GU");
    rig.tick();
    CHECK(mount.wait_for_count(":GU", status + 1, std::chrono::seconds(5)));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK(mount.count(":Sr") == 0);
    driver->set_connected(false);
}

// A GOTO fenced before ":MS" leaves no paused poll behind: the fence releases the pause the slew set, because the
// fenced body no longer owns the driver state and writes none of it.
// Falsified by: zwo_telescope_driver.cpp fence_goto(): removing poll_pause_.store(false), so ":GU" never grows.
TEST_CASE("ZWO async - AbortSlew during a held GOTO target write resumes the poll",
          "[zwo][telescope][async][operation-slot]") {
    Rig rig(5000);
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.hold_reply(":Sr");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));
    auto abort = std::async(std::launch::async, [&] { driver->abort_slew(); });
    static_cast<void>(wait_until([&] { return mount.count(":Q") > 0; }, 200));
    mount.release();
    const bool returned = abort.wait_for(kInitiatorBudget) == std::future_status::ready;
    abort.wait();
    CHECK(returned);

    const int status = mount.count(":GU");
    rig.tick();
    CHECK(mount.wait_for_count(":GU", status + 1, std::chrono::seconds(5)));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK(mount.count(":MS") == 0);
    driver->set_connected(false);
}

// The e3 stop goes through the send gate too: a jog that fences a GOTO whose ":MS" is on the wire waits for that
// exchange; the mount answers e3 (busy), and the fenced body sends no ":Q" on top of the jog. A PIN: the thread-based
// code checked its cancel flag before the stop.
// Falsified by: zwo_telescope_driver.cpp run_goto(): the e3 path calling protocol.abort_motion() directly instead of
// through send(":Q", ...), so a ":Q" follows the jog.
TEST_CASE("ZWO async - a GOTO fenced while its :MS is answered e3 sends no stop (PIN)",
          "[zwo][telescope][async][operation-slot]") {
    Rig rig;
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.reject_goto("e3", 1);
    mount.delay_next(":MS", std::chrono::milliseconds(150));
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS") > 0; }, 5000));
    std::chrono::microseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->move_axis(0, 1.0); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(wait_until([&] { return mount.count(":Q") > 0; }, 500));
    CHECK(mount.count(":MS") == 1);
    CHECK(jog_count(mount) == 1);
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

// Blocker 3: a Connected=false while the previous teardown still runs reaps it before it starts the next one, so two
// teardowns never run at once, and a repeated Connected=false never throws.
// Falsified by: zwo_telescope_driver.cpp set_connected(): starting the teardown without
// teardown_slot_.cancel_all_and_join() first, so a second teardown starts while the first still runs.
TEST_CASE("ZWO async - a repeated Connected=false waits for the running teardown and never throws",
          "[zwo][telescope][async][operation-slot]") {
    Latch latch;
    std::atomic<int> spawns{0};
    Rig rig;
    auto& driver = rig.driver;
    alpacacore::vendor::zwo::set_teardown_spawn_for_testing(*driver, [&](std::function<void()> body) {
        const int n = ++spawns;
        return std::thread([&latch, n, body = std::move(body)] {
            if (n == 1) latch.block();
            body();
        });
    });
    rig.connect();

    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE(latch.wait_entered());  // the first teardown is held before its body
    auto second = std::async(std::launch::async, [&] { driver->set_connected(false); });
    const bool returned_while_running = second.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    const int spawns_while_running = spawns.load();
    latch.release();  // before any REQUIRE can unwind, so the future's destructor never waits on the latch
    REQUIRE_NOTHROW(second.get());
    CHECK_FALSE(returned_while_running);
    CHECK(spawns_while_running == 1);
    CHECK(spawns.load() == 2);
    CHECK_FALSE(driver->get_connected());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_connected(false);
}

// Round 2 finding 4, on the slot design: AbortSlew fences the held GOTO and Connected=false fences and reaps it, in
// either order. Neither waits past one exchange, nothing the GOTO would send follows either call, and a reconnect
// finds the slots empty. It can pass by accident and never fails by accident; under ThreadSanitizer
// (sanitizers-tsan, [stress-guard]) it also checks the reapers.
// Falsified by: zwo_telescope_driver.cpp abort_slew(): reaping the GOTO slot itself (goto_slot_.cancel_all_and_join()
// after fence_goto(), a reaper outside set_connected_mutex_ that races disconnect's reap, failure 0011).
TEST_CASE("ZWO async - AbortSlew and Connected=false together during a held GOTO target write",
          "[zwo][telescope][async][operation-slot][stress-guard]") {
    const bool abort_first = GENERATE(true, false);
    CAPTURE(abort_first);
    Rig rig(5000);
    auto& mount = rig.mount;
    auto& driver = rig.driver;
    rig.connect();

    mount.hold_reply(":Sr");
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));
    const std::function<void()> abort = [&] {
        try {
            driver->abort_slew();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // NotConnected once the disconnect won the race.
        }
    };
    const std::function<void()> disconnect = [&] { driver->set_connected(false); };
    auto first = std::async(std::launch::async, abort_first ? abort : disconnect);
    auto second = std::async(std::launch::async, abort_first ? disconnect : abort);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // real time: both calls reach their fences
    mount.release();  // before any REQUIRE can unwind, so the futures' destructors never wait on the held reply
    const bool first_returned = first.wait_for(kInitiatorBudget) == std::future_status::ready;
    const bool second_returned = second.wait_for(kInitiatorBudget) == std::future_status::ready;
    first.wait();
    second.wait();
    CHECK(first_returned);
    CHECK(second_returned);
    const int setup_at_return = setup_count(mount);
    CHECK(setup_sent_after_return(mount, setup_at_return) == 0);
    CHECK(mount.count(":Sd") == 0);
    CHECK(mount.count(":MS") == 0);
    CHECK_FALSE(driver->get_connected());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_connected(false);
}

// The destructor with live bodies: a GOTO held on ":Sr", a pulse drain waiting for the same link and the poll. The
// destructor disconnects (cancelling the held read), fences and reaps the GOTO, then reaps the teardown, the pulse
// drain and the poll, without waiting out the link timeout. Under ThreadSanitizer ([stress-guard]) it also checks
// that no body outlives the members it touches.
// Falsified by: zwo_telescope_driver.cpp set_connected(): Connected=false without protocol_.cancel_pending_io(), so
// the fence waits for the held ":Sr" until the 5 s link timeout.
TEST_CASE("ZWO async - the destructor reaps a held GOTO with a pulse drain and the poll live",
          "[zwo][telescope][async][operation-slot][stress-guard]") {
    Rig rig(5000);
    auto& mount = rig.mount;
    rig.connect();

    mount.hold_reply(":Sr");
    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(mount.wait_until_held(std::chrono::seconds(5)));
    REQUIRE_NOTHROW(rig.driver->pulse_guide(0, 3000));  // the drain queues behind the held exchange
    auto destroy = std::async(std::launch::async, [&] { rig.driver.reset(); });
    const bool destroyed = destroy.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    mount.release();  // before any REQUIRE can unwind, so the future's destructor never waits on the held reply
    destroy.wait();
    CHECK(destroyed);
    CHECK(mount.count(":Sd") == 0);
    CHECK(mount.count(":MS") == 0);
}

#endif  // _WIN32

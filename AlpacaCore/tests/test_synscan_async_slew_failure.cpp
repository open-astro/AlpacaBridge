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

// open-astro#575, SynScan: the async slew task dispatches the GOTO on its own
// thread; a handset that never acknowledges it made the task log "Async slew
// dispatch failed" and drop Slewing to false -- a silent no-op to the client.
// The next Slewing read must return an Alpaca error instead (see
// test_skywatcher_async_slew_failure.cpp for the contract reference).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_synscan_handset.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

using alpacacore::test::require_alpaca_error;

using alpacacore::test::FakeSynScanHandset;
using alpacacore::test::SynScanGotoLanding;
using alpacacore::test::wait_until;

// This file's handset: a GOTO lands on its aim (a case may turn land_on_goto off), and the mount starts at
// 12AB0500,20000500.
std::shared_ptr<FakeSynScanHandset> make_handset(
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    return std::make_shared<FakeSynScanHandset>(clock, SynScanGotoLanding::OnTarget, "12AB0500,20000500#");
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port, int timeout_ms = 1000) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = timeout_ms;
    return info;
}

// Steps the fake clock one `step` at a time, once a driver task is parked on
// it, until `pred` holds or `max_steps` steps pass. Each step waits for the
// woken task to settle, so no step lands between two of its waits.
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred,
                   std::chrono::milliseconds step = std::chrono::milliseconds(250), int max_steps = 200) {
    for (int i = 0; i < max_steps; ++i) {
        if (pred()) return true;
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(step);
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return pred();
}

// Starts a 200 ms East (RA) pulse and waits, in real time, until the pulse's stop body is parked on the clock.
// advance_until() alone meets any clock waiter, and the status poll is always parked there, so on a loaded runner the
// clock could step past the whole IsPulseGuiding window before the body had started its wait. The body then never ran:
// a check that read the handset after the pulse missed its end, or passed without it when the case expected no
// tracking command.
// Each call installs its own hook and names the first thread spawned through it, which is this call's pulse body
// (pulse_guide() spawns one thread per pulse), so a case may call it once per pulse on the same driver. A pulse started
// afterwards with a bare pulse_guide() runs through the stale hook unnamed and gets no rendezvous: start every pulse a
// case steps the clock through with this helper.
void start_ra_pulse_parked(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver) {
    auto body_thread = std::make_shared<std::promise<std::thread::id>>();
    auto body_id = body_thread->get_future();
    auto named = std::make_shared<std::atomic<bool>>(false);
    alpacacore::vendor::synscan::set_pulse_spawn_for_testing(driver, [body_thread, named](std::function<void()> body) {
        std::thread thread(std::move(body));
        if (!named->exchange(true)) {
            body_thread->set_value(thread.get_id());
        }
        return thread;
    });
    driver.pulse_guide(2, 200);  // East, RA
    REQUIRE(body_id.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(clock.wait_for_waiter_thread(body_id.get(), std::chrono::milliseconds(5000)));
}

// Steps the clock until the pulse is over: its IsPulseGuiding window has closed and `ended` (the command that ends the
// pulse has reached the handset) holds, so a slow stop exchange cannot be read half done.
bool advance_until_pulse_ended(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                               const std::function<bool()>& ended) {
    return advance_until(clock, [&] { return !driver.get_is_pulse_guiding() && ended(); });
}

// The driver on a caller-owned task clock, so a case steps its waits with advance_until() instead of sleeping.
std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int port, alpacacore::util::TaskClock& clock,
                                                         int timeout_ms = 1000) {
    return alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(port, timeout_ms), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
}

// Runs a blocking driver call on its own thread, stepping the clock until it returns, then rethrows what it threw.
// `started` (optional) must hold, on real time, before the first step: a task parked on the clock from before the
// call (a pulse timer the call reaps) would otherwise satisfy the rendezvous while the call is still on the socket,
// and virtual time would run ahead of it.
template <typename Fn>
void run_virtual(alpacacore::test::FakeTaskClock& clock, Fn&& fn, const std::function<bool()>& started = nullptr) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        done.store(true);
    });
    const bool began = !started || wait_until([&] { return started() || done.load(); }, 5000);
    const bool finished = began && advance_until(clock, [&] { return done.load(); });
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));  // release a hung caller (it may park again) so the thread can join
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    caller.join();
    REQUIRE(began);
    REQUIRE(finished);
    if (error) std::rethrow_exception(error);
}

bool is_variable_rate_command(const std::string& command, int device) {
    return command.size() >= 6 && command[0] == 'P' && command[1] == 3 &&
           static_cast<unsigned char>(command[2]) == device;
}

bool is_zero_rate_command(const std::string& command) {
    return command.size() >= 6 && command[4] == 0 && command[5] == 0;
}

// Zero-rate RA (device 16) commands in the handset's log: how an RA pulse ends while the mount is not tracking.
int zero_rate_ra_commands(FakeSynScanHandset& st) {
    const auto commands = st.command_snapshot();
    return static_cast<int>(std::count_if(commands.begin(), commands.end(), [](const std::string& command) {
        return is_variable_rate_command(command, 16) && is_zero_rate_command(command);
    }));
}

int last_goto_index(const std::vector<std::string>& commands) {
    int result = -1;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (!commands[i].empty() && (commands[i][0] == 'r' || commands[i][0] == 'R')) {
            result = static_cast<int>(i);
        }
    }
    return result;
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

TEST_CASE("SynScan async - a GOTO the handset never acknowledges surfaces through Slewing (#575)",
          "[synscan][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // True: published before the task ran. Threw: the task won mutex_ first
    // and its dispatch already failed -- never a silent False.
    const SlewingRead first = read_slewing(*driver);
    REQUIRE((first == SlewingRead::True || first == SlewingRead::Threw));

    // The dispatch times out on the task thread. Today Slewing then reads
    // FALSE as if the mount had arrived; it must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // A retried initiator against a responsive handset clears it.
    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(wait_until([&] { return st->goto_count.load() == 1; }, 5000));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 15000));
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - failed dispatch preserves a stored slew error",
          "[synscan][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    st->reject_goto.store(false);
    REQUIRE(server.drop_connections());
    try {
        driver->move_axis(0, 1.0);
        FAIL("MoveAxis returned although the handset did not answer");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - successful dispatch clears a stored slew error",
          "[synscan][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->move_axis(0, 1.0));
    CHECK(driver->get_slewing());
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan Unpark - a no-op Unpark clears a stored slew error", "[synscan][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->unpark());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - AbortSlew clears a stored slew failure (#575)", "[synscan][telescope][async][slewfailure]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - AbortSlew fences a pending GOTO", "[synscan][telescope][async][abort]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    for (int i = 0; i < 50; ++i) {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
        REQUIRE_NOTHROW(driver->abort_slew());
        const int at_abort_return = st->goto_count.load();
        // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        INFO("iteration " << i << ", GOTO count at abort return=" << at_abort_return);
        CHECK(st->goto_count.load() == at_abort_return);
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - a stop failure still reaps the cancelled slew task",
          "[synscan][telescope][async][abort]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(server.drop_connections());
    require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::DriverException);

    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE_NOTHROW(driver->set_connected(true));
    const int before = st->goto_count.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(wait_until([&] { return st->goto_count.load() > before; }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - a failed tracking restore still clears Slewing (#830)",
          "[synscan][telescope][async][abort]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_tracking(true));
    REQUIRE_NOTHROW(driver->move_axis(0, 1.0));
    REQUIRE(driver->get_slewing());

    st->reject_tracking.store(true);
    require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::DriverException);
    st->reject_tracking.store(false);
    CHECK(driver->get_slewing() == false);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - MoveAxis supersedes a pending async slew", "[synscan][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(5.5, 20.0);
    driver->move_axis(0, 0.0);
    // Virtual time for the superseded slew task to wind down.
    clock.advance(std::chrono::milliseconds(100));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    const auto settled = st->command_snapshot();
    const int goto_index = last_goto_index(settled);
    const int stop_index = [&] {
        for (std::size_t i = 0; i < settled.size(); ++i) {
            if (is_variable_rate_command(settled[i], 16) && is_zero_rate_command(settled[i])) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }();
    REQUIRE(stop_index >= 0);
    CHECK((goto_index == -1 || goto_index < stop_index));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan async - blocking slew replaces a pending async target", "[synscan][telescope][async][ownership]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->slew_to_coordinates_async(5.5, 20.0);
    std::atomic<bool> sync_done{false};
    std::thread syncer([&] {
        driver->slew_to_coordinates(6.0, 22.0);
        sync_done.store(true);
    });
    const bool finished = advance_until(clock, [&] { return sync_done.load(); });
    syncer.join();
    REQUIRE(finished);
    auto commands = st->command_snapshot();
    int final_goto = last_goto_index(commands);
    REQUIRE(final_goto >= 0);
    const std::string replacement = commands[static_cast<std::size_t>(final_goto)];
    CHECK(replacement.find(',') != std::string::npos);
    // Virtual time for any stray tail of the replaced task.
    clock.advance(std::chrono::milliseconds(100));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    commands = st->command_snapshot();
    final_goto = last_goto_index(commands);
    REQUIRE(final_goto >= 0);
    CHECK(commands[static_cast<std::size_t>(final_goto)] == replacement);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - cross-axis pulses keep the RA tracking restore", "[synscan][telescope][pulseguiding]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);

    const auto started = std::chrono::steady_clock::now();
    driver->pulse_guide(2, 1500);  // East, RA
    driver->pulse_guide(0, 300);   // North, Dec; must not cancel the RA timer
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 5000));
    // The RA pulse ran its whole 1.5 s: a cancelled timer would also end it with T, but early.
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1500));

    const auto commands = st->command_snapshot();
    int last_ra = -1;
    bool tracking_restored_after_ra_stop = false;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (is_variable_rate_command(commands[i], 16)) {
            last_ra = static_cast<int>(i);
            tracking_restored_after_ra_stop = false;
        } else if (last_ra >= 0 && commands[i][0] == 'T') {
            tracking_restored_after_ra_stop = true;
        }
    }
    REQUIRE(last_ra >= 0);
    // An RA move replaces tracking on the handset and ends with the tracking command, not a stop (#1030).
    CHECK_FALSE(is_zero_rate_command(commands[static_cast<std::size_t>(last_ra)]));
    CHECK(tracking_restored_after_ra_stop);
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - short Dec pulse keeps the position override of a long RA pulse (#990)",
          "[synscan][telescope][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The fake mount never moves, so any difference from this reading is the private estimate.
    const double live_ra = driver->get_right_ascension();
    driver->pulse_guide(2, 1500);  // East, RA: the estimate is held until about 5.5 s
    driver->pulse_guide(0, 100);   // North, Dec: its own hold would end at about 4.1 s
    clock.wait_for_waiters(2, std::chrono::seconds(5));  // both pulse timers are parked
    clock.advance(std::chrono::milliseconds(4500));
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    // Past the Dec pulse's hold, inside the RA pulse's: the RA estimate must still be served.
    CHECK(std::abs(driver->get_right_ascension() - live_ra) > 1e-6);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - guide position does not publish as target", "[synscan][telescope][pulseguiding]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->pulse_guide(0, 150);
    require_alpaca_error([&] { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&] { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    driver->set_target_right_ascension(5.5);
    driver->set_target_declination(20.0);
    driver->pulse_guide(1, 150);
    CHECK(driver->get_target_right_ascension() == 5.5);
    CHECK(driver->get_target_declination() == 20.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan async slew - position reports mount feedback after PulseGuide", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->land_on_goto.store(false);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const double position_before_pulse_ra = driver->get_right_ascension();
    const double position_before_pulse_dec = driver->get_declination();
    driver->pulse_guide(0, 1000);  // Establish a distinct private guide-position estimate.
    REQUIRE(driver->get_is_pulse_guiding());

    constexpr double target_ra = 6.0;
    constexpr double target_dec = 22.0;
    const bool position_is_distinct =
        std::abs(position_before_pulse_ra - target_ra) > 0.1 || std::abs(position_before_pulse_dec - target_dec) > 1.0;
    REQUIRE(position_is_distinct);
    driver->slew_to_coordinates_async(target_ra, target_dec);
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    // The mount never moves, so the slew ends with the far-miss failure after one repeat GOTO (#1105).
    REQUIRE(advance_until(clock, [&] { return read_slewing(*driver) != SlewingRead::True; }));

    // The fake mount acknowledges the GOTO but stays at its fixed position.
    // Completion must not make the requested target masquerade as readback.
    CHECK(std::abs(driver->get_right_ascension() - position_before_pulse_ra) < 1e-6);
    CHECK(std::abs(driver->get_declination() - position_before_pulse_dec) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("SynScan - get_link_fault does not wait on a connect holding the driver mutex",
          "[synscan][telescope][link]") {
    // The echo passes, then the handset goes silent: every later handshake query burns its
    // response timeout while set_connected(true) holds the coarse mutex_. The management
    // listing polls get_link_fault() and must not queue behind that.
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (!chunk.empty() && chunk[0] == 'K') {
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        }
        return "";
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    driver->connect();
    // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // inside the handshake
    REQUIRE(driver->get_connecting());
    const auto t0 = std::chrono::steady_clock::now();
    std::string fault;
    std::thread reader([&] { fault = driver->get_link_fault(); });
    reader.join();
    for (const auto& read : std::vector<std::function<void()>>{[&] { (void)driver->get_right_ascension_rate(); },
                                                               [&] { (void)driver->get_declination_rate(); },
                                                               [&] { (void)driver->get_tracking_rate(); }}) {
        try {
            read();
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        }
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // Well under the multi-second handshake timeout a lock wait would cost,
    // with headroom for the sanitizer builds on a loaded runner.
    CHECK(ms < 1000);
    CHECK(fault.empty());
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }, 30000));
}

TEST_CASE("SynScan pulse state refuses a latched link fault without I/O and recovers",
          "[synscan][telescope][link][pulseguiding]") {
    const bool guiding = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    using alpacacore::vendor::synscan::poll_status_now_for_testing;
    REQUIRE_NOTHROW(driver->set_tracking(true));
    if (guiding) REQUIRE_NOTHROW(driver->pulse_guide(0, 25000));
    REQUIRE(driver->get_is_pulse_guiding() == guiding);
    const double altitude = driver->get_altitude();
    // The connect started the status poll, whose first tick is still queued behind the test's exchanges on the
    // wrapper mutex (not FIFO): unpaused, it can read after the third failure below and count a fourth, or
    // after the unmute and clear the latch (nightly run 40958614341). Pausing joins it.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    st->silent.store(true);
    for (int i = 0; i < 3; ++i) {
        poll_status_now_for_testing(*driver);
    }
    REQUIRE(driver->get_connected());
    const auto fault = driver->get_link_fault();
    REQUIRE_FALSE(fault.empty());
    const int requests = st->requests.load();
    try {
        (void)driver->get_is_pulse_guiding();
        FAIL("A faulted link must not answer from pulse timers");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()) == "SynScan mount communications compromised: " + fault);
    }
    CHECK(st->requests.load() == requests);
    st->silent.store(false);
    poll_status_now_for_testing(*driver);
    CHECK(driver->get_altitude() == altitude);
    CHECK(driver->get_link_fault().empty());
    CHECK(driver->get_is_pulse_guiding() == guiding);
    driver->abort_slew();
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("SynScan commanded state refuses a latched fault while safety stops still reach the handset",
          "[synscan][telescope][link]") {
    const bool moving = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    using alpacacore::vendor::synscan::poll_status_now_for_testing;
    driver->set_tracking(true);
    driver->set_utc_date(std::chrono::system_clock::now());  // UTCDate is not served before the clock is set
    const std::vector<std::pair<const char*, std::function<void()>>> reads{
        {"AtHome", [&] { (void)driver->get_at_home(); }},
        {"AtPark", [&] { (void)driver->get_at_park(); }},
        {"Slewing", [&] { (void)driver->get_slewing(); }},
        {"Tracking", [&] { (void)driver->get_tracking(); }},
        {"DeclinationRate", [&] { (void)driver->get_declination_rate(); }},
        {"RightAscensionRate", [&] { (void)driver->get_right_ascension_rate(); }},
        {"TrackingRate", [&] { (void)driver->get_tracking_rate(); }},
        {"GuideRate", [&] { (void)driver->get_guide_rate(); }},
        {"SideOfPier", [&] { (void)driver->get_side_of_pier(); }},
        {"DestinationSideOfPier", [&] { (void)driver->get_destination_side_of_pier(5.0, 20.0); }},
        {"SiteLatitude", [&] { (void)driver->get_site_latitude(); }},
        {"SiteLongitude", [&] { (void)driver->get_site_longitude(); }},
        {"SiderealTime", [&] { (void)driver->get_sidereal_time(); }},
        {"UTCDate", [&] { (void)driver->get_utc_date(); }},
    };
    for (const auto& [name, read] : reads) {
        INFO(name);
        REQUIRE_NOTHROW(read());
    }
    if (moving) driver->move_axis(1, 0.01);
    REQUIRE(driver->get_slewing() == moving);
    (void)driver->get_altitude();
    // The connect started the status poll, whose first tick is still queued behind the test's exchanges on the
    // wrapper mutex (not FIFO): unpaused, it can read after the third failure below and count a fourth, or
    // after the unmute and clear the latch (nightly run 40958614341). Pausing joins it.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    st->silent.store(true);
    for (int i = 0; i < 3; ++i) {
        poll_status_now_for_testing(*driver);
    }
    const auto fault = driver->get_link_fault();
    REQUIRE_FALSE(fault.empty());
    const int requests = st->requests.load();
    for (const auto& [name, read] : reads) {
        INFO(name);
        try {
            read();
            CHECK(false);
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(e.what()) == "SynScan mount communications compromised: " + fault);
        }
    }
    CHECK(st->requests.load() == requests);
    CHECK(driver->get_connected());
    CHECK(driver->get_can_pulse_guide());
    CHECK_FALSE(driver->get_name().empty());
    for (const auto& value : driver->get_device_state()) {
        CHECK(value.name == "TimeStamp");
    }
    st->silent.store(false);  // replies work again, but no poll has cleared the latch
    const int before_stop = st->command_count('P');
    REQUIRE_NOTHROW(driver->move_axis(1, 0.0));
    CHECK(st->command_count('P') > before_stop);
    const int before_abort = st->command_count('P');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('P') > before_abort);
    CHECK(driver->get_link_fault() == fault);
    poll_status_now_for_testing(*driver);
    CHECK(driver->get_link_fault().empty());
    for (const auto& [name, read] : reads) {
        INFO(name);
        CHECK_NOTHROW(read());
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan position - stale cache faults after repeated failed polls and recovers",
          "[synscan][telescope][link]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    using alpacacore::vendor::synscan::poll_status_now_for_testing;
    const double ra = driver->get_right_ascension();
    st->silent.store(true);

    // One failed tick: the frame ages but is still served; a single lost reply is not a fault.
    poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());

    // The third consecutive failed tick latches the fault: the stale frame is no longer an answer.
    poll_status_now_for_testing(*driver);
    poll_status_now_for_testing(*driver);
    std::string last_error;
    try {
        (void)driver->get_right_ascension();
        FAIL("stale position must not be served after a latched link fault");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        last_error = e.what();
    }
    CHECK(driver->get_connected());
    CHECK(last_error.find("communications compromised") != std::string::npos);
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing

    // The faulted frame is not served for Alt/Az either.
    try {
        (void)driver->get_altitude();
        FAIL("a faulted frame must not be served for Alt/Az");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }

    // Recovery is automatic: the next answered tick clears the latch.
    st->silent.store(false);
    poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_right_ascension() - ra) < 1e-6);
    CHECK(driver->get_link_fault().empty());
    st->silent.store(false);
    driver->set_connected(false);
}

// SynScan "T" modes (protocol v3.3): 1 = Alt/Az, 2 = equatorial in either hemisphere, 3 = PEC. The handset applies
// its own site, so a southern site must still send 2; 3 would start PEC playback.
TEST_CASE("SynScan tracking - mode follows mount geometry, the same in either hemisphere",
          "[synscan][telescope][tracking]") {
    using alpacacore::vendor::synscan::SynScanAlignmentSetting;
    struct Expected {
        unsigned char model_id;
        SynScanAlignmentSetting setting;
        alpacacore::AlignmentMode alignment;
        bool south;
        unsigned char tracking_mode;
    };
    for (const auto& expected :
         {Expected{50, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::GermanPolar, false, 2},
          Expected{50, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::GermanPolar, true, 2},
          Expected{128, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::AltAz, false, 1},
          Expected{128, SynScanAlignmentSetting::Auto, alpacacore::AlignmentMode::AltAz, true, 1},
          // #860: the configured geometry decides for an AZ-EQ mount (5 = AZ-EQ6, 6 = AZ-EQ5) ...
          Expected{5, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::GermanPolar, false, 2},
          Expected{6, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::GermanPolar, true, 2},
          Expected{5, SynScanAlignmentSetting::AltAz, alpacacore::AlignmentMode::AltAz, false, 1},
          // ... and is ignored for a mount whose geometry the model ID fixes.
          Expected{50, SynScanAlignmentSetting::AltAz, alpacacore::AlignmentMode::GermanPolar, false, 2},
          Expected{128, SynScanAlignmentSetting::Equatorial, alpacacore::AlignmentMode::AltAz, false, 1}}) {
        auto st = make_handset();
        st->model_id.store(expected.model_id);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, expected.setting);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_site_latitude(expected.south ? -41.3 : 41.3);
        driver->set_site_longitude(174.8);
        CHECK(driver->get_alignment_mode() == expected.alignment);
        driver->set_tracking(true);

        const auto commands = st->command_snapshot();
        const auto tracking = std::find_if(commands.rbegin(), commands.rend(), [](const std::string& command) {
            return command.size() >= 2 && command[0] == 'T';
        });
        REQUIRE(tracking != commands.rend());
        CHECK(static_cast<unsigned char>((*tracking)[1]) == expected.tracking_mode);
        driver->set_connected(false);
    }

    auto st = make_handset();
    st->model_id.store(5);  // AZ-EQ can be used in two alignment configurations; the HC does not report which.
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    // alignmentMode "auto" (or absent) keeps the refusal to guess (#857).
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    require_alpaca_error([&] { (void)driver->get_alignment_mode(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::DriverException);
    CHECK(st->command_count('T') == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - a same-axis replacement runs for its requested duration",
          "[synscan][telescope][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->pulse_guide(0, 2000);
    driver->pulse_guide(0, 2000);
    // Virtual time for the replaced pulse task to wind down; the 2 s pulse is still running.
    clock.advance(std::chrono::milliseconds(300));
    clock.wait_for_woken_settled(std::chrono::seconds(2));

    const auto commands = st->command_snapshot();
    int last_dec = -1;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (is_variable_rate_command(commands[i], 17)) {
            last_dec = static_cast<int>(i);
        }
    }
    REQUIRE(last_dec >= 0);
    CHECK_FALSE(is_zero_rate_command(commands[static_cast<std::size_t>(last_dec)]));
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - stops active pulse tasks and restores requested tracking",
          "[synscan][telescope][pulseguiding][abort]") {
    for (const int direction : {0, 2}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset();
        // The log size below spans a status-poll tick; the poll's own reads (e, t) are not what the case pins.
        st->log_status_reads.store(false);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
        const int tracking_before = st->command_count('T');
        driver->pulse_guide(direction, 1500);
        REQUIRE(driver->get_is_pulse_guiding());
        REQUIRE_NOTHROW(driver->abort_slew());
        const auto at_abort = st->command_snapshot();
        const int at_abort_size = static_cast<int>(at_abort.size());
        CHECK_FALSE(driver->get_is_pulse_guiding());
        if (direction == 2) {
            CHECK(st->command_count('T') == tracking_before + 1);
            CHECK(driver->get_tracking());
        }
        clock.advance(std::chrono::milliseconds(1700));
        CHECK(clock.wait_for_woken_settled(std::chrono::seconds(2)));
        CHECK(static_cast<int>(st->command_snapshot().size()) == at_abort_size);
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan AbortSlew - does not restore tracking disabled during an RA pulse",
          "[synscan][telescope][pulseguiding][abort]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    driver->pulse_guide(2, 1500);
    driver->set_tracking(false);
    const int tracking_writes_before_abort = st->command_count('T');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_FALSE(driver->get_tracking());
    CHECK(st->command_count('T') == tracking_writes_before_abort);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - SlewToCoordinates reaps the pulse and clears IsPulseGuiding (#831)",
          "[synscan][telescope][pulseguiding][reap]") {
    enum class Op { SlewToCoordinates, SlewToCoordinatesAsync, Park };
    for (const Op op : {Op::SlewToCoordinates, Op::SlewToCoordinatesAsync, Op::Park}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        driver->pulse_guide(0, 60000);  // outlasts the sync slew, so only the reap can clear it
        REQUIRE(driver->get_is_pulse_guiding());
        switch (op) {
            case Op::SlewToCoordinates:
                run_virtual(
                    clock, [&] { driver->slew_to_coordinates(5.5, 20.0); },
                    [&] { return st->goto_count.load() >= 1; });  // the pulse is reaped, the GOTO is out
                break;
            case Op::SlewToCoordinatesAsync:
                driver->slew_to_coordinates_async(5.5, 20.0);
                break;
            case Op::Park:
                driver->park();
                break;
        }
        INFO("op " << static_cast<int>(op));
        CHECK_FALSE(driver->get_is_pulse_guiding());
        // The async slew and the park run on the task clock: step them out before the disconnect joins them.
        REQUIRE(advance_until(clock, [&] { return !driver->get_slewing(); }));
        driver->set_connected(false);
    }
}

// A reaper stores a cancel flag and then wakes the parked task. If the notify
// does not pass through task_mutex_, a task that has read its flag as false
// but not yet blocked misses it and sleeps until the next advance(), holding
// up the reaper's join. FakeTaskClock's before_block hook holds the first
// parked task in that window until its predicate sees the reaper's store;
// with notify_task_waiters() the reaper is then blocked on task_mutex_ until
// the task blocks, without it the notify goes by while the task is outside
// its wait and the join hangs (same shape as the Sky-Watcher case, #743).
TEST_CASE("SynScan async - a reaper's cancel is not lost between a parked task's check and its block",
          "[synscan][telescope][async][pulseguiding]") {
    std::atomic<bool> at_window{false};
    std::atomic<int> fired{0};
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // Only the status poll is parked on the clock: it registered before the hook is set and its own generation
    // never moves during the case, so it stays blocked past the hook and the pulse task below is the first thread
    // at the hook.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::milliseconds(5000)));
    REQUIRE(clock.waiter_count() == 1);
    clock.set_before_block([&](const std::function<bool()>& pred) {
        if (fired.fetch_add(1) == 0) {
            at_window.store(true);
            // The slot runs the predicate and the block under its one mutex,
            // so the reaper's cancel cannot reach the body until it blocks:
            // hold the task in the window for a moment of real time.
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (!pred() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::yield();
            }
        }
    });
    driver->pulse_guide(0, 2000);  // North: its timer parks on the clock
    REQUIRE(wait_until([&] { return at_window.load(); }, 3000));

    // The superseding pulse reaps the parked one: cancel, notify, join.
    std::atomic<bool> reaped{false};
    std::thread reaper([&] {
        driver->pulse_guide(0, 300);
        reaped.store(true);
    });
    CHECK(wait_until([&] { return reaped.load(); }, 2000));

    // Whatever happened, reaching the timer's deadline wakes it, so the case ends instead of hanging in a join.
    clock.advance(std::chrono::seconds(5));
    reaper.join();
    driver->set_connected(false);
    driver.reset();
    clock.set_before_block(nullptr);
}

// Decision 0022: the 8 s Slewing hold is a util::HoldWindow, armed when a GOTO starts. A PIN of the type port
// (#1836): the slew body's landing check still reads the window (rule 3 is open, the 0022 row is partial), so a GOTO
// the handset reports done after 1.5 s keeps Slewing true until the hold ends. A change that lands on the handset's
// report updates this case.
TEST_CASE("SynScan hold window - a GOTO the handset reports done keeps Slewing until the 8 s hold ends (#1836 PIN)",
          "[synscan][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    const auto start = clock.now();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(advance_until(clock, [&] { return clock.now() - start >= std::chrono::milliseconds(7750); }));
    CHECK_FALSE(st->goto_in_progress());
    CHECK(driver->get_slewing());
    REQUIRE(advance_until(clock, [&] { return !driver->get_slewing(); }));
    CHECK(clock.now() - start >= std::chrono::seconds(8));
    driver->set_connected(false);
}

// Rule 2: AbortSlew clears the hold, so Slewing reads false as soon as it returns.
TEST_CASE("SynScan hold window - AbortSlew inside the hold clears Slewing at once",
          "[synscan][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    const auto start = clock.now();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
    REQUIRE(advance_until(clock, [&] { return clock.now() - start >= std::chrono::seconds(2); }));
    REQUIRE(driver->get_slewing());
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Rule 4: IsPulseGuiding is a per-axis util::HoldWindow on the task clock, the pulse plus the 1 s completion delay.
TEST_CASE("SynScan hold window - a pulse keeps its axis while virtual time has not reached its end",
          "[synscan][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    driver->pulse_guide(0, 500);  // North: Dec, 0.5 s, held until 1.5 s
    REQUIRE(driver->get_is_pulse_guiding());
    clock.advance(std::chrono::milliseconds(500));  // the pulse body sends its stop
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    clock.advance(std::chrono::milliseconds(999));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(2)));
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(std::chrono::milliseconds(1));  // exact to the tick
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// Rule 2 (#1836): the pulse window is armed before the rate command goes out, so a client polling IsPulseGuiding while
// the command is on the wire already reads true. The fake reads the getter when the Dec rate command reaches it.
TEST_CASE("SynScan hold window - the pulse window is armed before the guide rate command is written",
          "[synscan][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<alpacacore::TelescopeDriver*> watched{nullptr};
    std::atomic<int> guiding_at_command{-1};  // -1: no Dec rate command seen yet
    alpacacore::test::FakeMountServer server(
        [&watched, &guiding_at_command, inner = st->responder()](const std::string& chunk) {
            alpacacore::TelescopeDriver* const driver = watched.load();
            if (driver != nullptr && is_variable_rate_command(chunk, 17) && !is_zero_rate_command(chunk) &&
                guiding_at_command.load() < 0) {
                try {
                    guiding_at_command.store(driver->get_is_pulse_guiding() ? 1 : 0);
                } catch (...) {
                    guiding_at_command.store(2);
                }
            }
            return inner(chunk);
        });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    watched.store(driver.get());

    driver->pulse_guide(0, 500);  // North: Dec
    watched.store(nullptr);
    CHECK(guiding_at_command.load() == 1);
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// Rule 2 (#1836): a rate command that fails clears the window it armed, so IsPulseGuiding reads false at once, not for
// the pulse plus the 1 s completion delay. Over TCP a send on a reset connection fails without losing the link, so
// the getter still answers. An earlier RA stop whose tracking restore timed out leaves the cached mode owed to the
// handset, so the pulse reads no `t` and the rate command is its first exchange after the slewing gate's `L`.
TEST_CASE("SynScan hold window - a failed pulse rate command leaves IsPulseGuiding false",
          "[synscan][telescope][hold-window][pulseguiding]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    st->reject_tracking.store(true);
    REQUIRE_THROWS(driver->move_axis(0, 0.0));  // the stop lands, its tracking restore times out
    st->reject_tracking.store(false);
    REQUIRE(server.drop_connections());

    REQUIRE_THROWS(driver->pulse_guide(2, 500));  // East: RA; the rate command's write fails
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// Rule 2 (#1836): a SlewToCoordinates whose GOTO send throws clears the 8 s window it armed, so Slewing reads false as
// soon as the call has failed (it was true for 8 s). The handset never answers the GOTO, so the send times out.
TEST_CASE("SynScan hold window - a SlewToCoordinates whose GOTO send fails leaves Slewing false",
          "[synscan][telescope][hold-window]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    st->reject_goto.store(true);
    REQUIRE_THROWS(driver->slew_to_coordinates(5.5, 20.0));  // no virtual time passes: the window would be open
    REQUIRE(st->goto_held.load());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

#endif  // _WIN32

// The mode a stop restores is the one the handset held when the motion started, not the one cached at connect: a
// keypad change in between must not be undone by AbortSlew, MoveAxis(0) or the end of an RA pulse.
namespace {
std::unique_ptr<alpacacore::TelescopeDriver> connect_with_stale_tracking(const std::shared_ptr<FakeSynScanHandset>& st,
                                                                         alpacacore::test::FakeMountServer& server) {
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());  // fills the driver's cache with the handset's mode 1
    st->tracking_mode = 0;            // the keypad turns tracking off
    return driver;
}
}  // namespace

TEST_CASE("SynScan AbortSlew - does not turn tracking back on after a keypad change",
          "[synscan][telescope][abort][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);
    const int writes_before = st->command_count('T');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - stopping RA does not turn tracking back on after a keypad change",
          "[synscan][telescope][moveaxis][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);
    const int writes_before = st->command_count('T');
    driver->move_axis(0, 0.5);
    driver->move_axis(0, 0.0);
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - the end of an RA pulse does not turn tracking back on after a keypad change",
          "[synscan][telescope][pulseguiding][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);
    const int writes_before = st->command_count('T');
    driver->pulse_guide(2, 200);
    REQUIRE(wait_until([&]() { return !driver->get_is_pulse_guiding(); }, 3000));
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan SlewToCoordinates - refuses after the keypad turned tracking off",
          "[synscan][telescope][slew][tracking]") {
    for (const bool async : {false, true}) {
        auto st = make_handset();
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = connect_with_stale_tracking(st, server);
        INFO("async " << async);
        require_alpaca_error(
            [&]() {
                if (async) {
                    driver->slew_to_coordinates_async(5.5, 20.0);
                } else {
                    driver->slew_to_coordinates(5.5, 20.0);
                }
            },
            alpacacore::AlpacaError::InvalidOperation);
        CHECK(st->goto_count.load() == 0);
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan idle stops - still sent when the tracking read times out", "[synscan][telescope][tracking]") {
    for (const bool abort : {true, false}) {
        auto st = make_handset();
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port(), 300), alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_tracking());  // cache: mode 1
        st->reject_tracking_read = true;
        const int stops_before = st->command_count('P');
        const int writes_before = st->command_count('T');
        INFO("abort " << abort);
        if (abort) {
            REQUIRE_NOTHROW(driver->abort_slew());
        } else {
            REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
        }
        CHECK(st->command_count('P') > stops_before);
        CHECK(st->command_count('T') == writes_before + 1);  // the cached mode, since the read failed
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan stops - restore the mode the handset held when the motion started",
          "[synscan][telescope][tracking]") {
    // The keypad moved tracking from 1 to 2 before the motion; the stop restores 2, not the cached 1.
    for (const int site : {0, 1, 2}) {
        auto st = make_handset();
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = connect_with_stale_tracking(st, server);
        st->tracking_mode = 2;
        const int writes_before = st->command_count('T');
        INFO("stop site " << site);
        if (site == 0) {
            driver->abort_slew();
        } else if (site == 1) {
            driver->move_axis(0, 0.5);
            driver->move_axis(0, 0.0);
        } else {
            driver->pulse_guide(2, 200);
            REQUIRE(wait_until([&]() { return !driver->get_is_pulse_guiding(); }, 3000));
        }
        const auto commands = st->command_snapshot();
        REQUIRE(st->command_count('T') == writes_before + 1);
        const auto last_t = std::find_if(commands.rbegin(), commands.rend(),
                                         [](const std::string& c) { return !c.empty() && c[0] == 'T'; });
        REQUIRE(last_t != commands.rend());
        CHECK(static_cast<int>(static_cast<unsigned char>((*last_t)[1])) == 2);
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan AbortSlew - a finished RA pulse no longer counts as running when the keypad changes tracking",
          "[synscan][telescope][pulseguiding][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());
    driver->pulse_guide(2, 100);
    // No getter is read while waiting: only IsPulseGuiding clears the driver's pulse flag, and the pulse's end
    // time (duration plus completion delay) has passed by now.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    st->tracking_mode = 0;
    const int writes_before = st->command_count('T');
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(st->command_count('T') == writes_before);
    CHECK(st->tracking_mode.load() == 0);
    driver->set_connected(false);
}

// The handset turns tracking off when an RA move starts, so a stop that read `t` while the motion ran would see 0 and
// leave the mount not tracking. Each case starts at mode 2 and the stop must write it back.
namespace {
int last_tracking_write(FakeSynScanHandset& st) {
    const auto commands = st.command_snapshot();
    const auto last_t = std::find_if(commands.rbegin(), commands.rend(),
                                     [](const std::string& c) { return !c.empty() && c[0] == 'T'; });
    return last_t == commands.rend() ? -1 : static_cast<int>(static_cast<unsigned char>((*last_t)[1]));
}

std::unique_ptr<alpacacore::TelescopeDriver> connect_tracking_mode_2(const std::shared_ptr<FakeSynScanHandset>& st,
                                                                     alpacacore::test::FakeMountServer& server) {
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    st->tracking_mode = 2;
    REQUIRE(driver->get_tracking());
    return driver;
}

// How a case leaves a tracking restore that did not land: the RA rate command and its stop replaced tracking on the
// handset (mode 0), the `T` that should have resumed it timed out, and Tracking reads false.
enum class FailedRestore : std::uint8_t { RefusedPulseTimer, MoveAxisStop };

void fail_tracking_restore(alpacacore::TelescopeDriver& driver, FakeSynScanHandset& st, FailedRestore how) {
    st.reject_tracking = true;
    if (how == FailedRestore::RefusedPulseTimer) {
        alpacacore::vendor::synscan::set_pulse_spawn_for_testing(driver, [](std::function<void()>) -> std::thread {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
        CHECK_THROWS_AS(driver.pulse_guide(2, 200), alpacacore::AlpacaException);
        alpacacore::vendor::synscan::set_pulse_spawn_for_testing(
            driver, [](std::function<void()> body) { return std::thread(std::move(body)); });
    } else {
        driver.move_axis(0, 0.5);
        CHECK_THROWS_AS(driver.move_axis(0, 0.0), alpacacore::AlpacaException);
    }
    st.reject_tracking = false;
    REQUIRE(st.tracking_mode.load() == 0);  // the handset is not tracking
    REQUIRE_FALSE(driver.get_tracking());   // and the driver says so
}
}  // namespace

// The handset's mode 0 after a failed restore is the driver's own unfinished stop, not a keypad change: the next RA
// motion keeps the mode the client asked for and its end restores it. Before the fix the pulse read that 0 as the mode
// to restore, ran the motor at the guide rate alone and ended with a plain stop, so the mount stayed untracked.
TEST_CASE("SynScan pulse guide - the next RA pulse restores tracking after a failed restore",
          "[synscan][telescope][pulseguiding][tracking]") {
    for (const FailedRestore how : {FailedRestore::RefusedPulseTimer, FailedRestore::MoveAxisStop}) {
        INFO("failed restore " << static_cast<int>(how));
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
        REQUIRE(st->tracking_mode.load() == 2);
        fail_tracking_restore(*driver, *st, how);

        const int writes_before = st->command_count('T');
        start_ra_pulse_parked(clock, *driver);
        REQUIRE(advance_until_pulse_ended(clock, *driver, [&] { return st->command_count('T') > writes_before; }));
        CHECK(st->command_count('T') == writes_before + 1);
        CHECK(last_tracking_write(*st) == 2);
        CHECK(st->tracking_mode.load() == 2);
        CHECK(driver->get_tracking());
        driver->set_connected(false);
    }
}

// The same failed restore, ended by the client's own stop instead of a pulse: AbortSlew, a bare MoveAxis(0, 0) and a
// new RA jog's stop each send the requested mode, not the handset's 0.
TEST_CASE("SynScan stops - AbortSlew and MoveAxis restore tracking after a failed restore",
          "[synscan][telescope][tracking][abort][moveaxis]") {
    enum class Stop : std::uint8_t { AbortSlew, MoveAxisStop, RaJog };
    for (const Stop stop : {Stop::AbortSlew, Stop::MoveAxisStop, Stop::RaJog}) {
        INFO("stop " << static_cast<int>(stop));
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
        fail_tracking_restore(*driver, *st, FailedRestore::MoveAxisStop);

        const int writes_before = st->command_count('T');  // the log holds the rejected `T` too
        if (stop == Stop::AbortSlew) {
            REQUIRE_NOTHROW(driver->abort_slew());
        } else if (stop == Stop::MoveAxisStop) {
            REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
        } else {
            driver->move_axis(0, 0.5);
            REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
        }
        CHECK(st->command_count('T') == writes_before + 1);
        CHECK(last_tracking_write(*st) == 2);
        CHECK(st->tracking_mode.load() == 2);
        CHECK(driver->get_tracking());
        driver->set_connected(false);
    }
}

// A Tracking=false write that times out after a failed restore is still the client's latest request: the next RA
// stop must not send the older mode 2 and turn tracking back on. Before the fix the cache kept 2, and AbortSlew,
// MoveAxis(0, 0) or the next RA pulse's end sent `T 2`.
TEST_CASE("SynScan Tracking - a failed Tracking=false after a failed restore keeps tracking off",
          "[synscan][telescope][tracking][abort][moveaxis][pulseguiding]") {
    enum class Stop : std::uint8_t { AbortSlew, MoveAxisStop, RaPulse };
    for (const Stop stop : {Stop::AbortSlew, Stop::MoveAxisStop, Stop::RaPulse}) {
        INFO("stop " << static_cast<int>(stop));
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_tracking(true);
        fail_tracking_restore(*driver, *st, FailedRestore::MoveAxisStop);
        st->reject_tracking = true;
        CHECK_THROWS_AS(driver->set_tracking(false), alpacacore::AlpacaException);
        st->reject_tracking = false;

        const int writes_before = st->command_count('T');  // the log holds the rejected `T 0` too
        if (stop == Stop::AbortSlew) {
            REQUIRE_NOTHROW(driver->abort_slew());
        } else if (stop == Stop::MoveAxisStop) {
            REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
        } else {
            const int stops_before = zero_rate_ra_commands(*st);
            start_ra_pulse_parked(clock, *driver);
            REQUIRE(
                advance_until_pulse_ended(clock, *driver, [&] { return zero_rate_ra_commands(*st) > stops_before; }));
        }
        CHECK(st->command_count('T') == writes_before);
        CHECK(st->tracking_mode.load() == 0);
        CHECK_FALSE(driver->get_tracking());
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan MoveAxis - stopping an RA jog restores the mode the handset held before it",
          "[synscan][telescope][moveaxis][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_tracking_mode_2(st, server);
    driver->move_axis(0, 0.5);
    REQUIRE(st->tracking_mode.load() == 0);  // the jog replaced tracking
    driver->move_axis(0, 0.0);
    CHECK(last_tracking_write(*st) == 2);
    CHECK(st->tracking_mode.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan AbortSlew - during an RA pulse restores the mode the handset held before it",
          "[synscan][telescope][pulseguiding][abort][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_tracking_mode_2(st, server);
    driver->pulse_guide(2, 1500);
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(last_tracking_write(*st) == 2);
    CHECK(st->tracking_mode.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan PulseGuide - a pulse replaced mid-pulse restores the mode held before the first",
          "[synscan][telescope][pulseguiding][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_tracking_mode_2(st, server);
    driver->pulse_guide(2, 1500);
    REQUIRE(driver->get_is_pulse_guiding());
    driver->pulse_guide(2, 200);
    REQUIRE(wait_until([&]() { return !driver->get_is_pulse_guiding(); }, 4000));
    CHECK(last_tracking_write(*st) == 2);
    CHECK(st->tracking_mode.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - a Dec jog does not hide a keypad change from the next RA jog",
          "[synscan][telescope][moveaxis][tracking]") {
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = connect_with_stale_tracking(st, server);  // cache 1, handset 0
    const int writes_before = st->command_count('T');
    driver->move_axis(1, 0.5);
    driver->move_axis(0, 0.5);
    driver->move_axis(0, 0.0);
    CHECK(st->command_count('T') == writes_before);
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

// #2069: a Tracking write that reaches the wire ahead of the end of an RA pulse is the mode that stays. The write is
// parked inside the handset so the pulse end wakes while it is on the wire, with the mode it cached before the write
// landed; the end then sends its stop and restore behind it, in the wire turn, and must not put the old mode back.
TEST_CASE("SynScan PulseGuide - the end of an RA pulse leaves a Tracking write that reached the wire first",
          "[synscan][telescope][pulseguiding][tracking]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 10000);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    driver->pulse_guide(2, 500);

    st->hold_tracking_write = true;
    std::thread client([&] { driver->set_tracking(false); });
    REQUIRE(st->wait_tracking_write_held());

    // The pulse is over; its body wakes and reads the mode while the write is still parked.
    const int stops_before = st->command_count('P');
    // Stepped until the body's timer fires: it may not have parked on the clock yet when the pulse call returned.
    // The write is still parked, so the body can only be woken, not past its turn.
    REQUIRE(wait_until([&]() { return clock.waiter_count() >= 1; }, 4000));
    clock.advance(std::chrono::milliseconds(600));
    clock.wait_for_woken_settled(std::chrono::milliseconds(300));
    st->release_tracking_write();
    client.join();

    // The pulse end's stop is on the wire; its cache write-through ran in the same turn, so Tracking is still off.
    REQUIRE(advance_until(clock, [&]() { return st->command_count('P') > stops_before; }));
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);  // joins the pulse body, which has sent or skipped its stop and restore
    CHECK(st->tracking_mode.load() == 0);
}

// The pulse end now waits in the wire turn, so a disconnect that joins it must still return once the turn comes.
TEST_CASE("SynScan PulseGuide - a disconnect joins a pulse end that waits for its wire turn",
          "[synscan][telescope][pulseguiding][disconnect]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 10000);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    driver->pulse_guide(2, 500);

    st->hold_tracking_write = true;
    // The held write may finish or be ended by the disconnect's interrupt ("the link is closing", NotConnected); an
    // exception escaping this thread would be std::terminate, so it is caught and judged after the join.
    std::optional<int> client_error;
    std::thread client([&] {
        try {
            driver->set_tracking(false);
        } catch (const alpacacore::AlpacaException& ex) {
            client_error = ex.error_code();
        }
    });
    REQUIRE(st->wait_tracking_write_held());
    clock.advance(std::chrono::milliseconds(600));
    clock.wait_for_woken_settled(std::chrono::milliseconds(300));

    auto disconnected = std::async(std::launch::async, [&] { driver->set_connected(false); });
    st->release_tracking_write();
    client.join();
    if (client_error) CHECK(*client_error == alpacacore::AlpacaError::NotConnected);
    REQUIRE(disconnected.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    disconnected.get();
    CHECK_FALSE(driver->get_connected());
}

#ifndef _WIN32

// Decision 0022 rule 2 (#1836), a cancel clears the window: a pulse cut short by MoveAxis(axis, 0), a slew, Park or
// AbortSlew keeps its window only while that canceller runs, as the RA guard against reading the `t` 0 its blind stop
// left. A canceller that fails or is refused clears it on its way out, so IsPulseGuiding does not stay true over a
// stopped axis for the rest of the cut-short pulse. Each first pulse is 3 s on a fake clock that never advances, and
// the getters answer after each failure (over TCP a failed exchange does not lose the link).

// #2263: a replacement no longer cuts the older pulse short; it supersedes it only once its own rate command has
// landed. The link is reset before the replacement, so its rate command fails: the older RA pulse was not
// superseded and gets no stop, its window is put back (IsPulseGuiding true) and its own end still stops the axis.
TEST_CASE("SynScan hold window - a replacement whose rate command fails leaves the older pulse running",
          "[synscan][telescope][hold-window][pulseguiding][motion-owner]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    driver->pulse_guide(3, 3000);  // West: RA
    REQUIRE(driver->get_is_pulse_guiding());
    const int rate_commands = st->command_count('P');
    // The handset hangs up on a status read, so the replacement's rate command is the first send that fails.
    server.hang_up_on("L");
    alpacacore::vendor::synscan::poll_status_now_for_testing(*driver);
    // The rate command's own failure, not a slewing refusal: the older pulse was never stopped, so no failed stop of
    // it marked the axis as moving.
    require_alpaca_error([&] { driver->pulse_guide(2, 500); }, alpacacore::AlpacaError::DriverException);
    CHECK(st->command_count('P') == rate_commands);  // no stop of the older pulse reached the handset
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// #2263: a moving MoveAxis supersedes the pulse on its axis only once its own rate has landed. Its send fails here
// (the link is reset just before it), so the Dec pulse got no stop and still owns its window; the MoveAxis left the
// axis reported as possibly moving, as a failed MoveAxis send always has.
TEST_CASE("SynScan hold window - a MoveAxis whose send fails leaves the pulse on its axis running",
          "[synscan][telescope][hold-window][pulseguiding][moveaxis][motion-owner]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    driver->pulse_guide(0, 3000);  // North: Dec
    REQUIRE(driver->get_is_pulse_guiding());
    const int rate_commands = st->command_count('P');
    // The handset hangs up on a status read, so the MoveAxis's rate command is the first send that fails.
    server.hang_up_on("L");
    alpacacore::vendor::synscan::poll_status_now_for_testing(*driver);
    REQUIRE_THROWS(driver->move_axis(1, 0.5));
    CHECK(st->command_count('P') == rate_commands);  // no stop of the pulse reached the handset
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// The success path of the same canceller: an RA MoveAxis stop that cuts a pulse short owns the axis and its restore,
// so the window it clears leaves no restore owed and Tracking stays true.
TEST_CASE("SynScan hold window - an RA MoveAxis stop that cuts a pulse short keeps Tracking true",
          "[synscan][telescope][hold-window][pulseguiding][moveaxis]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    const int mode = st->tracking_mode.load();
    REQUIRE(mode > 0);

    driver->pulse_guide(3, 3000);  // West: RA
    REQUIRE(driver->get_is_pulse_guiding());
    driver->move_axis(0, 0.0);
    CHECK(st->tracking_mode.load() == mode);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

// Park with no stored park position reads the position after its join; a read the handset never answers refuses it.
TEST_CASE("SynScan hold window - a Park refused after cutting a pulse short leaves IsPulseGuiding false",
          "[synscan][telescope][hold-window][pulseguiding][park]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    std::atomic<bool> silent_position{false};
    alpacacore::test::FakeMountServer server(
        [&silent_position, inner = st->responder()](const std::string& chunk) -> std::string {
            if (silent_position.load() && !chunk.empty() && (chunk[0] == 'e' || chunk[0] == 'E')) {
                return "";  // no reply: the position read times out
            }
            return inner(chunk);
        });
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    driver->pulse_guide(0, 3000);  // North: Dec
    REQUIRE(driver->get_is_pulse_guiding());
    silent_position.store(true);
    REQUIRE_THROWS(driver->park());
    silent_position.store(false);
    CHECK_FALSE(driver->get_at_park());
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// AbortSlew whose stops fail leaves the slew state as it was (the mount may still be moving), but the pulse it cut
// short is over: its window is cleared, and the restore its RA stop owes stays (Tracking false). The handset hangs up
// on the cancel-GOTO, after the cut-short pulse's blind stop has landed, so the axis stops that follow fail.
TEST_CASE("SynScan hold window - an AbortSlew whose stop fails clears the window of a pulse it cut short",
          "[synscan][telescope][hold-window][pulseguiding][abort]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock, 300);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    driver->pulse_guide(3, 3000);  // West: RA
    REQUIRE(driver->get_is_pulse_guiding());
    server.hang_up_on("M");
    require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

// #2263, decision 0006 rule 3: a new motion command supersedes the pulse on its own axis and never joins it. The old
// pulse body then sends that axis nothing (no blind stop, no end-of-pulse stop or restore) and touches no driver
// state; the newer command owns the axis. A pulse on the other axis keeps running. Every first pulse below runs on a
// fake clock that a case steps itself, with the status poll paused so the handset's log holds only the motion.
namespace {

struct ParkedPulse {
    std::thread::id body;
    std::shared_future<void> ended;  // ready once the body has returned
};

// Starts a pulse whose body signals when it returns, and waits until that body is parked on the clock. Later pulses
// on the same driver run through the hook unsignalled.
ParkedPulse start_pulse_parked(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                               int direction, int duration_ms) {
    auto ended = std::make_shared<std::promise<void>>();
    auto started = std::make_shared<std::promise<std::thread::id>>();
    auto first = std::make_shared<std::atomic<bool>>(false);
    alpacacore::vendor::synscan::set_pulse_spawn_for_testing(driver,
                                                             [ended, started, first](std::function<void()> body) {
                                                                 if (first->exchange(true)) {
                                                                     return std::thread(std::move(body));
                                                                 }
                                                                 std::thread thread([body = std::move(body), ended] {
                                                                     body();
                                                                     ended->set_value();
                                                                 });
                                                                 started->set_value(thread.get_id());
                                                                 return thread;
                                                             });
    ParkedPulse pulse;
    pulse.ended = ended->get_future().share();
    driver.pulse_guide(direction, duration_ms);
    auto body = started->get_future();
    REQUIRE(body.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    pulse.body = body.get();
    REQUIRE(clock.wait_for_waiter_thread(pulse.body, std::chrono::milliseconds(5000)));
    return pulse;
}

bool returned(const ParkedPulse& pulse) {
    return pulse.ended.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
}

// Zero-rate variable-rate commands on `device` (16 RA, 17 Dec): two per axis stop.
int zero_rate_commands(FakeSynScanHandset& st, int device) {
    const auto commands = st.command_snapshot();
    return static_cast<int>(std::count_if(commands.begin(), commands.end(), [device](const std::string& command) {
        return is_variable_rate_command(command, device) && is_zero_rate_command(command);
    }));
}

// The last variable-rate command on `device` is a non-zero rate: the axis is still moving.
bool last_rate_moves(FakeSynScanHandset& st, int device) {
    const auto commands = st.command_snapshot();
    for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
        if (is_variable_rate_command(*it, device)) {
            return !is_zero_rate_command(*it);
        }
    }
    return false;
}

struct PulseAxis {
    const char* name;
    int direction;  // the PulseGuide direction: 0 North (Dec), 3 West (RA)
    int axis;       // the MoveAxis axis
    int device;     // the motor: 16 RA, 17 Dec
};
constexpr PulseAxis kPulseAxes[] = {{"Dec", 0, 1, 17}, {"RA", 3, 0, 16}};

std::unique_ptr<alpacacore::TelescopeDriver> make_tracking_driver(alpacacore::test::FakeTaskClock& clock, int port,
                                                                  int timeout_ms = 1000) {
    auto driver = make_driver(port, clock, timeout_ms);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    return driver;
}

}  // namespace

TEST_CASE("SynScan PulseGuide - a second pulse on an axis supersedes the first without a join or a stop (#2263)",
          "[synscan][telescope][pulseguiding][motion-owner]") {
    for (const auto& a : kPulseAxes) {
        INFO(a.name);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_tracking_driver(clock, server.port());

        const ParkedPulse first = start_pulse_parked(clock, *driver, a.direction, 3000);
        const int stops = zero_rate_commands(*st, a.device);
        const int tracking_writes = st->command_count('T');
        driver->pulse_guide(a.direction, 3000);
        REQUIRE(returned(first));                           // woken by the supersede; a join would also have ended it
        CHECK(zero_rate_commands(*st, a.device) == stops);  // the old body sent no stop: the replacement owns the axis
        CHECK(st->command_count('T') == tracking_writes);
        CHECK(last_rate_moves(*st, a.device));
        CHECK(driver->get_is_pulse_guiding());

        // The replacement's own end still stops the axis (Dec) or hands it back to tracking (RA).
        REQUIRE(advance_until(clock, [&] { return !driver->get_is_pulse_guiding(); }));
        if (a.device == 17) {
            CHECK(zero_rate_commands(*st, a.device) > stops);
        } else {
            CHECK(st->command_count('T') > tracking_writes);
        }
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan MoveAxis - a MoveAxis on the axis of a running pulse supersedes it without a join or a stop (#2263)",
          "[synscan][telescope][pulseguiding][moveaxis][motion-owner]") {
    for (const auto& a : kPulseAxes) {
        INFO(a.name);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_tracking_driver(clock, server.port());

        const ParkedPulse pulse = start_pulse_parked(clock, *driver, a.direction, 3000);
        const int stops = zero_rate_commands(*st, a.device);
        const int tracking_writes = st->command_count('T');
        driver->move_axis(a.axis, 0.5);
        REQUIRE(returned(pulse));
        CHECK(zero_rate_commands(*st, a.device) == stops);
        CHECK_FALSE(driver->get_is_pulse_guiding());
        CHECK(driver->get_slewing());

        // Past the end the pulse would have had: its body is gone, so nothing stops the jog or restores tracking.
        clock.advance(std::chrono::seconds(5));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
        CHECK(zero_rate_commands(*st, a.device) == stops);
        CHECK(st->command_count('T') == tracking_writes);
        CHECK(last_rate_moves(*st, a.device));

        driver->move_axis(a.axis, 0.0);
        CHECK(zero_rate_commands(*st, a.device) > stops);
        CHECK_FALSE(driver->get_slewing());
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan MoveAxis - a MoveAxis on one axis leaves the pulse on the other axis running (#2263)",
          "[synscan][telescope][pulseguiding][moveaxis][motion-owner]") {
    for (const auto& a : kPulseAxes) {
        INFO(a.name);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_tracking_driver(clock, server.port());

        const ParkedPulse pulse = start_pulse_parked(clock, *driver, a.direction, 2000);
        const int stops = zero_rate_commands(*st, a.device);
        const int tracking_writes = st->command_count('T');
        const int other_axis = 1 - a.axis;
        driver->move_axis(other_axis, 0.5);
        CHECK(driver->get_is_pulse_guiding());
        CHECK(zero_rate_commands(*st, a.device) == stops);
        CHECK(last_rate_moves(*st, a.device));
        CHECK(pulse.ended.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        // The pulse runs to its own end and stops its axis (Dec) or hands it back to tracking (RA).
        REQUIRE(advance_until(clock, [&] { return !driver->get_is_pulse_guiding(); }));
        REQUIRE(returned(pulse));
        if (a.device == 17) {
            CHECK(zero_rate_commands(*st, a.device) > stops);
        } else {
            CHECK(st->command_count('T') > tracking_writes);
        }
        driver->move_axis(other_axis, 0.0);
        driver->set_connected(false);
    }
}

// SynScan already refused a pulse during a slew or a park (open-astro#775); pinned here for the policy table: the
// refusal claims nothing, so the slew or park keeps running and the axes get no command. A Park in flight keeps its
// earlier refusal, InvalidWhileParked from the command gate. FindHome is not supported.
TEST_CASE("SynScan PulseGuide - refused while a slew or a park runs, which keeps running (#2263)",
          "[synscan][telescope][pulseguiding][motion-owner]") {
    struct Case {
        const char* name;
        std::function<void(alpacacore::TelescopeDriver&)> start;
        int refusal;
    };
    const Case cases[] = {
        {"SlewToCoordinatesAsync", [](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(5.0, 20.0); },
         alpacacore::AlpacaError::InvalidOperation},
        {"Park", [](alpacacore::TelescopeDriver& d) { d.park(); }, alpacacore::AlpacaError::InvalidWhileParked},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        st->goto_never_ends.store(true);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = make_tracking_driver(clock, server.port());

        c.start(*driver);
        REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
        const int rate_commands = st->command_count('P');
        const int cancels = st->command_count('M');
        for (const int direction : {0, 3}) {
            require_alpaca_error([&] { driver->pulse_guide(direction, 500); }, c.refusal);
        }
        CHECK(st->command_count('P') == rate_commands);
        CHECK(st->command_count('M') == cancels);
        CHECK_FALSE(driver->get_is_pulse_guiding());
        CHECK(driver->get_slewing());
        CHECK(st->goto_seen.load());
        driver->set_connected(false);
    }
}

// The older pulse's own end can queue behind the replacement's turn: the replacement holds its turn across its Dec
// pointing-state read (held in the handset here) while the first pulse's wait runs out. At its turn the old body
// finds it was superseded and sends nothing, so its stop never lands after the replacement's rate.
TEST_CASE("SynScan PulseGuide - an older pulse whose end waits behind the replacement's turn sends nothing (#2263)",
          "[synscan][telescope][pulseguiding][motion-owner]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    auto answer = st->responder();
    auto hold_pointing = std::make_shared<std::atomic<bool>>(false);
    auto pointing = std::make_shared<alpacacore::test::Latch>();
    alpacacore::test::FakeMountServer server([answer, hold_pointing, pointing](const std::string& chunk) {
        if (!chunk.empty() && chunk[0] == 'p' && hold_pointing->exchange(false)) {
            pointing->block();
        }
        return answer(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_tracking_driver(clock, server.port(), 5000);

    const ParkedPulse first = start_pulse_parked(clock, *driver, 0, 200);  // North: Dec
    const int stops = zero_rate_commands(*st, 17);
    auto turn_waits = std::make_shared<std::atomic<int>>(0);
    alpacacore::vendor::synscan::set_wire_turn_wait_hook_for_testing(*driver, [turn_waits] { ++*turn_waits; });
    hold_pointing->store(true);
    auto second = std::async(std::launch::async, [&] { driver->pulse_guide(0, 3000); });
    REQUIRE(pointing->wait_entered());              // the replacement holds its turn inside its `p` read
    clock.advance(std::chrono::milliseconds(250));  // the first pulse's wait ends; its stop queues for the turn
    REQUIRE(wait_until(
        [&] {
            return turn_waits->load() >= 2 ||
                   first.ended.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        },
        5000));
    pointing->release();
    second.get();
    REQUIRE(returned(first));
    alpacacore::vendor::synscan::set_wire_turn_wait_hook_for_testing(*driver, {});
    CHECK(zero_rate_commands(*st, 17) == stops);
    CHECK(last_rate_moves(*st, 17));
    CHECK(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// An older RA pulse whose end-of-pulse tracking restore failed waits on the clock before it retries. A replacement
// in that wait takes the axis over (its rate replaces the older one, and its own end restores tracking); the older
// body then gives up without touching the replacement's window or marking the axis as possibly moving.
TEST_CASE("SynScan PulseGuide - a replacement takes over from an older pulse whose stop is retrying (#2263)",
          "[synscan][telescope][pulseguiding][motion-owner]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = make_tracking_driver(clock, server.port(), 300);

    const ParkedPulse first = start_pulse_parked(clock, *driver, 3, 200);  // West: RA
    const int tracking_writes = st->command_count('T');
    st->reject_tracking.store(true);  // the pulse end's `T` times out
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(wait_until([&] { return st->command_count('T') > tracking_writes; }, 5000));
    REQUIRE(clock.wait_for_waiter_thread(first.body, std::chrono::milliseconds(5000)));  // parked before its retry
    st->reject_tracking.store(false);

    driver->pulse_guide(3, 3000);
    REQUIRE(returned(first));
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->command_count('T') == tracking_writes + 1);  // the older body sent no retry
    REQUIRE(advance_until(clock, [&] { return !driver->get_is_pulse_guiding(); }));
    CHECK(st->command_count('T') > tracking_writes + 1);  // the replacement's end restores tracking
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

// The older pulse fails all its stop attempts and sits in the wait after the last one. A replacement arms its window
// and releases mutex_ for its rate command; the old wait runs out right then (the handset steps the clock when the
// rate arrives). The old body's failure tail must wait for the turn and find itself superseded: run under mutex_
// alone it cleared the replacement's window (IsPulseGuiding false while the axis moves, decision 0022 rule 2) and
// marked the axis as a runaway (Slewing true after the replacement's clean end). RA, because only its stop (the
// tracking `T`) can fail on the fake: a Dec stop is blind.
TEST_CASE("SynScan PulseGuide - an older pulse whose stop failed leaves the replacement's window alone (#2263)",
          "[synscan][telescope][pulseguiding][motion-owner]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    auto answer = st->responder();
    auto wake_on_rate = std::make_shared<std::atomic<bool>>(false);
    auto rate = std::make_shared<alpacacore::test::Latch>();
    alpacacore::test::FakeTaskClock* const virtual_clock = &clock;
    alpacacore::test::FakeMountServer server([answer, wake_on_rate, rate, virtual_clock](const std::string& chunk) {
        if (is_variable_rate_command(chunk, 16) && !is_zero_rate_command(chunk) && wake_on_rate->exchange(false)) {
            virtual_clock->advance(std::chrono::milliseconds(100));  // the old body's last retry wait runs out
            rate->block();
        }
        return answer(chunk);
    });
    REQUIRE(server.ok());
    auto driver = make_tracking_driver(clock, server.port(), 300);

    const ParkedPulse first = start_pulse_parked(clock, *driver, 3, 200);  // West: RA
    st->reject_tracking.store(true);                                       // every stop attempt's `T` times out
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int before = st->command_count('T');
        clock.advance(std::chrono::milliseconds(attempt == 0 ? 250 : 100));
        REQUIRE(wait_until([&] { return st->command_count('T') > before; }, 5000));
        REQUIRE(clock.wait_for_waiter_thread(first.body, std::chrono::milliseconds(5000)));
    }
    st->reject_tracking.store(false);

    wake_on_rate->store(true);
    auto second = std::async(std::launch::async, [&] { driver->pulse_guide(3, 3000); });
    REQUIRE(rate->wait_entered());
    REQUIRE(returned(first));
    rate->release();
    second.get();
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());

    const int tracking_writes = st->command_count('T');
    // The replacement's end restores tracking (its `T`), whatever the old body did to the window.
    REQUIRE(advance_until_pulse_ended(clock, *driver, [&] { return st->command_count('T') > tracking_writes; }));
    CHECK_FALSE(driver->get_slewing());
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

#endif  // _WIN32

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
//
// Motion-owner follow-ups (issue #2467): tests that pin the #2263 motion-claim table for this driver.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_celestron_handset.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace cel = alpacacore::vendor::celestron;
using alpacacore::test::CelestronTracking;
using alpacacore::test::FakeCelestronHandset;
using alpacacore::test::require_alpaca_error;
using alpacacore::test::wait_until;

// The driver on a caller-owned task clock over a fake handset: tracking on, the mount at 12AB0500,20000500, and the
// model and pier bytes of an Alt-Az HC (a GEM's 'W' would send a slew through the meridian-flip path half the day).
struct NexstarRig {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<FakeCelestronHandset> st;
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit NexstarRig(int response_timeout_ms = 6000, char model = '0') {
        st = std::make_shared<FakeCelestronHandset>(clock, CelestronTracking::EqNorth, "12AB0500,20000500#");
        st->model_id.store(model);
        st->pier_side.store('0');
        server = std::make_unique<alpacacore::test::FakeMountServer>(st->responder());
        REQUIRE(server->ok());
        cel::ConnectionInfo info;
        info.type = cel::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server->port();
        info.response_timeout_ms = response_timeout_ms;
        driver = cel::create_celestron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt,
                                                           std::nullopt, cel::CelestronAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        cel::set_status_poll_paused_for_testing(*driver, true);  // the poll sends nothing of its own
    }
    ~NexstarRig() { driver->set_connected(false); }
};

// Steps the fake clock `span` in 250 ms steps, each settled.
void step_virtual(alpacacore::test::FakeTaskClock& clock, std::chrono::milliseconds span) {
    for (auto done = std::chrono::milliseconds(0); done < span; done += std::chrono::milliseconds(250)) {
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
}

bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred, std::size_t waiters) {
    for (int i = 0; i < 200; ++i) {
        if (pred()) return true;
        if (!clock.wait_for_waiters(waiters, std::chrono::seconds(5))) break;
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return pred();
}

// Rate-0 frames sent to the RA (axis 0) or DEC (axis 1) motor.
int zero_rate_stops(FakeCelestronHandset& st, int axis) {
    const auto device = static_cast<unsigned char>(axis == 0 ? 0x10 : 0x11);
    std::lock_guard<std::mutex> lock(st.command_mutex);
    int n = 0;
    for (const auto& c : st.commands) {
        if (c.size() < 6 || c[0] != 'P' || static_cast<unsigned char>(c[2]) != device) continue;
        const auto op = static_cast<unsigned char>(c[3]);
        if ((op == 6 || op == 7 || op == 36 || op == 37) && c[4] == '\0' && c[5] == '\0') ++n;
    }
    return n;
}

}  // namespace

// PIN: ends the overlap refusal for a completed Park. Mutation that breaks it: drop the motion_owner_.finished() call
// in set_slewing_flag_locked() (celestron_telescope_driver.cpp, the Slewing-false branch), so the refusal never ends.
// A Park that has completed ends the overlap refusal: the PulseGuide is then refused only for the parked mount
// (InvalidWhileParked, no longer the slew text), and accepted again after Unpark.
TEST_CASE("Celestron motion owner - PulseGuide is accepted again after a Park completes and Unpark runs (#2467)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    NexstarRig rig(6000, static_cast<char>(7));  // an SLT: Unpark's tracking restore needs a known mount model
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    rig.driver->park();
    std::string running_text;
    CHECK(alpacacore::test::error_code_of([&] { rig.driver->pulse_guide(2, 100); }, &running_text) ==
          alpacacore::AlpacaError::InvalidWhileParked);  // a Park keeps its older refusal
    REQUIRE(advance_until(rig.clock, [&] { return alpacacore::test::park_done(*rig.driver); }, 2));
    REQUIRE(rig.driver->get_at_park());
    std::string parked_text;
    CHECK(alpacacore::test::error_code_of([&] { rig.driver->pulse_guide(2, 100); }, &parked_text) ==
          alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(parked_text.find("slew, park or home is running") == std::string::npos);
    CHECK(rig.st->guide_command_count(0) == 0);

    rig.driver->unpark();
    REQUIRE(advance_until(rig.clock, [&] { return !rig.driver->get_at_park() && !rig.driver->get_slewing(); }, 1));
    rig.driver->set_tracking(true);
    CHECK_NOTHROW(rig.driver->pulse_guide(2, 100));
    CHECK(rig.st->guide_command_count(0) == 1);
    CHECK(rig.driver->get_is_pulse_guiding());
}

// PIN: a refused chain start stops the axis through the send gate: the stop is on the wire, the pulse state is closed,
// and the error reaches the client.
TEST_CASE("Celestron motion owner - a refused pulse chain start stops the axis and closes the pulse (#2467)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    NexstarRig rig;
    REQUIRE(rig.driver->get_can_pulse_guide());
    cel::set_pulse_spawn_for_testing(*rig.driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    const int stops = zero_rate_stops(*rig.st, 0);
    require_alpaca_error([&] { rig.driver->pulse_guide(3, 5000); }, alpacacore::AlpacaError::DriverException);
    CHECK(rig.st->guide_command_count(0) == 1);  // the first chunk went out
    CHECK(zero_rate_stops(*rig.st, 0) > stops);  // and the axis was stopped
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// RED on the unmodified driver: the blocking slew reads the pier (the flip decision) before it claims the mount, so a
// read that fails leaves a running pulse on its axis, as the async slew's order always did.
TEST_CASE("Celestron motion owner - a blocking slew whose pier read fails leaves a running pulse alone (#2467)",
          "[celestron][telescope][pulseguide][motion-owner]") {
    NexstarRig rig(300);
    REQUIRE(rig.driver->get_can_pulse_guide());
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    const std::size_t idle = rig.clock.waiter_count();
    rig.driver->pulse_guide(3, 5000);  // RA: no pier read of its own
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, std::chrono::seconds(5)));
    REQUIRE(rig.st->guide_command_count(0) == 1);
    rig.st->pier_hold_ms.store(1000);  // the flip decision's pier read now outlasts the 300 ms response timeout
    bool threw = false;
    try {
        rig.driver->slew_to_coordinates(5.5, 20.0);
    } catch (const alpacacore::AlpacaException&) {
        threw = true;
    }
    rig.st->pier_hold_ms.store(0);
    CHECK(threw);
    CHECK(rig.st->goto_count.load() == 0);
    CHECK(rig.driver->get_is_pulse_guiding());  // the pulse was not superseded
    step_virtual(rig.clock, std::chrono::milliseconds(3000));
    REQUIRE(wait_until([&] { return rig.st->guide_command_count(0) == 2; }, 5000));  // its next chunk still goes out
}

#endif  // !_WIN32

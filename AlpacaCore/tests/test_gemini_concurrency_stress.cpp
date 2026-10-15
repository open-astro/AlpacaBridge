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

// Connect/disconnect/operate concurrency stress for the three Gemini drivers
// (issue #101).
//
// All three get the full-seam treatment, because pty-backed fakes exist and
// let a connect succeed. The first two are shaped differently, and the storm
// reaches a different surface in each:
//   - PDH Advanced 3 Switch over fake_gemini_pdh.h. This one does own a
//     serial reader thread that a disconnect has to reap, and it writes the
//     status cache while the Alpaca getters read it.
//   - Flat Panel Pro CoverCalibrator over fake_gemini_flatpanel.h. This one
//     is strictly request/response -- no reader thread, and set_connected
//     (false) reaps nothing. The thread this driver owns is
//     calibrator_task_thread_, reaped by the DESTRUCTOR
//     (reap_calibrator_task(true)) rather than by a disconnect.
//
//     Be precise about how often the storm actually reaches it: most
//     submissions take the INLINE fast path instead. start_calibrator_task()
//     claims inline whenever !cover_in_flight_ && calibrator_pending_count_
//     == 0 (gemini_flatpanel_driver.cpp:897). open_cover()/close_cover() set
//     cover_in_flight_, so while a cover move is in flight a calibrator
//     submission takes the BACKGROUND path instead; the cover moves feed it.
//     A second op thread slipping between another thread's gate check and
//     its claim reaches it too. The fake acks a cover command at once, so the
//     moves are short and the background path is hit opportunistically, not
//     on every run. Treat it as sampled, NOT as covered; deterministic
//     reproduction needs the fake to hold a reply (set_reply_delay(">L#",
//     ...)) and is a follow-up.
//
//     The cover task is stormed by open_cover()/close_cover() behind a
//     get_cover_moving() gate, plus halt_cover() (open-astro#1067). The fake
//     acks a cover command at once, so each move is short and the same
//     "rarely connected" caveat below applies: it exercises the
//     start_cover_task() submission and reap paths, not a long mechanical move.
//
// The focuser runs over fake_gemini_focuser.h too (open-astro#1067). It is
// request/response like the flat panel, with no reader thread. Connects now
// succeed, so the storm covers the real connect/disconnect transitions
// (transition_mutex_, the handshake, the fd close). It rarely holds a
// CONNECTED driver while the operate calls run: a connect takes about 300 ms
// and the lifecycle threads disconnect every few hundred microseconds, so in a
// measured run only 2 of 68033 guarded calls got past NotConnected. The PDH
// and flat-panel cases share that harness property. Deepening it is a harness
// change and a follow-up.

#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_gemini_flatpanel.h"
#include "fake_gemini_focuser.h"
#include "fake_gemini_pdh.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeGeminiFlatPanel;
using alpacacore::test::FakeGeminiFocuser;
using alpacacore::test::FakeGeminiPdh;

namespace {

// open-astro#326: the operate callbacks take the storm's StressCallGuard by
// reference instead of the file-scope call() template they used to share. Same
// per-call isolation -- run_lifecycle_stress wraps the WHOLE callback in one
// try/catch, so an unguarded throw skips every call below it for that whole
// iteration -- but the guard COUNTS what it swallows, so an unexpected throw
// fails the case rather than being discarded.
void pdh_switch_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
    guard([&] { static_cast<void>(sw.get_max_switch()); });
    guard([&] { static_cast<void>(sw.get_switch_value(0)); });
    guard([&] { static_cast<void>(sw.get_switch_name(0)); });
    guard([&] { static_cast<void>(sw.get_can_write(1)); });
    // ids 0 and 1 are USB A and USB B, both writable; the always-on
    // pass-through rail that refuses writes is id 6 ("DC1").
    guard([&] { sw.set_switch_value(1, 1.0); });
    guard([&] { static_cast<void>(sw.get_device_state()); });
}

void flatpanel_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& panel = static_cast<alpacacore::CoverCalibratorDriver&>(d);
    guard([&] { static_cast<void>(panel.get_calibrator_state()); });
    guard([&] { static_cast<void>(panel.get_cover_state()); });
    guard([&] { static_cast<void>(panel.get_brightness()); });
    guard([&] { static_cast<void>(panel.get_max_brightness()); });
    guard([&] { static_cast<void>(panel.get_calibrator_changing()); });
    guard([&] { static_cast<void>(panel.get_cover_moving()); });
    // Submit a calibrator change only when one is not already in flight.
    // Unconditional submission is what builds an unbounded thread chain
    // here: on the background path calibrator_on()/calibrator_off() return
    // as soon as they spawn a task thread, each spawned thread joins its
    // predecessor (so they all stay live), and four op threads submit every
    // ~200 us -- far faster than one set_light() (two send_command_locked()
    // round trips) can drain.
    //
    // Deliberately no figure for that drain rate. This TEST_CASE builds the
    // *Pro* model, and send_command_locked() skips its kCommandDelayMs
    // settle entirely when config_.model == FlatPanelModel::Pro
    // (gemini_flatpanel_protocol_wrapper.cpp:853) -- that fixed 50 ms cost
    // is a Lite/Rev2 property, and this file storms neither. On the Pro path
    // what remains is the fake's reply latency plus read_response()'s
    // kReadCharDelayMs (10 ms) poll granularity, which is a property of the
    // fake and the runner rather than of the driver, so quoting a number
    // here would just be a figure nobody re-measures.
    //
    // The gate's conclusion does not depend on that number, and a faster
    // drain only strengthens it: fewer commands are ever in flight at once,
    // so the chain is shorter, not longer. The gate is a TOCTOU check, not
    // serialization -- several op threads can observe "not changing" in the
    // same window and all submit together, so it does not cap the chain at
    // one command at a time, only at op_threads (4) per drain interval
    // instead of unbounded. That is what keeps it survivable on a faster
    // runner (pthread_create failing, or TSan's live-thread ceiling) without
    // losing coverage of the task path.
    guard([&] {
        if (!panel.get_calibrator_changing()) {
            // Alternate on/off per thread so run_calibrator_command(false, 0)
            // and the off-behind-inline-on ordering both get stormed, not
            // just calibrator_on().
            thread_local bool on = true;
            if (on) {
                panel.calibrator_on(10);
            } else {
                panel.calibrator_off();
            }
            on = !on;
        }
    });
    // Cover move: same TOCTOU-gated submission as the calibrator above, so
    // start_cover_task() spawns one task thread per drain at most instead of
    // an unbounded chain. The gate does not serialize: two op threads can
    // both see "not moving" and the loser gets InvalidOperation from
    // start_cover_task() (gemini_flatpanel_driver.cpp:762), which the case
    // lists as expected. halt_cover() takes cover_task_mutex_ against the
    // same transition, so it is stormed alongside the submissions.
    guard([&] {
        if (!panel.get_cover_moving()) {
            thread_local bool open = true;
            if (open) {
                panel.open_cover();
            } else {
                panel.close_cover();
            }
            open = !open;
        }
    });
    guard([&] { panel.halt_cover(); });
    guard([&] { static_cast<void>(panel.get_device_state()); });
}

void focuser_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
    guard([&] { static_cast<void>(focuser.get_is_moving()); });
    guard([&] { static_cast<void>(focuser.get_position()); });
    guard([&] { static_cast<void>(focuser.get_max_step()); });
    guard([&] { static_cast<void>(focuser.get_max_increment()); });
    guard([&] { static_cast<void>(focuser.get_temperature()); });
    guard([&] { focuser.move(1234); });
    guard([&] { focuser.halt(); });
}

}  // namespace

TEST_CASE("Gemini PDH Advanced 3 switch - concurrent connect/disconnect/operate stress", "[gemini][switch][stress]") {
    FakeGeminiPdh hub;
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, hub.slave_path(), 19200);

    // Prove the fake actually connects before storming it -- otherwise this
    // silently degrades into a fail-fast test that never reaches the reader
    // thread (the PR #3 lesson recorded in the SynScan stress file).
    // settle_connected() checks its deadline only BEFORE each
    // set_connected() attempt, not during one, so a single
    // slow-but-successful first attempt is never the failure mode -- the
    // budget only matters if that attempt actually fails and there is no
    // time left to retry. The PDH's handshake retry ladder is
    // 0.1s + 2s + 1s of sleeps plus up to 3 x kRequestTimeoutMs (2.5s) ~=
    // 10.6s worst case, so the budget has to clear that, not merely
    // approach it: at the harness default of 10 s a dropped reply would
    // leave no time for the retry this budget exists to fund. 15 s clears
    // the ladder with margin and costs nothing, because the fake answers on
    // the first attempt and the budget is never consumed.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    // Tears the proving connect back down before the storm starts. This is a
    // real teardown, not a no-op -- settle_connected() above just proved the
    // driver CONNECTED, so this closes the port and reaps what the driver
    // owns. It has no throwing failure path today; CHECK_NOTHROW pins that,
    // so a future regression surfaces as a named failure on this line rather
    // than as an unexpected exception attributed to the whole TEST_CASE.
    CHECK_NOTHROW(driver->set_connected(false));

    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { pdh_switch_operate(guard, d); });

    // Still 10 s here, deliberately: a disconnect closes the port and reaps
    // the reader thread, it never walks the handshake ladder, so the
    // connect side's extra margin buys nothing on this call.
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Gemini PDH Advanced 3 switch - destruction races an in-flight connect", "[gemini][switch][stress]") {
    FakeGeminiPdh hub;
    const std::string port = hub.slave_path();
    // 25 iterations, not the harness default of 100: unlike a fail-fast
    // case, every iteration here reaches a real handshake, whose first
    // attempt alone sleeps 100 ms before its read. 100 iterations would put
    // ~10 s on the clock for this case on its own, against the 30-minute
    // timeout-minutes bound on the sanitizers-tsan job it runs in, which
    // the whole [stress] suite shares. Do not "restore" the default
    // without re-checking both.
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::gemini::create_gemini_pdh_switch(0, port, 19200); }, 25);
}

TEST_CASE("Gemini PDH Advanced 3 switch - racing disconnect is never dropped", "[gemini][switch][stress]") {
    FakeGeminiPdh hub;
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, hub.slave_path(), 19200);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

TEST_CASE("Gemini Flat Panel Pro - concurrent connect/disconnect/operate stress", "[gemini][covercalibrator][stress]") {
    FakeGeminiFlatPanel panel;
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600);

    // 15 s, same reasoning as the PDH case above: the flat panel's handshake
    // retry ladder is the same shape, and the budget is retry headroom, not
    // a bound on a single slow-but-successful attempt.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    // Tears the proving connect back down before the storm starts. This is a
    // real teardown, not a no-op -- settle_connected() above just proved the
    // driver CONNECTED, so this closes the port and reaps what the driver
    // owns. It has no throwing failure path today; CHECK_NOTHROW pins that,
    // so a future regression surfaces as a named failure on this line rather
    // than as an unexpected exception attributed to the whole TEST_CASE.
    CHECK_NOTHROW(driver->set_connected(false));

    // NotConnected (racing disconnect) plus InvalidOperation: two op threads
    // can pass the get_cover_moving() gate together and the second
    // open_cover()/close_cover() is refused while the first is in flight
    // (gemini_flatpanel_driver.cpp:762). Nothing else is expected.
    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidOperation});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { flatpanel_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Gemini Flat Panel Pro - destruction races an in-flight connect", "[gemini][covercalibrator][stress]") {
    FakeGeminiFlatPanel panel;
    const std::string port = panel.slave_path();
    // 25 not 100, same reasoning as the PDH destruction case above.
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, port, 9600); }, 25);
}

TEST_CASE("Gemini Flat Panel Pro - racing disconnect is never dropped", "[gemini][covercalibrator][stress]") {
    FakeGeminiFlatPanel panel;
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

TEST_CASE("Gemini focuser - concurrent connect/disconnect/operate stress", "[gemini][focuser][stress]") {
    FakeGeminiFocuser fake;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    // Prove the fake connects before storming it, so this cannot degrade into
    // a fail-fast case that never reaches the connected paths. 15 s covers the
    // handshake retry ladder (100 ms + 2 s + 1 s of sleeps plus read
    // timeouts); the fake answers the first attempt, so it is never spent.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    CHECK_NOTHROW(driver->set_connected(false));

    // Expected set stated explicitly (AGENTS.md, CI section). A connected
    // focuser's only legitimate throw is NotConnected from a racing disconnect
    // (ensure_connected(), gemini_focuser_driver.cpp:313, and the wrapper's own
    // check under its lock, gemini_protocol_wrapper.cpp:597). move() clamps
    // instead of throwing InvalidValue (gemini_focuser_driver.cpp:302), and the
    // fake answers every query. A DriverException (wrapper read/write failure)
    // is deliberately not expected: the fake never fails a transaction, so one
    // would be a real regression.
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { focuser_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Gemini focuser - destruction races an in-flight connect", "[gemini][focuser][stress]") {
    FakeGeminiFocuser fake;
    const std::string port = fake.slave_path();
    // 25 not 100, same reasoning as the PDH destruction case above: every
    // iteration reaches a real handshake.
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::gemini::create_gemini_focuser(0, port); }, 25);
}

TEST_CASE("Gemini focuser - racing disconnect is never dropped", "[gemini][focuser][stress]") {
    FakeGeminiFocuser fake;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

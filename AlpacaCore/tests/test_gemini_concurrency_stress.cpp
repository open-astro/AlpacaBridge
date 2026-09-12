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
// Two of them get the full-seam treatment, because pty-backed fakes already
// exist and drive them into the CONNECTED state, where the background work
// actually runs -- but the two drivers are shaped differently, and the storm
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
//     == 0 (gemini_flatpanel_driver.cpp:897), the callback issues no cover
//     move so cover_in_flight_ is always false, and the submission gate
//     below only fires when the count reads 0. The background path is
//     therefore reached only when a second op thread slips between another
//     thread's gate check and its claim -- a microsecond window, so it is
//     hit opportunistically, not on every run. Treat the background path as
//     sampled, NOT as covered; deterministic reproduction needs the fake to
//     hold a reply (set_reply_delay(">L#", ...)) and is a follow-up.
//
//     The cover task is not reached at all: the operate callback issues no
//     open/close/halt_cover, so cover_task_thread_ is never constructed and
//     reap_cover_task(true) always takes its !joinable() early exit. Adding
//     a cover call would widen this past a registration PR; also a
//     follow-up.
//
// The focuser stays on the accepted fail-fast bar (the ZWO/iOptron
// precedent): a serial path that cannot exist, so every connect fails fast
// and the storm still covers the AsyncConnectable machinery and the
// connect-failure cleanup. Note that a pty-backed fake DOES now exist for it
// -- AlpacaCore/tests/fake_gemini_focuser.h, added with the transition-mutex
// fix (open-astro#333) -- it is simply not wired into this storm yet. Wire
// that up rather than writing a second seam.

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
#include "fake_gemini_pdh.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeGeminiFlatPanel;
using alpacacore::test::FakeGeminiPdh;

namespace {

// A racing disconnect makes any of these throw NotConnected, and the harness
// only swallows the callback as a whole -- per-call guards keep one throw
// from skipping every call below it for that whole iteration. std::exception
// rather than AlpacaException: anything else escaping the serial teardown
// would otherwise unwind past the remaining calls, which is the exact
// failure mode this helper exists to prevent.
template <typename Fn>
void call(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
    }
}

void pdh_switch_operate(AlpacaDriver& d) {
    auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
    call([&] { static_cast<void>(sw.get_max_switch()); });
    call([&] { static_cast<void>(sw.get_switch_value(0)); });
    call([&] { static_cast<void>(sw.get_switch_name(0)); });
    call([&] { static_cast<void>(sw.get_can_write(1)); });
    // ids 0 and 1 are USB A and USB B, both writable; the always-on
    // pass-through rail that refuses writes is id 6 ("DC1").
    call([&] { sw.set_switch_value(1, 1.0); });
    call([&] { static_cast<void>(sw.get_device_state()); });
}

void flatpanel_operate(AlpacaDriver& d) {
    auto& panel = static_cast<alpacacore::CoverCalibratorDriver&>(d);
    call([&] { static_cast<void>(panel.get_calibrator_state()); });
    call([&] { static_cast<void>(panel.get_cover_state()); });
    call([&] { static_cast<void>(panel.get_brightness()); });
    call([&] { static_cast<void>(panel.get_max_brightness()); });
    call([&] { static_cast<void>(panel.get_calibrator_changing()); });
    call([&] { static_cast<void>(panel.get_cover_moving()); });
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
    call([&] {
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
    call([&] { static_cast<void>(panel.get_device_state()); });
}

void focuser_operate(AlpacaDriver& d) {
    auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
    call([&] { static_cast<void>(focuser.get_is_moving()); });
    call([&] { static_cast<void>(focuser.get_position()); });
    call([&] { static_cast<void>(focuser.get_max_step()); });
    call([&] { static_cast<void>(focuser.get_max_increment()); });
    call([&] { static_cast<void>(focuser.get_temperature()); });
    call([&] { focuser.move(1234); });
    call([&] { focuser.halt(); });
}

// Never a real device path: the storm connects hundreds of times, so a
// plausible port (/dev/ttyUSB0) could open whatever is actually plugged in
// and take the MyFocuserPro2 handshake to it.
constexpr const char* kAbsentSerialPort = "/dev/gemini-alpacabridge-absent";

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

    alpacacore::test::run_lifecycle_stress(*driver, pdh_switch_operate);

    // Still 10 s here, deliberately: a disconnect closes the port and reaps
    // the reader thread, it never walks the handshake ladder, so the
    // connect side's extra margin buys nothing on this call.
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);
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

    alpacacore::test::run_lifecycle_stress(*driver, flatpanel_operate);

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);
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
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, kAbsentSerialPort);

    alpacacore::test::run_lifecycle_stress(*driver, focuser_operate);

    // The port cannot exist, so no connect in the storm can have succeeded.
    CHECK(driver->get_connected() == false);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Gemini focuser - destruction races an in-flight connect", "[gemini][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::gemini::create_gemini_focuser(0, kAbsentSerialPort); });
}

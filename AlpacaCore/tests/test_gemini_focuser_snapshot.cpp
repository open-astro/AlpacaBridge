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

// Gemini focuser getters served from a util::StateSnapshot (open-astro#1229,
// decision 0010). The driver runs on a FakeTaskClock, so its state poll runs
// only when a case advance()s the clock, and a getter's wire traffic is
// counted exactly on the pty fake.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <variant>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_gemini_focuser.h"
#include "fake_task_clock.h"

using alpacacore::test::connect_and_park;
using alpacacore::test::FakeGeminiFocuser;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::PolledRig;
using alpacacore::test::tick;
using namespace std::chrono_literals;

namespace {

// Longer than the poll period (250 ms) so one advance() is one tick.
constexpr auto kTick = 300ms;
constexpr auto kBound = 5000ms;

// The driver on a pty fake and a fake clock, not yet connected.
struct GeminiRig : PolledRig<FakeGeminiFocuser, alpacacore::FocuserDriver> {
    GeminiRig()
        : PolledRig({kTick, kBound}, [](FakeGeminiFocuser& fake, FakeTaskClock& clock) {
              return alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
          }) {}
};

int wire_reads(const FakeGeminiFocuser& fake) { return fake.count(":00#") + fake.count(":01#") + fake.count(":06#"); }

// A blind command is counted when the fake's reader thread has seen it.
bool wire_saw(const FakeGeminiFocuser& fake, const std::string& prefix) {
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (fake.count(prefix) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return fake.count(prefix) == 1;
}

// Waits up to kBound until thread `tid` (once set) is asleep in the kernel (state S in /proc), for example in a
// join; returns whether it got there.
bool wait_asleep(const std::atomic<pid_t>& tid) {
    while (tid.load() == 0) std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream stat("/proc/self/task/" + std::to_string(tid.load()) + "/stat");
        std::string line;
        std::getline(stat, line);
        const auto name_end = line.rfind(')');  // the state follows the thread name, which may hold spaces
        if (name_end != std::string::npos && name_end + 2 < line.size() && line[name_end + 2] == 'S') return true;
        std::this_thread::yield();
    }
    return false;
}

}  // namespace

TEST_CASE("Gemini Focuser Snapshot - getters between poll ticks cause no wire reads",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());

    CHECK(rig.driver().get_position() == 1000);
    CHECK(rig.driver().get_is_moving() == false);
    CHECK(rig.driver().get_temperature() == Catch::Approx(12.5));

    const int reads_before = wire_reads(rig.fake());
    for (int i = 0; i < 50; ++i) {
        (void)rig.driver().get_position();
        (void)rig.driver().get_is_moving();
        (void)rig.driver().get_temperature();
    }
    CHECK(wire_reads(rig.fake()) == reads_before);

    // The next tick measures the device again and the getters follow.
    rig.fake().set_position(2500);
    rig.fake().set_moving(true);
    rig.fake().set_temperature(-3.5);
    CHECK(rig.driver().get_position() == 1000);  // unchanged until the tick
    rig.tick();
    CHECK(rig.driver().get_position() == 2500);
    CHECK(rig.driver().get_is_moving() == true);
    CHECK(rig.driver().get_temperature() == Catch::Approx(-3.5));
    CHECK(wire_reads(rig.fake()) == reads_before + 3);
}

TEST_CASE("Gemini Focuser Snapshot - IsMoving is true right after a successful Move",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());

    rig.driver().move(4000);
    CHECK(wire_saw(rig.fake(), ":054000#"));  // Move still reaches the device
    CHECK(rig.driver().get_is_moving() == true);

    // The poll that follows measures the device, which says it is not moving.
    rig.tick();
    CHECK(rig.driver().get_is_moving() == false);
}

TEST_CASE("Gemini Focuser Snapshot - Halt reaches the device and IsMoving reads false",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    rig.fake().set_moving(true);
    REQUIRE(rig.connect());
    CHECK(rig.driver().get_is_moving() == true);

    rig.driver().halt();
    CHECK(wire_saw(rig.fake(), ":27#"));
    CHECK(rig.driver().get_is_moving() == false);
}

TEST_CASE("Gemini Focuser Snapshot - disconnect stops the poll and getters throw NotConnected",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());

    rig.driver().set_connected(false);
    CHECK(rig.clock().waiter_count() == 0);  // the poll body ended and was joined
    const int reads = wire_reads(rig.fake());
    rig.clock().advance(kTick);
    CHECK(wire_reads(rig.fake()) == reads);

    for (auto probe : {+[](alpacacore::FocuserDriver& d) { (void)d.get_position(); },
                       +[](alpacacore::FocuserDriver& d) { (void)d.get_is_moving(); },
                       +[](alpacacore::FocuserDriver& d) { (void)d.get_temperature(); }}) {
        try {
            probe(rig.driver());
            FAIL("expected NotConnected");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        }
    }
}

TEST_CASE("Gemini Focuser Snapshot - failed polls latch a link fault and a good poll clears it",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    CHECK(rig.driver().get_position() == 1000);

    rig.fake().set_garbage_state_replies(true);
    rig.tick();
    rig.tick();
    CHECK(rig.driver().get_position() == 1000);  // two failures: still below the threshold
    rig.tick();

    // Latched: the getters and Move refuse, Connected stays true.
    CHECK(rig.driver().get_connected());
    const auto expect_compromised = [](const std::function<void()>& call) {
        try {
            call();
            FAIL("expected DriverException");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(e.what()).find("communications compromised") != std::string::npos);
        }
    };
    expect_compromised([&] { (void)rig.driver().get_position(); });
    expect_compromised([&] { (void)rig.driver().get_is_moving(); });
    expect_compromised([&] { (void)rig.driver().get_temperature(); });
    expect_compromised([&] { rig.driver().move(500); });
    CHECK(rig.fake().count(":05500#") == 0);  // nothing commanded while faulted

    // Halt is the safety stop: never refused.
    rig.driver().halt();
    CHECK(wire_saw(rig.fake(), ":27#"));

    // The poll keeps asking while faulted; the first good reply restores service.
    rig.fake().set_garbage_state_replies(false);
    rig.tick();
    CHECK(rig.driver().get_position() == 1000);
    CHECK(rig.driver().get_temperature() == Catch::Approx(12.5));
}

TEST_CASE("Gemini Focuser Snapshot - a temperature probe that fails keeps Position and backs off",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    rig.fake().set_garbage_temperature(true);
    REQUIRE(rig.connect());

    CHECK(rig.driver().get_position() == 1000);
    CHECK(rig.driver().get_is_moving() == false);
    try {
        (void)rig.driver().get_temperature();
        FAIL("expected DriverException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(rig.fake().count(":06#") == 1);  // the connect-time frame asked once

    rig.tick();
    rig.tick();
    CHECK(rig.fake().count(":06#") == 1);  // backed off: Position keeps polling, the probe is not asked

    // After the retry delay the probe is asked again, and a recovered probe is served.
    rig.fake().set_garbage_temperature(false);
    tick(rig.clock(), 31s, kBound);
    CHECK(rig.fake().count(":06#") == 2);
    CHECK(rig.driver().get_temperature() == Catch::Approx(12.5));
}

// Falsified by: gemini_focuser_driver.cpp: probe() start the frame from state_.read() (a failed read after a good one
// serves the old temperature, and every backoff poll copies it forward).
TEST_CASE("Gemini Focuser Snapshot - a temperature probe that fails after a good read is reported",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    CHECK(rig.driver().get_temperature() == Catch::Approx(12.5));

    rig.fake().set_garbage_temperature(true);
    rig.tick();  // the failed read
    rig.tick();  // a backoff poll
    CHECK(rig.fake().count(":06#") == 2);
    try {
        (void)rig.driver().get_temperature();
        FAIL("expected DriverException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(rig.driver().get_position() == 1000);
}

// Falsified by: gemini_focuser_driver.cpp: set_connected() drop the connect's first-frame check (a connect whose first
// state poll was lost succeeds with no frame).
TEST_CASE("Gemini Focuser Driver - a getter after a successful connect never throws DriverException",
          "[gemini][focuser][snapshot][unit]") {
    GeminiRig rig;
    rig.fake().set_garbage_state_replies(true);
    // Both first polls are lost: the connect is refused, so no getter can run on a
    // session without a frame, and the refused connect leaves nothing behind.
    CHECK_THROWS_AS(rig.driver().set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(rig.driver().get_connected());
    CHECK_FALSE(rig.driver().get_device_firmware().has_value());
    rig.fake().set_garbage_state_replies(false);
    // The port was released: a connect with replies back succeeds and serves a frame.
    REQUIRE_NOTHROW(rig.driver().set_connected(true));
    CHECK_NOTHROW(rig.driver().get_position());
    CHECK_NOTHROW(rig.driver().get_is_moving());
}

// Falsified by: gemini_focuser_driver.cpp: link_up() drop the !state_.link_lost() term, so a
// fault standing past util::kLinkStalenessBound left Connected true and the getters on a latched DriverException.
TEST_CASE("Gemini Focuser Snapshot - a fault that stands past the link-loss bound drops Connected",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    REQUIRE(rig.driver().get_position() == 1000);
    CHECK(alpacacore::util::is_serial_port_in_use(rig.fake().slave_path()));

    // Three failed polls latch the fault; Connected is still true (decision 0009).
    rig.fake().set_garbage_state_replies(true);
    for (int i = 0; i < 3; ++i) {
        rig.tick();
    }
    CHECK(rig.driver().get_connected() == true);

    // The fault stands past util::kLinkStalenessBound (30 s): the link is lost.
    rig.clock().advance(31s);
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    CHECK(rig.driver().get_connected() == false);
    using alpacacore::test::require_alpaca_error;
    require_alpaca_error([&] { (void)rig.driver().get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver().get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { rig.driver().move(500); }, alpacacore::AlpacaError::NotConnected);
    CHECK(rig.driver().get_device_state().empty());
    // on_lost() released the transport.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(rig.fake().slave_path()));

    // No autonomous relink; the next Connect re-runs the probe and serves a fresh frame.
    rig.fake().set_garbage_state_replies(false);
    rig.fake().set_position(1700);
    rig.driver().set_connected(true);
    CHECK(rig.driver().get_connected() == true);
    CHECK(rig.driver().get_position() == 1700);
}

// Falsified by: gemini_focuser_driver.cpp: get_link_fault() delete the override (the management listing never says why
// the link is down).
TEST_CASE(
    "Gemini Focuser Snapshot - the link fault text is reported while faulted and after the loss, and cleared by "
    "Disconnect",
    "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    CHECK(rig.driver().get_link_fault().empty());

    rig.fake().set_garbage_state_replies(true);
    for (int i = 0; i < 3; ++i) rig.tick();
    CHECK_FALSE(rig.driver().get_link_fault().empty());  // latched, Connected still true

    rig.clock().advance(31s);
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    CHECK(rig.driver().get_connected() == false);
    CHECK_FALSE(rig.driver().get_link_fault().empty());  // kept after the loss

    rig.driver().set_connected(false);
    CHECK(rig.driver().get_link_fault().empty());
}

// Falsified by: gemini_focuser_driver.cpp: probe() delete the cancel checks between the exchanges (a disconnect
// during a poll waits for the ":01#" and ":06#" exchanges too, each up to one serial timeout).
TEST_CASE("Gemini Focuser Snapshot - a disconnect during a poll waits for the exchange in flight only",
          "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    REQUIRE(connect_and_park(*driver, clock, kBound));

    // The next poll's first exchange (":00#") is held by the fake.
    fake.gate_next_reply();
    const int moving_reads = fake.count(":01#");
    const int temperature_reads = fake.count(":06#");
    clock.advance(kTick);
    REQUIRE(fake.wait_reply_held(kBound));

    std::atomic<pid_t> disconnect_tid{0};
    std::thread disconnect([&] {
        disconnect_tid.store(::gettid());
        driver->set_connected(false);
    });
    // Before the poll join teardown blocks only on uncontended mutexes, so the disconnecting thread is first asleep
    // in that join, after the cancel store.
    const bool joining = wait_asleep(disconnect_tid);
    fake.release_reply();
    disconnect.join();

    REQUIRE(joining);
    CHECK(fake.count(":01#") == moving_reads);
    CHECK(fake.count(":06#") == temperature_reads);
    CHECK(driver->get_link_fault().empty());
}

// Falsified by: gemini_focuser_driver.cpp: ~GeminiFocuserDriver() delete the poll_cancelled_ store before
// state_.stop_and_join() (removing a connected focuser during a poll waits for the ":01#" and ":06#" exchanges too).
TEST_CASE(
    "Gemini Focuser Snapshot - destroying a connected focuser during a poll waits for the exchange in flight only",
    "[gemini][focuser][snapshot][unit][fake]") {
    FakeGeminiFocuser fake;
    FakeTaskClock clock;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path(), 9600, clock);
    REQUIRE(connect_and_park(*driver, clock, kBound));

    fake.gate_next_reply();
    const int moving_reads = fake.count(":01#");
    const int temperature_reads = fake.count(":06#");
    clock.advance(kTick);
    REQUIRE(fake.wait_reply_held(kBound));

    std::atomic<pid_t> destroy_tid{0};
    std::thread destroy([&] {
        destroy_tid.store(::gettid());
        driver.reset();
    });
    // Before the poll join the destructor blocks only on an uncontended mutex, so the destroying thread is first
    // asleep in that join, after the cancel store.
    const bool joining = wait_asleep(destroy_tid);
    fake.release_reply();
    destroy.join();

    REQUIRE(joining);
    CHECK(fake.count(":01#") == moving_reads);
    CHECK(fake.count(":06#") == temperature_reads);
}

// Falsified by: gemini_focuser_driver.cpp: device_state_age() delete the override (the TimeStamp is the request time,
// not the frame's).
TEST_CASE("Gemini Focuser Snapshot - DeviceState TimeStamp is when the frame was measured",
          "[gemini][focuser][snapshot][unit][fake]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    rig.clock().advance(std::chrono::milliseconds(90));  // under the poll period: no poll, the frame ages
    std::string stamp;
    for (const auto& entry : rig.driver().get_device_state()) {
        if (entry.name == "TimeStamp") stamp = std::get<std::string>(entry.value);
    }
    const auto taken = std::chrono::system_clock::now();
    REQUIRE_FALSE(stamp.empty());

    std::tm tm{};
    int ms = 0;
    REQUIRE(std::sscanf(stamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                        &tm.tm_min, &tm.tm_sec, &ms) == 7);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const auto stamped = std::chrono::system_clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
    const auto behind = std::chrono::duration_cast<std::chrono::milliseconds>(taken - stamped);
    INFO("TimeStamp is " << behind.count() << " ms behind now");
    CHECK(behind >= std::chrono::milliseconds(60));  // the 90 ms frame age; a request-time stamp is ~0
    CHECK(behind < std::chrono::milliseconds(5000));
}

// ---------------------------------------------------------------------------
// Decision 0009 rule 2a (issue #1723): a pulled adapter loses the link at once,
// not after util::kLinkStalenessBound. Connected reads false, every member
// throws NotConnected, DeviceState is empty, the port is released, the poll
// ends, and only a client Connect reopens it.
// ---------------------------------------------------------------------------

namespace {

using alpacacore::test::ReplugPort;
using alpacacore::test::require_alpaca_error;

constexpr int kNotConnected = static_cast<int>(alpacacore::AlpacaError::NotConnected);

// Pulls the cable once the fake has received `cmd` more than `seen` times (it is then holding that reply, see
// hold_next_reply() and hold_reply_to()), so the exchange on the wire sees the hangup. Runs off the test thread:
// no test macros here.
std::thread unplug_when_asked(FakeGeminiFocuser& fake, std::string cmd, int seen) {
    return std::thread([&fake, cmd = std::move(cmd), seen] {
        const auto deadline = std::chrono::steady_clock::now() + kBound;
        while (fake.count(cmd) <= seen && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);  // real time: the fake's reader thread records the command
        }
        fake.unplug();
    });
}

// The members a lost link refuses, and the released port.
void require_lost(alpacacore::FocuserDriver& driver, const std::string& old_pts) {
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_device_state().empty());
    require_alpaca_error([&] { (void)driver.get_position(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_is_moving(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_temperature(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_max_step(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_temp_comp(); }, kNotConnected);
    require_alpaca_error([&] { driver.move(500); }, kNotConnected);
    require_alpaca_error([&] { driver.halt(); }, kNotConnected);
    // The wrapper released the port: no claim is left on the dead node.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(old_pts));
}

bool reports_loss(const alpacacore::FocuserDriver& driver) {
    return driver.get_link_fault().find("Serial link lost") != std::string::npos;
}

}  // namespace

TEST_CASE("Gemini Focuser Link - a cable pulled during a command loses the link at once",
          "[gemini][focuser][unit][fake][link-lost]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    const std::string old_pts = rig.fake().slave_path();
    REQUIRE(alpacacore::util::is_serial_port_in_use(old_pts));

    rig.fake().hold_next_reply(300ms);  // the focuser sits on the ":24#" reply while the cable is pulled
    std::thread pull = unplug_when_asked(rig.fake(), ":24#", rig.fake().count(":24#"));
    const auto start = std::chrono::steady_clock::now();
    require_alpaca_error([&] { (void)rig.driver().get_temp_comp(); }, kNotConnected);
    pull.join();
    CHECK(std::chrono::steady_clock::now() - start < 2s);  // the hangup, not a serial timeout
    require_lost(rig.driver(), old_pts);
    // Decision 0009 rule 5: the loss reason is kept while the link stays lost.
    CHECK(reports_loss(rig.driver()));

    // The next poll finds the link gone and ends; releasing the transport again does not drop the reason.
    rig.clock().advance(kTick);
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    CHECK(rig.clock().waiter_count() == 0);
    CHECK(reports_loss(rig.driver()));

    // Disconnect after the loss closes nothing twice and clears the reason.
    REQUIRE_NOTHROW(rig.driver().set_connected(false));
    CHECK_FALSE(rig.driver().get_connected());
    CHECK(rig.driver().get_link_fault().empty());
}

TEST_CASE("Gemini Focuser Link - a write to a pulled adapter loses the link at once",
          "[gemini][focuser][unit][fake][link-lost]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    const std::string old_pts = rig.fake().slave_path();

    rig.fake().unplug();  // idle: the next command's write is what meets the dead port
    require_alpaca_error([&] { rig.driver().halt(); }, kNotConnected);
    require_lost(rig.driver(), old_pts);
    CHECK(reports_loss(rig.driver()));
    REQUIRE_NOTHROW(rig.driver().set_connected(false));
    CHECK(rig.driver().get_link_fault().empty());  // a Disconnect clears the reason
}

TEST_CASE("Gemini Focuser Link - a cable pulled during a poll ends the poll at once",
          "[gemini][focuser][unit][fake][link-lost]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    const std::string old_pts = rig.fake().slave_path();

    rig.fake().hold_next_reply(300ms);  // the next poll's ":00#" reply
    std::thread pull = unplug_when_asked(rig.fake(), ":00#", rig.fake().count(":00#"));
    rig.clock().advance(kTick);
    // The woken poll body runs its exchange in real time, then exits instead of parking for the next tick.
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    pull.join();
    CHECK(rig.clock().waiter_count() == 0);
    require_lost(rig.driver(), old_pts);
    CHECK(reports_loss(rig.driver()));
    REQUIRE_NOTHROW(rig.driver().set_connected(false));
}

TEST_CASE("Gemini Focuser Link - a cable pulled during a poll's temperature read ends the poll at once",
          "[gemini][focuser][unit][fake][link-lost]") {
    GeminiRig rig;
    REQUIRE(rig.connect());
    const std::string old_pts = rig.fake().slave_path();

    // The ":06#" read is optional, so its failure is swallowed in measure(); the poll must still end in this cycle
    // (probe() throws, so the frame is dropped), not park for the next tick with the link already gone.
    rig.fake().hold_reply_to(":06#", 300ms);
    std::thread pull = unplug_when_asked(rig.fake(), ":06#", rig.fake().count(":06#"));
    rig.clock().advance(kTick);
    REQUIRE(rig.clock().wait_for_woken_settled(kBound));
    pull.join();
    CHECK(rig.clock().waiter_count() == 0);  // no second tick was needed: on_lost ran, the poll is not parked
    require_lost(rig.driver(), old_pts);
    CHECK(reports_loss(rig.driver()));
    REQUIRE_NOTHROW(rig.driver().set_connected(false));
}

TEST_CASE("Gemini Focuser Link - a cable pulled during the connect's first poll fails the Connect",
          "[gemini][focuser][unit][fake][link-lost]") {
    GeminiRig rig;
    rig.fake().hold_reply_to(":00#", 300ms);  // the seed's first exchange sits on its reply while the cable goes
    std::thread pull = unplug_when_asked(rig.fake(), ":00#", 0);
    require_alpaca_error([&] { rig.driver().set_connected(true); }, kNotConnected);
    pull.join();
    CHECK_FALSE(rig.driver().get_connected());
    CHECK_FALSE(rig.driver().get_device_firmware().has_value());
    CHECK(rig.clock().waiter_count() == 0);  // no poll body is left behind
    // A connect that never completed is not a lost session: the reason went to the client in the refusal.
    CHECK(rig.driver().get_link_fault().empty());
}

TEST_CASE("Gemini Focuser Link - a cable pulled during the connect's temperature read fails the Connect",
          "[gemini][focuser][unit][fake][link-lost]") {
    GeminiRig rig;
    // The ":06#" read is optional, so its hangup is swallowed by measure(); probe() sees the released port and
    // throws, so no frame publishes and the Connect fails NotConnected.
    rig.fake().hold_reply_to(":06#", 300ms);
    std::thread pull = unplug_when_asked(rig.fake(), ":06#", 0);
    require_alpaca_error([&] { rig.driver().set_connected(true); }, kNotConnected);
    pull.join();
    CHECK_FALSE(rig.driver().get_connected());
    CHECK(rig.clock().waiter_count() == 0);  // no poll body was started: the seed published no frame
    CHECK(rig.driver().get_link_fault().empty());
}

TEST_CASE("Gemini Focuser Link - a Connect over a lost link reopens without a Disconnect first",
          "[gemini][focuser][unit][fake][link-lost]") {
    FakeTaskClock clock;  // before the driver, which waits on it
    FakeGeminiFocuser focuser;
    FakeGeminiFocuser replugged;
    ReplugPort port(focuser);
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, port.path.string(), 9600, clock);
    REQUIRE(connect_and_park(*driver, clock, kBound));

    focuser.unplug();
    require_alpaca_error([&] { driver->halt(); }, kNotConnected);  // the write meets the dead port
    REQUIRE_FALSE(driver->get_connected());
    CHECK(reports_loss(*driver));

    // No Disconnect, and no poll tick since the loss: a Connect while the raw flag is still set must not be taken
    // as already connected.
    port.replug(replugged);
    replugged.set_position(1700);
    REQUIRE(connect_and_park(*driver, clock, kBound));
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // the next Connect clears the reason
    CHECK(driver->get_position() == 1700);
    CHECK(replugged.connects() == 1);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

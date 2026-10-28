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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>
#include <alpacacore/version.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_qhy_qfocuser.h"

namespace {

using alpacacore::test::require_alpaca_error;

// The getters are served from the state snapshot (open-astro#1310), so a dead
// port is seen by the next poll (250 ms period), not by the getter itself: wait
// for the getter to say NotConnected, and return how long that took.
std::chrono::milliseconds wait_not_connected(const std::function<void()>& fn,
                                             std::chrono::milliseconds budget = std::chrono::seconds(3)) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        try {
            fn();
        } catch (const alpacacore::AlpacaException& ex) {
            REQUIRE(ex.error_code() == static_cast<int>(alpacacore::AlpacaError::NotConnected));
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        }
        if (std::chrono::steady_clock::now() - start > budget) FAIL("the getter never reported the dead link");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Never a real device path: a plausible /dev/ttyACM0 could be the actual focuser.
constexpr const char* kAbsentPort = "/dev/qhy-qfocuser-absent";

// A by-id style symlink to the fake's pty, for cases that unplug the focuser
// and then expect a reconnect to fail. sever() empties the fake's own path,
// but the driver keeps the /dev/pts/N it was built with, and once that index
// is free devpts hands it to the next pty opened on the box: under ctest -j
// that is often a neighbouring case's fake, which answers the handshake and
// turns the expected NotConnected into a connect. A removed link is never
// reissued, so the driver is given this path and unplug() removes it.
struct UnpluggablePort {
    alpacacore::test::FakeQhyQFocuser& fake;
    std::filesystem::path path;
    explicit UnpluggablePort(alpacacore::test::FakeQhyQFocuser& f) : fake(f) {
        path = std::filesystem::temp_directory_path() /
               ("alpacacore-qfocuser-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_symlink(fake.slave_path(), path);
    }
    void unplug() {
        fake.sever();
        std::filesystem::remove(path);
    }
    ~UnpluggablePort() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    UnpluggablePort(const UnpluggablePort&) = delete;
    UnpluggablePort& operator=(const UnpluggablePort&) = delete;
};

}  // namespace

TEST_CASE("QHY Q-Focuser Driver - Defaults", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "QHY Q-Focuser");
    CHECK(driver->get_absolute() == true);
    // MaxStep and MaxIncrement are properties: a disconnected read refuses (issue #1478).
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_device_firmware().has_value() == false);
}

TEST_CASE("QHY Q-Focuser Driver - Device metadata", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(3, kAbsentPort);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY Q-Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore QHY Q-Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "QHY_QFOCUSER_3");
    auto other = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);
    CHECK(other->get_unique_id() != driver->get_unique_id());
}

TEST_CASE("QHY Q-Focuser Driver - Not connected throws", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_temp_comp(false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(1000); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY Q-Focuser Driver - Unsupported actions", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("QHY Q-Focuser Driver - Reply parser", "[qhy][focuser][unit]") {
    using alpacacore::vendor::qhy::parse_qfocuser_reply;

    auto f = parse_qfocuser_reply("{\"idx\":1,\"id\":\"\\u001f@SL3KG\\u0018TH2C\",\"version\":20231207,\"bv\":208}");
    REQUIRE(f.size() == 4);
    CHECK(f["idx"] == "1");
    CHECK(f["version"] == "20231207");
    CHECK(f["bv"] == "208");
    CHECK(f["id"] == ".@SL3KG.TH2C");

    auto t = parse_qfocuser_reply("{\"idx\":4,\"temp\":120683,\"c_t\":18727,\"c_r\":125,\"o_t\":-5250,\"sg\":0}");
    CHECK(t["o_t"] == "-5250");
    CHECK(t["c_r"] == "125");

    CHECK(parse_qfocuser_reply("{\"idx\":-1}")["idx"] == "-1");
    CHECK(parse_qfocuser_reply("garbage").empty());
    CHECK(parse_qfocuser_reply("{\"idx\":5,\"pos\":").empty());
    CHECK(parse_qfocuser_reply("{}").empty());
}

TEST_CASE("QHY Q-Focuser Driver - Value range validation", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    alpacacore::vendor::qhy::QFocuserSettings settings;
    settings.max_step = 5000;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), settings);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    CHECK(driver->get_max_step() == 5000);
    require_alpaca_error([&]() { driver->move(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move(5001); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(fake.count("\"cmd_id\":6") == 0);  // rejected before reaching the wire
    CHECK_NOTHROW(driver->move(5000));
    CHECK(fake.count("\"cmd_id\":6,\"tar\":5000}") == 1);

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Driver - State machine", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") has_timestamp = true;
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("QHY Q-Focuser Driver - Connect applies settings and reports motion", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    fake.set_position(1000);
    fake.set_steps_per_poll(400);

    alpacacore::vendor::qhy::QFocuserSettings settings;
    settings.reverse = true;
    settings.speed = 3;
    settings.hold_force = true;
    settings.hold_ihold = 6;
    settings.hold_irun = 10;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), settings);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    CHECK(fake.connects() == 1);
    CHECK(fake.count("\"cmd_id\":7,\"rev\":1}") == 1);
    CHECK(fake.count("\"cmd_id\":13,\"speed\":3}") == 1);
    CHECK(fake.count("\"cmd_id\":16,\"ihold\":6,\"irun\":10}") == 1);
    CHECK(fake.count("\"cmd_id\":12,\"force\":1}") == 1);
    CHECK(driver->get_device_firmware().value_or("") == "20231207 (board 208)");

    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_temperature() == Catch::Approx(23.881));
    CHECK(driver->get_temp_comp_available() == false);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotImplemented);

    driver->move(2000);
    // 400 steps/poll from 1000 never reaches 2000 on the first poll, so this is
    // deterministic. The exact intermediate position is not asserted: reads
    // follow the 250 ms poll, so on a loaded or ASan runner the fake could
    // have advanced — the settle loop below
    // proves arrival instead.
    CHECK(driver->get_is_moving() == true);
    for (int i = 0; i < 20 && driver->get_is_moving(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_position() == 2000);

    // Halt: the fake stops mid-move and the driver drops its target.
    fake.set_steps_per_poll(1);
    driver->move(3000);
    CHECK(driver->get_is_moving() == true);
    driver->halt();
    CHECK(fake.count("\"cmd_id\":3}") == 1);
    CHECK(driver->get_is_moving() == false);

    // Platform 7 DeviceState carries the operational trio while connected.
    bool has_moving = false, has_position = false, has_temperature = false;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "IsMoving") has_moving = true;
        if (entry.name == "Position") has_position = true;
        if (entry.name == "Temperature") has_temperature = true;
    }
    CHECK(has_moving);
    CHECK(has_position);
    CHECK(has_temperature);

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_device_firmware().has_value() == false);
}

TEST_CASE("QHY Q-Focuser Driver - Hold settings skipped on USB power", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    fake.set_voltage_tenths(50);  // 5.0 V: USB-powered
    alpacacore::vendor::qhy::QFocuserSettings settings;
    settings.hold_force = true;
    settings.temperature_source = "chip";
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), settings);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    CHECK(fake.count("\"cmd_id\":16") == 0);
    CHECK(fake.count("\"cmd_id\":12") == 0);
    CHECK(driver->get_temperature() == Catch::Approx(18.727));

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Driver - Absent port fails connect with NotConnected", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_connected() == false);
}

// Regression guard for the one-reply-behind firmware behaviour (PR #526
// review). With the fake in one-behind mode, each reply is held until a later
// inbound OUT, so a driver that writes a command and reads without ever
// sending a kick gets nothing and times out — this case fails if the kick is
// removed from transact_locked() entirely.
//
// Scope, stated honestly: a pty has no USB OUT-packet boundary, so this cannot
// reproduce the coalescing that makes tcdrain matter, nor distinguish the fast
// proactive kick from the 200 ms fallback kick. tcdrain's role (keeping the
// command and the kick in separate USB packets on real hardware) and the
// per-read latency that the proactive kick protects are validated on the Pi
// via ConformU's FAST timing gate, not here. What this pins is that the kick
// mechanism and read-until-idx loop are required to talk to one-behind
// firmware at all.
TEST_CASE("QHY Q-Focuser Driver - one-behind firmware requires the kick", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    fake.set_one_behind(true);
    fake.set_position(1500);
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(fake.connects() >= 1);
    // Reads must reach the awaited reply despite every reply arriving one OUT
    // late; the kick is what clocks each one out.
    CHECK(driver->get_position() == 1500);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_temperature() == Catch::Approx(23.881));

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

// A mid-session link loss (USB unplug / CDC-ACM re-enumeration) must fail a
// transaction promptly, not spin a core until the serial timeout. Severing the
// pty makes the driver's next read see POLLHUP/EOF; transact_locked() maps that
// to NotConnected at once. The timing bound is the regression guard: a busy-spin
// would take the full ~3 s serial timeout.
TEST_CASE("QHY Q-Focuser Driver - dead link fails fast, no busy-spin", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    fake.sever();

    const auto elapsed = wait_not_connected([&]() { driver->get_position(); }).count();
    CHECK(elapsed < 1500);  // one 250 ms poll tick plus a fast failure, « the 3 s serial timeout a busy-spin would burn

    driver->set_connected(false);
}

// Issue #527: once the link is gone, Connected must read false without a
// reconnect (the #445 shape), every operation must throw NotConnected, and
// Connected=true must reconnect rather than take the idempotency early return.
TEST_CASE("QHY Q-Focuser Driver - lost link drops Connected and reconnects", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    UnpluggablePort port(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, port.path.string());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(fake.connects() == 1);

    port.unplug();
    wait_not_connected([&]() { driver->get_position(); });
    CHECK(driver->get_connected() == false);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(1200); }, alpacacore::AlpacaError::NotConnected);
    // A lost link refuses MaxStep and MaxIncrement too (ADR 0009: every member throws NotConnected).
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);

    // Connected=true against the dead link is a reconnect attempt, not a
    // no-op: the unplugged port's link is gone, so the open fails and throws.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_connected() == false);

    // Connected=false still tears down cleanly.
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

// Issue #527: a present tty that answers with brace-less garbage makes the
// reader return early on every slice. Without a pause between fallback kicks
// that path re-kicked in a tight loop (thousands of kicks per second) until
// the 3 s deadline; the kick count bounds the rate.
TEST_CASE("QHY Q-Focuser Driver - garbage replies do not busy-spin the kick loop", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The state poll (250 ms period) now drives the wire: let the first garbage
    // poll start, then watch it through its 3 s deadline plus the next poll.
    fake.set_garbage(true);
    const int before = fake.kicks();
    const auto start = std::chrono::steady_clock::now();
    while (fake.kicks() == before && std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const int first = fake.kicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    const int kicks = fake.kicks() - before;
    INFO("kicks=" << kicks << " first=" << first - before);
    // 3 s deadline / 50 ms floor = 60 kicks at most per poll, plus the proactive
    // one; the window holds at most two polls. A busy-spin is thousands.
    CHECK(kicks <= 140);
    CHECK(kicks >= 1);

    // Garbage is no valid reply: the polls fail, three in a row latch the
    // fault, and the getter reports it (as get_position() did before the
    // snapshot, when the read itself hit the wire). Three 3 s polls.
    const auto fault_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        try {
            (void)driver->get_position();
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == static_cast<int>(alpacacore::AlpacaError::DriverException));
            break;
        }
        if (std::chrono::steady_clock::now() > fault_deadline) FAIL("garbage polls never latched the fault");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    fake.set_garbage(false);
    driver->set_connected(false);
}

// Issue #528: a synchronous disconnect that lands while a synchronous connect
// is inside the wrapper's handshake (which sleeps 100 ms before the first
// exchange) must not be dropped. Without transition_mutex_ thread B saw
// "not connected" twice and returned as a no-op while A stored true.
TEST_CASE("QHY Q-Focuser Driver - sync disconnect during sync connect is not dropped", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());

    std::thread a([&] {
        try {
            driver->set_connected(true);
        } catch (const std::exception&) {
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // inside A's pre-handshake sleep
    driver->set_connected(false);
    a.join();

    CHECK(driver->get_connected() == false);
    CHECK(fake.connects() == 1);
    driver->set_connected(false);
}

// Issue #527 review (PR #531): an explicit Connected=false straight after the
// link loss, with no reconnect attempt in between, must still run the full
// teardown. The link-aware idempotency check saw false == false and returned
// early, leaving connected_ true and the firmware cache populated, which the
// management endpoint renders without a Connected check.
TEST_CASE("QHY Q-Focuser Driver - explicit disconnect after lost link clears state", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    UnpluggablePort port(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, port.path.string());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_device_firmware().has_value());

    port.unplug();
    wait_not_connected([&]() { driver->get_position(); });
    CHECK(driver->get_connected() == false);

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_device_firmware().has_value() == false);
    // And a later Connected=true is a plain connect, not a stale-link reconnect.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
}

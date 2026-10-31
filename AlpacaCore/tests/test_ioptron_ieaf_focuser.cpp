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
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_protocol_wrapper.h>
#include <alpacacore/version.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_ioptron_ieaf.h"
#include "fake_task_clock.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("iOptron iEAF Focuser Driver - Defaults", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "iOptron iEAF");
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    CHECK(driver->get_absolute() == true);
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEAF Focuser Driver - Metadata", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iEAF Electronic Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iEAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "IOPTRON_IEAF_3");
}

TEST_CASE("iOptron iEAF Focuser Driver - Not connected throws", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(1, "/dev/ttyUSB0");

    REQUIRE(driver->get_connected() == false);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(1000); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEAF Focuser Driver - Unsupported actions", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("iOptron iEAF Focuser Driver - Static capabilities", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    // Range limits are fixed by the protocol (7-digit move field, hardware range 0..99999), but MaxStep and
    // MaxIncrement are properties: a disconnected read refuses (issue #1478), the value is read connected below.
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);

    // Step size in microns is not exposed by the iEAF protocol -- but
    // disconnected, the connection check answers first (open-astro#309).
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEAF Focuser Driver - unsupported methods refuse while disconnected", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    // No temperature compensation in hardware. Disconnected, all three are
    // NotConnected (open-astro#309): the connection check precedes the
    // not-implemented answer, so the driver never reports a capability -- or
    // refuses a write on capability grounds -- for hardware it has not opened.
    //
    // The NotImplemented answer a CONNECTED iEAF gives is unchanged but is NOT
    // asserted anywhere: there is no fake for this focuser (fake_ioptron_mount.h
    // fakes the mount, and every create_ieaf_focuser() in the suite points at a
    // /dev path that never opens), so deleting either throw would leave the
    // suite green. The ToupTek and Gemini focusers, which do have fakes, carry
    // connected cases for exactly this; the iEAF needs a serial fake of its own
    // before it can have one.
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_temp_comp(false); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEAF Focuser Driver - State machine", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    // Platform 7 DeviceState while disconnected: operational getters throw and
    // are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; no non-compliant "Connected".
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("iOptron iEAF Focuser Driver - Unique IDs", "[ioptron][focuser][unit]") {
    auto driver0 = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::ioptron::create_ieaf_focuser(1, "/dev/ttyUSB1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "IOPTRON_IEAF_0");
    CHECK(driver1->get_unique_id() == "IOPTRON_IEAF_1");
}

TEST_CASE("iOptron iEAF Focuser Driver - Model names", "[ioptron][focuser][unit]") {
    using alpacacore::vendor::ioptron::is_ieaf_model;
    // :DeviceInfo# model codes: 2 = iEAF, 3 = iAFS2/3, anything else is not ours.
    CHECK(is_ieaf_model(2));
    CHECK(is_ieaf_model(3));
    CHECK_FALSE(is_ieaf_model(0));
    CHECK_FALSE(is_ieaf_model(1));
    CHECK_FALSE(is_ieaf_model(4));

    // Default model is the iEAF.
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/null");
    CHECK(driver->get_name() == "iOptron iEAF");
    CHECK(driver->get_description() == "iOptron iEAF Electronic Focuser Driver");

    // The config model selects the name; no handshake involved.
    auto iafs2 = alpacacore::vendor::ioptron::create_ieaf_focuser(1, "/dev/null", "iafs2");
    CHECK(iafs2->get_name() == "iOptron iAFS2/3");
    CHECK(iafs2->get_description() == "iOptron iAFS2/3 Electronic Focuser Driver");
    auto ieaf = alpacacore::vendor::ioptron::create_ieaf_focuser(2, "/dev/null", "ieaf");
    CHECK(ieaf->get_name() == "iOptron iEAF");
    // Unique IDs stay model-independent so a persisted device keeps its identity.
    CHECK(iafs2->get_unique_id() == "IOPTRON_IEAF_1");
}

// Hardware-free connect over the pty fake: the handshake, status read and
// move round-trip, so the lifecycle tests below exercise a real connect.
TEST_CASE("iOptron iEAF Focuser Driver - connects and moves over the fake", "[ioptron][focuser][unit]") {
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(fake.connects() == 1);
    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_max_step() == 99999);
    CHECK(driver->get_max_increment() == 99999);
    driver->move(1500);
    CHECK(fake.count(":FM") == 1);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

// #1590: the 100 ms status cache ages by the driver's TaskClock.
TEST_CASE("iOptron iEAF Focuser Driver - the status cache ages by the TaskClock",
          "[ioptron][focuser][unit][taskclock]") {
    alpacacore::test::FakeIoptronIeaf fake;
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path(), "ieaf", clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    clock.advance(std::chrono::seconds(1));  // outdate whatever the connect cached
    (void)driver->get_position();
    const int polls = fake.count(":FI#");
    (void)driver->get_position();
    (void)driver->get_is_moving();
    CHECK(fake.count(":FI#") == polls);  // within 100 ms: served from the cache
    clock.advance(std::chrono::milliseconds(150));
    (void)driver->get_position();
    CHECK(fake.count(":FI#") == polls + 1);  // past STATUS_CACHE_TTL: polled again
    driver->set_connected(false);
}

// Parameter validation precedes the connection check (ASCOM precedence).
TEST_CASE("iOptron iEAF Focuser Driver - move range is InvalidValue even while disconnected",
          "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");
    require_alpaca_error([&]() { driver->move(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move(99999 + 1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move(1000); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEAF Focuser Driver - move range over the fake", "[ioptron][focuser][unit]") {
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    require_alpaca_error([&]() { driver->move(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move(99999 + 1); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(fake.count(":FM") == 0);
    driver->set_connected(false);
}

// Issue #528: a synchronous disconnect that lands while a synchronous connect
// is inside the wrapper's handshake (which sleeps 100 ms before the first
// exchange) must not be dropped. Without transition_mutex_ thread B saw
// "not connected" twice (nothing async in flight, idempotent) and returned as
// a no-op while A went on to store true.
TEST_CASE("iOptron iEAF Focuser Driver - sync disconnect during sync connect is not dropped",
          "[ioptron][focuser][unit]") {
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());

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

// Issue #995: a connected iOptron link is held exclusively (TIOCEXCL), so a
// second open of the same tty, which is what every iOptron auto-detect probe
// does to each candidate, fails with EBUSY and the probe skips the port
// instead of flushing the connected driver's replies and writing its own query.
TEST_CASE("iOptron iEAF Focuser Driver - a connected port refuses a second open", "[ioptron][focuser][unit]") {
    if (geteuid() == 0) {
        SUCCEED("root bypasses TIOCEXCL");
        return;
    }
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    errno = 0;
    const int fd = open(fake.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    const int err = errno;
    if (fd >= 0) close(fd);
    CHECK(fd < 0);
    CHECK(err == EBUSY);

    // A second driver on the same node stands in for a probe's open().
    auto intruder = alpacacore::vendor::ioptron::create_ieaf_focuser(1, fake.slave_path());
    require_alpaca_error([&] { intruder->set_connected(true); },
                         static_cast<int>(alpacacore::AlpacaError::NotConnected));
    CHECK(intruder->get_connected() == false);

    CHECK(fake.connects() == 1);
    CHECK(driver->get_position() == 1000);
    driver->set_connected(false);
}

// A device that drops off the bus between the command and its reply: the wrapper reports the hangup and the
// errno at once, not a read timeout after serial_timeout_s.
// Falsified by: ioptron_ieaf_protocol_wrapper.cpp:462 read_response() sending ReadOutcome::LinkDead to the timeout
// branch (the message reads "iEAF read timeout" and the call takes the full serial_timeout_s).
TEST_CASE("iOptron iEAF protocol wrapper - a hangup before the reply throws serial link lost with the errno",
          "[ioptron][focuser][unit][link-dead]") {
    alpacacore::test::FakeIoptronIeaf fake;
    alpacacore::vendor::ioptron::IeafProtocolWrapper wrapper;
    alpacacore::vendor::ioptron::IeafConnectionConfig config;
    config.serial_port = fake.slave_path();
    wrapper.connect(config);
    fake.hold_next_reply(std::chrono::milliseconds(300));  // the fake sits on the :FI# reply while the cable is pulled
    std::thread pull([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        fake.unplug();
    });
    const auto start = std::chrono::steady_clock::now();
    std::string what;
    try {
        wrapper.get_status();
        FAIL("a hung-up port must not return a value");
    } catch (const alpacacore::AlpacaException& ex) {
        what = ex.what();
    }
    pull.join();
    CHECK(what == "iEAF serial link lost (errno 5)");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    try {
        wrapper.disconnect();
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Decision 0009 rule 2 (issue #1723): a pulled cable loses the link at once.
// Connected reads false, every member throws NotConnected, DeviceState is
// empty, the port is released, get_link_fault() keeps the reason until the
// next Connect or Disconnect, and only a client Connect reopens it.
// ---------------------------------------------------------------------------

namespace {

using alpacacore::test::ReplugPort;
using std::chrono::milliseconds;

// Real time: these waits are for the fake's reader thread to see a command on the wire.
template <typename Pred>
bool wait_until(Pred pred, milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

// The AlpacaException code fn threw, or 0. Safe on a non-test thread.
int alpaca_code_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.error_code();
    } catch (...) {
        return -1;
    }
    return 0;
}

constexpr int kNotConnected = static_cast<int>(alpacacore::AlpacaError::NotConnected);

// What a lost link refuses; old_pts is the node the driver had open.
void require_lost(alpacacore::FocuserDriver& driver, const std::string& old_pts) {
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_device_state().empty());
    require_alpaca_error([&] { (void)driver.get_position(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_is_moving(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_temperature(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver.get_max_step(); }, kNotConnected);
    require_alpaca_error([&] { driver.move(1200); }, kNotConnected);
    require_alpaca_error([&] { driver.halt(); }, kNotConnected);
    // The wrapper released the port: no claim is left on the dead node.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(old_pts));
}

}  // namespace

TEST_CASE("iEAF Link - a cable pulled during a status read loses the link at once and Connect reopens",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeIoptronIeaf fake;
    alpacacore::test::FakeIoptronIeaf replugged;
    replugged.set_position(2000);
    ReplugPort port(fake);
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, port.path.string(), "ieaf", clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_device_firmware().has_value());
    const std::string old_pts = fake.slave_path();
    REQUIRE(alpacacore::util::is_serial_port_in_use(old_pts));

    clock.advance(std::chrono::seconds(1));  // no cached frame: the read goes to the wire
    const int polls = fake.count(":FI#");
    fake.hold_next_reply(milliseconds(300));  // the focuser sits on the :FI# reply while the cable is pulled
    std::atomic<int> code{0};
    std::thread caller([&] { code.store(alpaca_code_of([&] { (void)driver->get_position(); })); });
    REQUIRE(wait_until([&] { return fake.count(":FI#") == polls + 1; }, milliseconds(2000)));
    const auto pulled = std::chrono::steady_clock::now();
    fake.unplug();
    caller.join();
    CHECK(std::chrono::steady_clock::now() - pulled < std::chrono::seconds(2));  // not the 4 s read timeout
    CHECK(code.load() == kNotConnected);
    require_lost(*driver, old_pts);
    CHECK_FALSE(driver->get_device_firmware().has_value());
    // Decision 0009 rule 5: the loss reason is kept while the link stays lost.
    CHECK(driver->get_link_fault() == "iEAF serial link lost (errno 5)");

    // Nothing reopens it on its own; a client Connect over the lost link does.
    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK(driver->get_position() == 2000);
    CHECK(replugged.connects() == 1);
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("iEAF Link - a cable pulled before a move's ack loses the link at once",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeIoptronIeaf fake;
    ReplugPort port(fake);
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, port.path.string(), "ieaf", clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = fake.slave_path();

    // The ack is optional (a silent firmware only costs the ack wait), but a hangup there is not silence.
    fake.hold_next_reply(milliseconds(300));
    std::atomic<int> code{0};
    std::thread caller([&] { code.store(alpaca_code_of([&] { driver->move(1500); })); });
    REQUIRE(wait_until([&] { return fake.count(":FM") == 1; }, milliseconds(2000)));
    fake.unplug();
    caller.join();
    CHECK(code.load() == kNotConnected);
    require_lost(*driver, old_pts);
    CHECK(driver->get_link_fault() == "iEAF serial link lost (errno 5)");
    REQUIRE_NOTHROW(driver->set_connected(false));
}

TEST_CASE("iEAF Link - a write to a pulled focuser loses the link at once and Disconnect clears the reason",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeIoptronIeaf fake;
    ReplugPort port(fake);
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, port.path.string(), "ieaf", clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = fake.slave_path();

    fake.unplug();  // idle: the next command's write is what meets the dead port
    require_alpaca_error([&] { driver->halt(); }, kNotConnected);
    require_lost(*driver, old_pts);
    CHECK(driver->get_link_fault() == "iEAF serial link lost (errno 5)");
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // a Disconnect clears the reason
}

TEST_CASE("iEAF Link - a status frame cached before the loss is not served",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;  // never advanced: the cached frame stays inside its 100 ms
    alpacacore::test::FakeIoptronIeaf fake;
    ReplugPort port(fake);
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, port.path.string(), "ieaf", clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_position() == 1000);  // fills the status cache

    fake.unplug();
    require_alpaca_error([&] { driver->halt(); }, kNotConnected);  // the write meets the dead port
    require_alpaca_error([&] { (void)driver->get_position(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver->get_is_moving(); }, kNotConnected);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, kNotConnected);
    CHECK(driver->get_device_state().empty());
    REQUIRE_NOTHROW(driver->set_connected(false));
}

TEST_CASE("iEAF Link - a Connect over a lost link reopens without a Disconnect and drops the old frame",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;  // never advanced: a frame from before the loss would still be fresh
    alpacacore::test::FakeIoptronIeaf fake;
    alpacacore::test::FakeIoptronIeaf replugged;
    replugged.set_position(2000);
    ReplugPort port(fake);
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, port.path.string(), "ieaf", clock);
    driver->set_connected(true);
    REQUIRE(driver->get_position() == 1000);  // fills the status cache

    fake.unplug();
    require_alpaca_error([&] { driver->halt(); }, kNotConnected);
    REQUIRE_FALSE(driver->get_connected());

    // No Disconnect: a Connect while the raw flag is still set must not be taken as already connected.
    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_position() == 2000);  // the replugged focuser's frame, not the cached one
    CHECK(replugged.count(":FI#") == 1);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

TEST_CASE("iEAF Link - a cable pulled during the connect handshake fails the Connect and latches nothing",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeIoptronIeaf fake;
    alpacacore::test::FakeIoptronIeaf replugged;
    ReplugPort port(fake);
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, port.path.string(), "ieaf", clock);
    const std::string old_pts = fake.slave_path();

    fake.hold_next_reply(milliseconds(300));  // the first :DeviceInfo# reply is held while the cable goes
    std::atomic<int> code{0};
    std::thread connecting([&] { code.store(alpaca_code_of([&] { driver->set_connected(true); })); });
    REQUIRE(wait_until([&] { return fake.connects() == 1; }, milliseconds(2000)));
    fake.unplug();
    connecting.join();  // the handshake's own retries (about 2 s), then the failure
    CHECK(code.load() == kNotConnected);
    CHECK_FALSE(driver->get_connected());
    // A connect that never completed is not a lost session: no reason is kept, and the port is closed.
    CHECK(driver->get_link_fault().empty());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(old_pts));

    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_position() == 1000);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

TEST_CASE("iEAF Link - the wrapper keeps the loss reason until the next connect or disconnect",
          "[ioptron][focuser][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    alpacacore::test::FakeIoptronIeaf replugged;
    alpacacore::vendor::ioptron::IeafProtocolWrapper wrapper;
    alpacacore::vendor::ioptron::IeafConnectionConfig config;
    config.serial_port = fake.slave_path();
    wrapper.connect(config);
    REQUIRE(wrapper.link_alive());
    REQUIRE(wrapper.link_fault().empty());

    fake.unplug();
    require_alpaca_error([&] { wrapper.halt(); }, kNotConnected);
    CHECK_FALSE(wrapper.link_alive());
    CHECK_FALSE(wrapper.is_connected());
    CHECK(wrapper.link_fault() == "iEAF serial link lost (errno 5)");

    config.serial_port = replugged.slave_path();
    wrapper.connect(config);  // a successful connect clears both
    CHECK(wrapper.link_alive());
    CHECK(wrapper.link_fault().empty());

    replugged.unplug();
    require_alpaca_error([&] { (void)wrapper.get_status(); }, kNotConnected);
    REQUIRE(wrapper.link_fault() == "iEAF serial link lost (errno 5)");
    wrapper.disconnect();  // so does a disconnect
    CHECK(wrapper.link_fault().empty());
}

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
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iefw_protocol_wrapper.h>
#include <alpacacore/version.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_ioptron_ieaf.h"

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("iOptron iEFW Filter Wheel Driver - Defaults", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    // Default model is the iEFW-15; "iefw18" selects the other name.
    CHECK(driver->get_name() == "iOptron iEFW-15");
    CHECK(alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0, "iefw18")->get_name() ==
          "iOptron iEFW-18");
    CHECK(alpacacore::vendor::ioptron::create_iefw_filterwheel(0, "/dev/null", "iefw18")->get_name() ==
          "iOptron iEFW-18");
    CHECK(driver->get_connecting() == false);
    // Firmware is only known while connected; the web UI hides the row otherwise.
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Device metadata", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iEFW Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iEFW Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "IOPTRON_IEFW_3");
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Not connected throws", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(1, "/dev/ttyUSB0");

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    // Never opened the port, so the sync disconnect path is a no-op.
    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Unsupported actions", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Names and offsets before connect", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    // Slot count is unknown until the handshake, so config-time values are
    // accepted at any length and resized at connect (ZWO EFW semantics).
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10, -5}));
    REQUIRE_NOTHROW(driver->set_names({"L", "", "B"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10, -5});
    // Empty entries get the "Filter N" default immediately.
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "Filter 2", "B"});
    // A lone shorthand token is left alone until the slot count is known.
    REQUIRE_NOTHROW(driver->set_names({"LRGBSHOC"}));
    REQUIRE(driver->get_names() == std::vector<std::string>{"LRGBSHOC"});
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Value range validation", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(2, "/dev/ttyUSB0");

    // Range validation precedes the connection check (ASCOM precedence): a
    // negative position is InvalidValue even while disconnected.
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(-100); }, alpacacore::AlpacaError::InvalidValue);
    // The upper bound depends on the slot count, which needs the handshake,
    // so a large value while disconnected is NotConnected, not InvalidValue.
    require_alpaca_error([&]() { driver->set_position(8); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - State machine", "[ioptron][filterwheel][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(4, 1);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        REQUIRE(entry.name != "Position");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Model codes", "[ioptron][filterwheel][unit]") {
    using alpacacore::vendor::ioptron::iefw_model_name;
    using alpacacore::vendor::ioptron::iefw_slot_count;
    using alpacacore::vendor::ioptron::is_iefw_model;

    // :DeviceInfo# model codes (INDI ioptron_wheel.cpp): 99 = iEFW-15,
    // 98 = iEFW-18. Confirmed on an iEFW-15 (code 99, 5 slots).
    CHECK(is_iefw_model(99));
    CHECK(is_iefw_model(98));
    // The focusers (2 = iEAF, 3 = iAFS2/3) share the handshake but must be rejected here.
    CHECK_FALSE(is_iefw_model(2));
    CHECK_FALSE(is_iefw_model(3));
    CHECK_FALSE(is_iefw_model(0));
    CHECK_FALSE(is_iefw_model(97));

    CHECK(iefw_slot_count(99) == 5);
    CHECK(iefw_slot_count(98) == 8);
    CHECK(iefw_slot_count(2) == 0);

    CHECK(iefw_model_name(99) == "iEFW-15");
    CHECK(iefw_model_name(98) == "iEFW-18");
    CHECK(iefw_model_name(0) == "iEFW");
}

TEST_CASE("iOptron iEFW Filter Wheel Driver - Unique IDs", "[ioptron][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(1, 0);
    auto d5 = alpacacore::vendor::ioptron::create_iefw_filterwheel(5, "/dev/ttyUSB1");

    CHECK(d0->get_unique_id() != d1->get_unique_id());
    CHECK(d5->get_unique_id() == "IOPTRON_IEFW_5");
    CHECK(d5->get_device_number() == 5);
}

// Issue #995: a connected iEFW link is held exclusively, is released on
// disconnect, and a connect aimed at a port another driver holds is refused
// (what an auto-detect probe does: skip the candidate).
TEST_CASE("iOptron iEFW Filter Wheel Driver - a connected port refuses a second open", "[ioptron][filterwheel][unit]") {
    if (geteuid() == 0) {
        SUCCEED("root bypasses TIOCEXCL");
        return;
    }
    // The iEAF fake answers the same :DeviceInfo# handshake; model 99 = iEFW-15.
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    alpacacore::vendor::ioptron::IefwProtocolWrapper wheel;
    alpacacore::vendor::ioptron::IefwConnectionConfig cfg;
    cfg.serial_port = fake.slave_path();
    cfg.serial_timeout_s = 2;
    wheel.connect(cfg);
    REQUIRE(wheel.is_connected());

    errno = 0;
    int fd = open(fake.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    const int err = errno;
    if (fd >= 0) close(fd);
    CHECK(fd < 0);
    CHECK(err == EBUSY);

    alpacacore::vendor::ioptron::IefwProtocolWrapper intruder;
    require_alpaca_error([&] { intruder.connect(cfg); }, static_cast<int>(alpacacore::AlpacaError::NotConnected));

    wheel.disconnect();
    fd = open(fake.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

// --- Position served through util::TtlStatusCache (issue #1072) ---
//
// The fake answers the iEFW handshake (model 99 = iEFW-15, 5 slots) and :WP#, so every Position read
// that reaches the wire is counted in fake.count(":WP#"). The cache TTL is 100 ms; kPastTtl clears it.

namespace {

using namespace std::chrono_literals;
constexpr auto kPastTtl = 150ms;

struct ConnectedIefw {
    alpacacore::test::FakeIoptronIeaf fake;
    std::unique_ptr<alpacacore::FilterWheelDriver> driver;

    ConnectedIefw() {
        fake.set_model(99);
        driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(0, fake.slave_path());
        driver->set_connected(true);
    }
    ~ConnectedIefw() {
        try {
            driver->set_connected(false);
        } catch (...) {
        }
    }
};

}  // namespace

// Falsified by: ioptron_filterwheel_driver.cpp get_position() calling protocol_.get_position() on every
// read (two :WP# on the wire inside the TTL, not one), or a cache that never expires (no third :WP#).
TEST_CASE("iOptron iEFW Filter Wheel Driver - two Position reads inside the TTL cost one :WP#",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_slot(3);
    const int before = wheel.fake.count(":WP#");

    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.fake.count(":WP#") == before + 1);

    wheel.fake.set_wheel_slot(1);  // changes behind the driver's back: the frame still stands
    CHECK(wheel.driver->get_position() == 3);
    std::this_thread::sleep_for(kPastTtl);
    CHECK(wheel.driver->get_position() == 1);
    CHECK(wheel.fake.count(":WP#") == before + 2);
}

// Falsified by: ioptron_filterwheel_driver.cpp set_position() without status_cache_.invalidate() after
// :WM (the read after the move is served from the pre-move frame: one :WP#, and the latch reads -1 over the cached
// old slot instead of the target 4).
TEST_CASE("iOptron iEFW Filter Wheel Driver - a Position read after set_position goes to the wire",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_slot(3);
    const int before = wheel.fake.count(":WP#");
    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.driver->get_position() == 3);

    wheel.driver->set_position(4);
    CHECK(wheel.driver->get_position() == 4);
    CHECK(wheel.fake.count(":WP#") == before + 2);
    CHECK(wheel.fake.count(":WM04#") == 1);
}

// The iEFW connect sleeps 100 ms before its handshake, so a stale frame cannot outlive a reconnect here; the
// link-health record can. Two failures before the reconnect and two after must not add up to the three that
// latch the fault.
// Falsified by: ioptron_filterwheel_driver.cpp connect and disconnect both without status_cache_.reset() (the
// second failed read after the reconnect throws "communications compromised"). Either call alone is not
// caught: the other one clears the count too (the connect reset covers a refill that races a disconnect).
TEST_CASE("iOptron iEFW Filter Wheel Driver - a reconnect clears the Position link-health count",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_slot(3);
    wheel.fake.fail_wheel_position(true);
    CHECK_THROWS_AS(wheel.driver->get_position(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(wheel.driver->get_position(), alpacacore::AlpacaException);
    wheel.fake.fail_wheel_position(false);

    wheel.driver->set_connected(false);
    wheel.driver->set_connected(true);
    wheel.fake.fail_wheel_position(true);
    CHECK_THROWS_AS(wheel.driver->get_position(), alpacacore::AlpacaException);
    try {
        wheel.driver->get_position();
        FAIL("a failed read must throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
    }
    wheel.fake.fail_wheel_position(false);
}

// Falsified by: ioptron_filterwheel_driver.cpp reading Position without the cache's fault threshold (the
// third failed read throws the raw parse error, not "iOptron iEFW communications compromised"), or serving
// the last good frame after a failed refill (the first failed read returns 3).
TEST_CASE("iOptron iEFW Filter Wheel Driver - failed Position reads are not served and latch a link fault",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_slot(3);
    CHECK(wheel.driver->get_position() == 3);
    std::this_thread::sleep_for(kPastTtl);

    wheel.fake.fail_wheel_position(true);
    for (int i = 0; i < 2; ++i) {  // below the threshold the raw failure comes through, not the latch
        try {
            wheel.driver->get_position();
            FAIL("a failed read must throw");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
        }
    }
    try {
        wheel.driver->get_position();
        FAIL("third consecutive failure must throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }

    // A good refill clears the latch with no reconnect, and its frame is served again.
    wheel.fake.fail_wheel_position(false);
    const int before = wheel.fake.count(":WP#");
    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.driver->get_position() == 3);
    CHECK(wheel.fake.count(":WP#") == before + 1);
}

// Falsified by: ioptron_filterwheel_driver.cpp caching a -1 frame (the second poll is served -1 from the
// cache: one :WP# for two polls), or mapping -1 to a slot (Position reads 0, not the ASCOM moving sentinel).
TEST_CASE("iOptron iEFW Filter Wheel Driver - Position reads -1 while moving and the move end is seen at once",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_moving_polls(2);
    wheel.driver->set_position(4);
    const int before = wheel.fake.count(":WP#");

    CHECK(wheel.driver->get_position() == -1);
    CHECK(wheel.driver->get_position() == -1);
    CHECK(wheel.driver->get_position() == 4);  // no sleep: a moving frame is never held
    CHECK(wheel.fake.count(":WP#") == before + 3);
}

// Decision 0016: a member that needs no device access never waits behind a Position refill.
// Falsified by: ioptron_filterwheel_driver.cpp get_position() holding mutex_ across the :WP# exchange
// (get_names() waits the whole 400 ms hold).
TEST_CASE("iOptron iEFW Filter Wheel Driver - get_names does not wait behind a slow Position refill",
          "[ioptron][filterwheel][unit][position-cache][timing]") {
    ConnectedIefw wheel;
    wheel.fake.hold_next_reply(400ms);
    const int before = wheel.fake.count(":WP#");
    auto refill = std::async(std::launch::async, [&] { return wheel.driver->get_position(); });
    for (int i = 0; i < 500 && wheel.fake.count(":WP#") == before; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(wheel.fake.count(":WP#") == before + 1);

    const auto start = std::chrono::steady_clock::now();
    CHECK(wheel.driver->get_names().size() == 5);
    CHECK(wheel.driver->get_name() == "iOptron iEFW-15");
    CHECK(std::chrono::steady_clock::now() - start < 150ms);
    refill.get();
}

// Decision 0016 rules 1 and 4: the connect handshake runs off the state lock.
// Falsified by: ioptron_filterwheel_driver.cpp set_connected(true) holding mutex_ across the handshake
// (get_names() and get_name() wait the whole 400 ms hold).
TEST_CASE("iOptron iEFW Filter Wheel Driver - get_names does not wait behind a held connect",
          "[ioptron][filterwheel][unit][position-cache][timing]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(0, fake.slave_path());
    fake.hold_next_reply(400ms);
    auto connect = std::async(std::launch::async, [&] { driver->set_connected(true); });
    for (int i = 0; i < 500 && fake.connects() == 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(fake.connects() == 1);  // :DeviceInfo# is on the wire and its reply is held

    const auto start = std::chrono::steady_clock::now();
    CHECK_NOTHROW(driver->get_names());
    CHECK_NOTHROW(driver->get_name());
    CHECK(std::chrono::steady_clock::now() - start < 150ms);
    connect.get();
    driver->set_connected(false);
}

// Two writers race: the latch (target_/moving_), :WM and the restore on a throw are one step under
// io_mutex_. Without it a PUT can leave target_ on a slot the wheel did not reach, and Position reads -1 until
// the next move. Probabilistic: it guards the conversion, and a lock split is not caught reliably.
// Falsified by: ioptron_filterwheel_driver.cpp set_position() setting target_ and calling :WM without one lock
// held across both (after the writers finish, Position stays -1 although the wheel settled on a slot).
TEST_CASE("iOptron iEFW Filter Wheel Driver - concurrent set_position calls leave Position on the wheel slot",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_moving_polls(1);
    auto writer = [&](int first) {
        for (int i = 0; i < 20; ++i) {
            try {
                wheel.driver->set_position((first + i) % 5);
            } catch (const alpacacore::AlpacaException&) {
            }
        }
    };
    auto a = std::async(std::launch::async, writer, 1);
    auto b = std::async(std::launch::async, writer, 3);
    a.get();
    b.get();

    int position = -1;
    for (int i = 0; i < 20 && position < 0; ++i) {
        position = wheel.driver->get_position();
        std::this_thread::sleep_for(kPastTtl);
    }
    CHECK(position == wheel.fake.wheel_slot());
}

// A FocusOffsets write that lands while the connect reads the wheel's stored offsets is kept, not replaced by them.
// Falsified by: ioptron_filterwheel_driver.cpp publish_connect_state() overwriting focus_offsets_ with the stored
// offsets without re-checking under mutex_ that they are still empty (Offsets read back as zeros).
TEST_CASE("iOptron iEFW Filter Wheel Driver - a FocusOffsets write during connect is kept",
          "[ioptron][filterwheel][unit][position-cache][timing]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(0, fake.slave_path());
    auto connect = std::async(std::launch::async, [&] { driver->set_connected(true); });
    for (int i = 0; i < 500 && fake.count(":FW1#") == 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(fake.count(":FW1#") == 1);
    fake.hold_next_reply(400ms);  // holds the first :WF reply
    for (int i = 0; i < 500 && fake.count(":WF00#") == 0; ++i) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(fake.count(":WF00#") == 1);
    const std::vector<int> written{11, 22, 33, 44, 55};
    driver->set_focus_offsets(written);
    connect.get();
    CHECK(driver->get_focus_offsets() == written);
    driver->set_connected(false);
}

// Since #1723 the unplugged :WM write also loses the link, so the read after it is refused twice over: by the
// connection check before the cache, and by the invalidate in set_position()'s outer catch. The invalidate alone
// now guards a write that fails with the link up (ETIMEDOUT on a stalled adapter), which the pty fake cannot produce.
// Falsified by: ioptron_filterwheel_driver.cpp set_position() without status_cache_.invalidate() in the outer catch
// together with ensure_connected() without its link_alive() check (the read after the refused move is served from
// the frame cached before it and returns 3); either one alone is not caught.
TEST_CASE("iOptron iEFW Filter Wheel Driver - a refused move invalidates the cached Position",
          "[ioptron][filterwheel][unit][position-cache]") {
    ConnectedIefw wheel;
    wheel.fake.set_wheel_slot(3);
    REQUIRE(wheel.driver->get_position() == 3);

    wheel.fake.unplug();  // the :WM write fails
    CHECK_THROWS_AS(wheel.driver->set_position(4), alpacacore::AlpacaException);
    require_alpaca_error([&] { (void)wheel.driver->get_position(); },
                         static_cast<int>(alpacacore::AlpacaError::NotConnected));  // not the cached frame
}

// A wheel that drops off the bus between the command and its reply: the wrapper reports the hangup and the
// errno at once, not a read timeout after serial_timeout_s.
// Falsified by: ioptron_iefw_protocol_wrapper.cpp read_response() sending ReadOutcome::LinkDead to the timeout
// branch (the message reads "iEFW read timeout" and the call takes the full 4 s).
TEST_CASE("iOptron iEFW protocol wrapper - a hangup before the reply throws serial link lost with the errno",
          "[ioptron][filterwheel][unit][link-dead]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    alpacacore::vendor::ioptron::IefwProtocolWrapper wrapper;
    alpacacore::vendor::ioptron::IefwConnectionConfig config;
    config.serial_port = fake.slave_path();
    wrapper.connect(config);

    fake.hold_next_reply(300ms);  // the fake sits on the :WP# reply while the cable is pulled
    std::thread pull([&] {
        std::this_thread::sleep_for(60ms);
        fake.unplug();
    });
    const auto start = std::chrono::steady_clock::now();
    std::string what;
    try {
        wrapper.get_position();
        FAIL("a hung-up port must not return a slot");
    } catch (const alpacacore::AlpacaException& ex) {
        what = ex.what();
    }
    pull.join();
    CHECK(what == "iEFW serial link lost (errno 5)");
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    try {
        wrapper.disconnect();
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Decision 0009 rule 2 (issue #1723): a pulled cable loses the link at once.
// Connected reads false, every member throws NotConnected, DeviceState is
// empty, the port is released, get_link_fault() keeps the reason until the
// next Connect or Disconnect, and only a client Connect reopens it. A wheel
// that stops answering with the cable in only faults (rule 1).
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

// The AlpacaException text fn threw, or empty.
std::string alpaca_what_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.what();
    }
    return {};
}

constexpr int kNotConnected = static_cast<int>(alpacacore::AlpacaError::NotConnected);
const std::string kLostReason = "iEFW serial link lost (errno 5)";
const std::string kLostCall = "Filter wheel link lost; Connect again once the wheel is back";

std::unique_ptr<alpacacore::FilterWheelDriver> make_replug_wheel(const ReplugPort& port) {
    return alpacacore::vendor::ioptron::create_iefw_filterwheel(0, port.path.string());
}

// What a lost link refuses; old_pts is the node the driver had open.
void require_lost(alpacacore::FilterWheelDriver& driver, const std::string& old_pts) {
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_device_state().empty());
    CHECK_FALSE(driver.get_device_firmware().has_value());
    require_alpaca_error([&] { (void)driver.get_position(); }, kNotConnected);
    require_alpaca_error([&] { driver.set_position(1); }, kNotConnected);
    // The wrapper released the port: no claim is left on the dead node.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(old_pts));
}

}  // namespace

// Falsified by: ioptron_iefw_protocol_wrapper.cpp:486 read_response() throwing on ReadOutcome::LinkDead without
// fail_link_locked() (the call throws DriverException, Connected stays true, the port stays claimed);
// ioptron_filterwheel_driver.cpp:100 get_connected() without link_alive() (Connected reads true, DeviceState
// is not empty); :89 get_device_firmware() without link_alive() (the firmware is still shown); :133 the connect
// check reading the raw flag (the Connect over the lost link returns at once and Connected stays false).
TEST_CASE("iEFW Link - a cable pulled during a Position read loses the link at once and Connect reopens",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    alpacacore::test::FakeIoptronIeaf replugged;
    replugged.set_model(99);
    replugged.set_wheel_slot(2);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_device_firmware().has_value());
    const std::string old_pts = fake.slave_path();
    REQUIRE(alpacacore::util::is_serial_port_in_use(old_pts));

    const int polls = fake.count(":WP#");
    fake.hold_next_reply(milliseconds(300));  // the wheel sits on the :WP# reply while the cable is pulled
    std::atomic<int> code{0};
    std::thread caller([&] { code.store(alpaca_code_of([&] { (void)driver->get_position(); })); });
    REQUIRE(wait_until([&] { return fake.count(":WP#") == polls + 1; }, milliseconds(2000)));
    const auto pulled = std::chrono::steady_clock::now();
    fake.unplug();
    caller.join();
    CHECK(std::chrono::steady_clock::now() - pulled < std::chrono::seconds(2));  // not the 4 s read timeout
    CHECK(code.load() == kNotConnected);
    require_lost(*driver, old_pts);
    // Decision 0009 rule 5: the loss reason is kept while the link stays lost.
    CHECK(driver->get_link_fault() == kLostReason);

    // Nothing reopens it on its own; a client Connect over the lost link does.
    port.replug(replugged);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK(driver->get_device_firmware().has_value());
    CHECK(driver->get_position() == 2);
    CHECK(replugged.connects() == 1);
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

// Falsified by: ioptron_iefw_protocol_wrapper.cpp:529 send_command_ack_locked() without the LinkDead branch (the
// hangup is taken for a silent firmware: set_position() returns and Connected stays true).
TEST_CASE("iEFW Link - a cable pulled before a move's ack loses the link at once",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = fake.slave_path();

    // The ack is optional (a silent firmware only costs the ack wait), but a hangup there is not silence.
    fake.hold_reply_to(":WM", milliseconds(300));
    std::atomic<int> code{0};
    std::thread caller([&] { code.store(alpaca_code_of([&] { driver->set_position(3); })); });
    REQUIRE(wait_until([&] { return fake.count(":WM03#") == 1; }, milliseconds(2000)));
    fake.unplug();
    caller.join();
    CHECK(code.load() == kNotConnected);
    require_lost(*driver, old_pts);
    CHECK(driver->get_link_fault() == kLostReason);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

// Falsified by: ioptron_iefw_protocol_wrapper.cpp:461 write_data() without the EIO/ENXIO/ENODEV branch (the move
// throws DriverException "Write failed", Connected stays true); ioptron_filterwheel_driver.cpp:133 the disconnect
// check reading the link-aware value (the Disconnect returns at once and the reason is kept);
// ioptron_iefw_protocol_wrapper.cpp:318 disconnect() without set_fault("") (the reason outlives the Disconnect).
TEST_CASE("iEFW Link - a write to a pulled wheel loses the link at once and Disconnect clears the reason",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const std::string old_pts = fake.slave_path();

    fake.unplug();  // idle: the next command's write is what meets the dead port
    require_alpaca_error([&] { driver->set_position(2); }, kNotConnected);
    require_lost(*driver, old_pts);
    CHECK(driver->get_link_fault() == kLostReason);
    REQUIRE_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());  // a Disconnect clears the reason
}

// The status cache counts the read that lost the link as a failed read; after two earlier failures that one latches
// its "communications compromised" DriverException. A lost link is NotConnected all the same, and later calls are
// refused before the cache is read.
// Falsified by: ioptron_filterwheel_driver.cpp:218 get_position() without the ensure_connected() in its catch
// (the losing read throws DriverException "communications compromised"); :340 ensure_connected() without the
// link_alive() check (the losing read throws DriverException, the later calls "iEFW not connected" from the wrapper).
TEST_CASE("iEFW Link - a loss after failed reads is NotConnected and later calls are refused before the cache",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    fake.fail_wheel_position(true);  // the cable is in, the replies do not parse: two failed reads, below the latch
    for (int i = 0; i < 2; ++i) {
        CHECK(alpaca_code_of([&] { (void)driver->get_position(); }) ==
              static_cast<int>(alpacacore::AlpacaError::DriverException));
    }
    REQUIRE(driver->get_connected());

    fake.unplug();  // the third read's write meets the dead port
    require_alpaca_error([&] { (void)driver->get_position(); }, kNotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault() == kLostReason);  // the loss reason, not the cache's fault text
    for (int i = 0; i < 3; ++i) {
        CHECK(alpaca_what_of([&] { (void)driver->get_position(); }) == kLostCall);
    }
    CHECK(alpaca_what_of([&] { driver->set_position(1); }) == kLostCall);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

// A loss mid-move: the move latch (Position -1) from before the loss is not carried into the reconnected session.
// Falsified by: ioptron_filterwheel_driver.cpp:141 the connect without teardown_locked() (Connected reads true
// while the reconnect is still reading the wheel's firmware: the raw flag from before the loss and the fresh link).
// Falsified by: ioptron_filterwheel_driver.cpp:441 publish_connect_state() without `moving_ = false;` (the reconnected
// wheel's Position reads -1, the stale move latch, instead of its slot 2).
TEST_CASE("iEFW Link - a Connect over a lost link mid-move reopens without a Disconnect and drops the move latch",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    fake.set_wheel_moving_polls(100);  // the wheel stays in motion until the cable goes
    alpacacore::test::FakeIoptronIeaf replugged;
    replugged.set_model(99);
    replugged.set_wheel_slot(2);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    driver->set_connected(true);
    driver->set_position(4);
    REQUIRE(driver->get_position() == -1);

    fake.unplug();
    require_alpaca_error([&] { (void)driver->get_position(); }, kNotConnected);
    REQUIRE_FALSE(driver->get_connected());

    // No Disconnect: a Connect while the raw flag is still set must not be taken as already connected.
    port.replug(replugged);
    replugged.hold_reply_to(":FW1#", milliseconds(300));
    std::atomic<int> code{-2};
    std::thread connecting([&] { code.store(alpaca_code_of([&] { driver->set_connected(true); })); });
    REQUIRE(wait_until([&] { return replugged.count(":FW1#") == 1; }, milliseconds(3000)));
    CHECK_FALSE(driver->get_connected());  // the handshake passed, the reconnect has not finished
    connecting.join();
    CHECK(code.load() == 0);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_position() == 2);  // the replugged wheel's slot, not the -1 of the move before the loss
    CHECK(replugged.count(":WP#") == 1);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

// Falsified by: ioptron_iefw_protocol_wrapper.cpp:439 fail_link_locked() without its `if (connected_)` guard (the
// handshake's hangup keeps a loss reason after the failed Connect).
TEST_CASE("iEFW Link - a cable pulled during the connect handshake fails the Connect and latches nothing",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    alpacacore::test::FakeIoptronIeaf replugged;
    replugged.set_model(99);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
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
    CHECK(driver->get_position() == 0);
    REQUIRE_NOTHROW(driver->set_connected(false));
}

// The firmware and stored-offset reads after the handshake log a failure and carry on; a hangup there is a loss.
// Falsified by: ioptron_filterwheel_driver.cpp:426 publish_connect_state() without the link_alive() check (the
// Connect succeeds onto a released port: no error, Connected false, the loss reason kept).
TEST_CASE("iEFW Link - a cable pulled while the connect reads the firmware fails the Connect and latches nothing",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    const std::string old_pts = fake.slave_path();

    fake.hold_reply_to(":FW1#", milliseconds(300));
    std::atomic<int> code{-2};
    std::thread connecting([&] { code.store(alpaca_code_of([&] { driver->set_connected(true); })); });
    REQUIRE(wait_until([&] { return fake.count(":FW1#") == 1; }, milliseconds(3000)));
    fake.unplug();
    connecting.join();
    CHECK(code.load() == kNotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(old_pts));
}

// Decision 0009 rule 1: a wheel that stops answering with the cable in is not a lost link. Three failed Position
// reads latch the status cache's fault: DriverException, Connected stays true, get_link_fault() gives the fault
// text, and a good reply clears it. The third read is a real 4 s read timeout (the wrapper's serial timeout).
// Falsified by: ioptron_filterwheel_driver.cpp:110 get_link_fault() without the status cache's fault() (it
// reads empty while the fault is latched).
TEST_CASE("iEFW Link - a silent wheel with the cable in latches a fault and stays Connected",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    fake.set_wheel_slot(3);
    ReplugPort port(fake);
    auto driver = make_replug_wheel(port);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    fake.fail_wheel_position(true);
    for (int i = 0; i < 2; ++i) {
        (void)alpaca_code_of([&] { (void)driver->get_position(); });
    }
    fake.fail_wheel_position(false);
    fake.set_silent(true);
    try {
        (void)driver->get_position();
        FAIL("a silent wheel must not return a slot");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().find("iEFW read timeout") != std::string::npos);
    CHECK(driver->get_device_firmware().has_value());

    fake.set_silent(false);
    CHECK(driver->get_position() == 3);  // the next good reply clears the fault, no reconnect
    CHECK(driver->get_link_fault().empty());
    REQUIRE_NOTHROW(driver->set_connected(false));
}

// Falsified by: ioptron_iefw_protocol_wrapper.cpp:307 connect() without link_lost_.store(false) (link_alive()
// stays false after the reconnect); :308 connect() without set_fault("") (the old reason outlives it).
TEST_CASE("iEFW Link - the wrapper keeps the loss reason until the next connect or disconnect",
          "[ioptron][filterwheel][unit][fake][link-lost]") {
    alpacacore::test::FakeIoptronIeaf fake;
    fake.set_model(99);
    alpacacore::test::FakeIoptronIeaf replugged;
    replugged.set_model(99);
    alpacacore::vendor::ioptron::IefwProtocolWrapper wrapper;
    alpacacore::vendor::ioptron::IefwConnectionConfig config;
    config.serial_port = fake.slave_path();
    wrapper.connect(config);
    REQUIRE(wrapper.link_alive());
    REQUIRE(wrapper.link_fault().empty());

    fake.unplug();
    require_alpaca_error([&] { wrapper.move_to(1); }, kNotConnected);
    CHECK_FALSE(wrapper.link_alive());
    CHECK_FALSE(wrapper.is_connected());
    CHECK(wrapper.link_fault() == kLostReason);

    config.serial_port = replugged.slave_path();
    wrapper.connect(config);  // a successful connect clears both
    CHECK(wrapper.link_alive());
    CHECK(wrapper.link_fault().empty());

    replugged.unplug();
    require_alpaca_error([&] { (void)wrapper.get_position(); }, kNotConnected);
    REQUIRE(wrapper.link_fault() == kLostReason);
    wrapper.disconnect();  // so does a disconnect
    CHECK(wrapper.link_fault().empty());
}

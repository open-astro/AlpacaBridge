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

// The remotealpaca Switch proxied device (decision 0023, issue #1753 slice 5, #2709) over the scripted FakeHttpClient
// and a scripted upstream (alpaca_upstream_script.h), with the poll on the fake task clock (decision 0005). Fake
// evidence only: no socket, no network. Cases 3 and 4 (and part of 6) of the required set come from the driver's
// contract_sweep.h entry. The rules the Switch shares with the other proxied devices (the snapshot, the loss bound, the
// self-loop check) live in UpstreamProxy (upstream_proxy.h) and are covered at length by
// test_remotealpaca_observingconditions.cpp; the cases here pin what the Switch's own members do with each of them.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_switch_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "alpaca_upstream_script.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_http_client.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace remote = alpacacore::vendor::remotealpaca;
using alpacacore::SwitchDriver;
using alpacacore::test::alpaca_error_message;
using alpacacore::test::AlpacaUpstreamScript;
using alpacacore::test::require_alpaca_error;

constexpr std::chrono::milliseconds kRendezvous{5000};
constexpr int kUpstreamPort = 11111;
const std::string kPrefix = "127.0.0.1:11111: ";

// The fake clock, the scripted upstream behind the fake HTTP client, and the driver, destroyed in reverse.
struct Rig {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeHttpClient http;
    AlpacaUpstreamScript upstream{AlpacaUpstreamScript::Kind::Switch};
    std::unique_ptr<SwitchDriver> driver;

    explicit Rig(std::chrono::seconds poll = std::chrono::seconds(1), remote::ServerAddressSource self = {},
                 int device_number = 0) {
        upstream.install(http);
        driver = remote::create_remotealpaca_switch(device_number, config(poll), http.client(), std::move(self), clock);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() { driver.reset(); }

    static remote::RemoteDeviceConfig config(std::chrono::seconds poll) {
        remote::RemoteDeviceConfig config;
        config.host = "127.0.0.1";
        config.port = kUpstreamPort;
        config.upstream_device_number = 0;
        config.poll_period = poll;
        return config;
    }

    // Connect, then wait for the poll to park on the clock.
    void connect() {
        REQUIRE(alpacacore::test::connect_and_park(*driver, clock, kRendezvous));
        REQUIRE(driver->get_connected());
    }

    // One poll period on the clock; returns when the woken poll has parked again or ended.
    void poll(std::chrono::nanoseconds by = std::chrono::seconds(1)) {
        clock.advance(by);
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    }

    // Three failed polls in a row latch the fault (decision 0009 rule 1); the upstream answers again after them.
    void fault() {
        http.fail_next(3);
        poll();
        poll();
        poll();
        REQUIRE(driver->get_connected());
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }
};

// Counts the WARN lines the remotealpaca component logs about a slow poll, and keeps the last one; the previous sink is
// restored on destruction. The sink runs on the poll thread.
class SlowPollWarns {
public:
    SlowPollWarns() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level != alpacacore::logging::LogLevel::Warn || component != "remotealpaca" ||
                    message.find("poll period") == std::string_view::npos) {
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                ++count_;
                last_ = std::string(message);
            });
    }
    SlowPollWarns(const SlowPollWarns&) = delete;
    SlowPollWarns& operator=(const SlowPollWarns&) = delete;
    ~SlowPollWarns() { alpacacore::logging::set_log_sink(previous_); }

    int count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }
    std::string last() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_;
    }

private:
    alpacacore::logging::LogSink previous_ = alpacacore::logging::get_log_sink();
    mutable std::mutex mutex_;
    int count_ = 0;
    std::string last_;
};

}  // namespace

TEST_CASE("Remote Alpaca Switch Driver - Defaults", "[remotealpaca][switch][unit]") {
    Rig rig;
    REQUIRE(rig.driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(rig.driver->get_device_number() == 0);
    REQUIRE_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_connecting());
    CHECK(rig.driver->get_name() == "Remote Alpaca Switch");
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_device_state().empty());
    // MaxSwitch and every per-switch capability are the upstream's, read at Connect, so each is NotConnected.
    require_alpaca_error([&] { (void)rig.driver->get_max_switch(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver->get_can_write(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver->get_can_async(0); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);  // building the driver touches no network
}

TEST_CASE("Remote Alpaca Switch Driver - Device metadata", "[remotealpaca][switch][unit]") {
    Rig rig(std::chrono::seconds(1), {}, 3);
    CHECK(rig.driver->get_device_number() == 3);
    CHECK(rig.driver->get_description() == "Switch of the upstream Alpaca server 127.0.0.1:11111 device 0");
    CHECK(rig.driver->get_driver_info() == "AlpacaCore Remote Alpaca Switch Driver");
    CHECK(rig.driver->get_driver_version() == alpacacore::kVersion);
    CHECK(rig.driver->get_interface_version() == 3);
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_SW_3");
    // Decision 0023 rule 5: the UniqueID is this server's own, never the upstream's, connected or not.
    rig.upstream.set("uniqueid", "\"upstream-unique-id-0000\"");
    rig.connect();
    CHECK(rig.driver->get_unique_id() == "REMOTEALPACA_SW_3");
    CHECK(rig.upstream.calls("uniqueid") == 0);
    // Once a Connect read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    CHECK(rig.driver->get_name() == "Remote Alpaca Alpaca Simulator Switch");
    CHECK(rig.driver->get_description() == "ASCOM Alpaca Simulator Switch");
}

TEST_CASE("Remote Alpaca Switch Driver - the getters serve the snapshot and the Connect's metadata, calling nothing",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    auto& s = *rig.driver;
    const int calls_after_connect = rig.http.calls();
    CHECK(s.get_max_switch() == 3);
    CHECK(s.get_switch_name(1) == "Dew Heater");
    CHECK(s.get_switch_description(1) == "Dew Heater switch");
    CHECK(s.get_min_switch_value(2) == 0.0);
    CHECK(s.get_max_switch_value(2) == 10.0);
    CHECK(s.get_switch_step(2) == 0.5);
    CHECK(s.get_can_write(0));
    CHECK_FALSE(s.get_can_write(2));
    CHECK_FALSE(s.get_can_async(0));
    CHECK(s.get_can_async(1));
    CHECK(s.get_switch(0));
    CHECK(s.get_switch_value(1) == 40.0);
    CHECK(s.get_switch_value(2) == 2.5);
    CHECK(s.get_state_change_complete(0));
    CHECK(s.get_state_change_complete(1));
    const auto state = s.get_device_state();
    REQUIRE(state.size() == 10);  // GetSwitchN, GetSwitchValueN, StateChangeCompleteN for three ids, and TimeStamp
    CHECK(state[3].name == "GetSwitch1");
    CHECK(std::get<bool>(state[3].value));
    CHECK(state[4].name == "GetSwitchValue1");
    CHECK(std::get<double>(state[4].value) == 40.0);
    CHECK(state.back().name == "TimeStamp");
    CHECK(rig.http.calls() == calls_after_connect);

    // The metadata is read once at Connect; the poll measures GetSwitch and GetSwitchValue of every switch, and
    // StateChangeComplete of the asynchronous one only.
    CHECK(rig.upstream.calls("getswitchname") == 3);
    const int polled_complete = rig.upstream.calls("statechangecomplete");
    rig.upstream.set_for("getswitchvalue", "2", "7.5");
    rig.upstream.set_for("getswitchname", "2", "\"Renamed\"");
    CHECK(s.get_switch_value(2) == 2.5);  // until the next poll the frame holds
    rig.poll();
    CHECK(s.get_switch_value(2) == 7.5);
    CHECK(s.get_switch_name(2) == "Sensor");
    CHECK(rig.upstream.calls("getswitchname") == 3);
    CHECK(rig.upstream.calls("statechangecomplete") == polled_complete + 1);
}

// DeviceState's TimeStamp is when the frame was measured (decision 0010), through the switch base's
// device_state_age(): a frame the poll stopped refreshing carries its age.
TEST_CASE("Remote Alpaca Switch Driver - DeviceState is stamped with the frame's age", "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    remote::set_poll_paused_for_testing(*rig.driver, true);
    rig.poll(std::chrono::seconds(20));
    const auto stamp = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*rig.driver));
    const auto age = std::chrono::system_clock::now() - stamp;
    CHECK(age >= std::chrono::seconds(19));
    CHECK(age <= std::chrono::seconds(25));
    remote::set_poll_paused_for_testing(*rig.driver, false);
}

TEST_CASE("Remote Alpaca Switch Driver - SetSwitch and SetSwitchValue are forwarded and written through",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    auto& s = *rig.driver;
    s.set_switch(0, false);
    CHECK(rig.upstream.calls("setswitch") == 1);
    // Written through: GetSwitch and GetSwitchValue (the range's minimum) read the write before any poll.
    CHECK_FALSE(s.get_switch(0));
    CHECK(s.get_switch_value(0) == 0.0);
    s.set_switch_value(1, 75.0);
    CHECK(rig.upstream.calls("setswitchvalue") == 1);
    CHECK(s.get_switch_value(1) == 75.0);
    CHECK(s.get_switch(1));
    s.set_switch_value(1, 0.0);
    CHECK_FALSE(s.get_switch(1));  // at the minimum
    // The upstream holds them: the next poll measures the same.
    rig.poll();
    CHECK_FALSE(s.get_switch(0));
    CHECK(s.get_switch_value(1) == 0.0);
    s.set_switch(0, true);
    CHECK(s.get_switch_value(0) == 1.0);  // the range's maximum
    rig.poll();
    CHECK(s.get_switch(0));
}

// A device takes a value between steps at the nearest step from the minimum, within its range: the write-through reads
// what the device will, not the value as sent.
TEST_CASE("Remote Alpaca Switch Driver - SetSwitchValue writes the value through on the switch's step",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.upstream.set_switch(1, "Dew Heater", 0.0, 50.0, 30.0, true, true,
                            0.0);  // steps 0 and 30; 50 is the range's end
    rig.connect();
    auto& s = *rig.driver;
    s.set_switch_value(1, 20.0);
    CHECK(s.get_switch_value(1) == 30.0);  // the nearer step
    CHECK(s.get_switch(1));
    s.set_switch_value(1, 14.0);
    CHECK(s.get_switch_value(1) == 0.0);
    CHECK_FALSE(s.get_switch(1));
    s.set_switch_value(1, 50.0);
    CHECK(s.get_switch_value(1) == 50.0);  // the next step (60) is past the range: the range's end
    // The upstream got each value as sent.
    CHECK(rig.upstream.calls("setswitchvalue") == 3);
    // A switch on a step of 1: an off-step value reads the step.
    s.set_switch_value(0, 0.4);
    CHECK(s.get_switch_value(0) == 0.0);
}

TEST_CASE("Remote Alpaca Switch Driver - Value range validation", "[remotealpaca][switch][unit]") {
    SECTION("a negative id and a non-finite value are InvalidValue before the connection check") {
        Rig rig;
        require_alpaca_error([&] { (void)rig.driver->get_switch(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { (void)rig.driver->get_switch_name(-1); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_switch_value(-1, 0.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_switch_value(0, std::numeric_limits<double>::quiet_NaN()); },
                             AlpacaError::InvalidValue);
        require_alpaca_error([&] { rig.driver->set_async_value(0, std::numeric_limits<double>::infinity()); },
                             AlpacaError::InvalidValue);
        // An id the upstream may have is NotConnected: MaxSwitch is not known until a Connect read it.
        require_alpaca_error([&] { (void)rig.driver->get_switch(99); }, AlpacaError::NotConnected);
        CHECK(rig.http.calls() == 0);
    }
    SECTION("connected: an id at or past MaxSwitch and a value outside the switch's range send nothing") {
        Rig rig;
        rig.connect();
        auto& s = *rig.driver;
        for (const int id : {3, 99}) {
            require_alpaca_error([&] { (void)s.get_switch(id); }, AlpacaError::InvalidValue);
            require_alpaca_error([&] { (void)s.get_switch_value(id); }, AlpacaError::InvalidValue);
            require_alpaca_error([&] { (void)s.get_state_change_complete(id); }, AlpacaError::InvalidValue);
            require_alpaca_error([&] { (void)s.get_can_write(id); }, AlpacaError::InvalidValue);
            require_alpaca_error([&] { (void)s.get_max_switch_value(id); }, AlpacaError::InvalidValue);
            require_alpaca_error([&] { s.set_switch(id, true); }, AlpacaError::InvalidValue);
        }
        require_alpaca_error([&] { s.set_switch_value(1, 100.5); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { s.set_switch_value(1, -0.5); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { s.set_async_value(1, 101.0); }, AlpacaError::InvalidValue);
        CHECK(rig.upstream.calls("setswitch") == 0);
        CHECK(rig.upstream.calls("setswitchvalue") == 0);
        CHECK(rig.upstream.calls("setasyncvalue") == 0);
        s.set_switch_value(1, 100.0);  // the range's end is in range
        CHECK(rig.upstream.calls("setswitchvalue") == 1);
    }
    SECTION("the factory refuses each config field out of range before any network call") {
        alpacacore::test::FakeTaskClock clock;
        alpacacore::test::FakeHttpClient http;
        for (const auto bad : {std::chrono::seconds(0), std::chrono::seconds(61)}) {
            require_alpaca_error(
                [&] { (void)remote::create_remotealpaca_switch(0, Rig::config(bad), http.client(), {}, clock); },
                AlpacaError::InvalidValue);
        }
        for (const int port : {0, 65536}) {
            remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
            config.port = port;
            require_alpaca_error([&] { (void)remote::create_remotealpaca_switch(0, config, http.client(), {}, clock); },
                                 AlpacaError::InvalidValue);
        }
        remote::RemoteDeviceConfig config = Rig::config(std::chrono::seconds(2));
        config.upstream_device_number = -1;
        require_alpaca_error([&] { (void)remote::create_remotealpaca_switch(0, config, http.client(), {}, clock); },
                             AlpacaError::InvalidValue);
        CHECK(http.calls() == 0);
    }
}

TEST_CASE("Remote Alpaca Switch Driver - SetAsync reads StateChangeComplete false until the upstream finishes",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    auto& s = *rig.driver;
    s.set_async_value(1, 80.0);
    CHECK(rig.upstream.calls("setasyncvalue") == 1);
    // Written through: false at once; the value is the upstream's, measured by the poll.
    CHECK_FALSE(s.get_state_change_complete(1));
    CHECK(s.get_switch_value(1) == 40.0);
    rig.poll();
    CHECK_FALSE(s.get_state_change_complete(1));  // the upstream's change still runs
    rig.upstream.finish_async();
    rig.poll();
    CHECK(s.get_state_change_complete(1));
    CHECK(s.get_switch_value(1) == 80.0);
    s.set_async(1, false);
    CHECK(rig.upstream.calls("setasync") == 1);
    CHECK_FALSE(s.get_state_change_complete(1));
    rig.upstream.finish_async();
    rig.poll();
    CHECK(s.get_state_change_complete(1));
    CHECK_FALSE(s.get_switch(1));
    // A switch that is not asynchronous: the upstream refuses with its own error, unchanged, and nothing is written.
    CHECK(alpaca_error_message([&] { s.set_async(0, false); }, AlpacaError::NotImplemented) ==
          kPrefix + "Switch 0 cannot be set asynchronously");
    CHECK(s.get_state_change_complete(0));
    CHECK(s.get_switch(0));
}

// Decision 0023: a member the upstream lacks is NotImplemented. An ISwitchV2 upstream has no Connect and no
// asynchronous members: it gets Connected=true, CanAsync reads false without asking it, every change is complete, and
// SetAsync and SetAsyncValue send nothing.
TEST_CASE("Remote Alpaca Switch Driver - an ISwitchV2 upstream has no asynchronous members",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.upstream.set("interfaceversion", "2");
    rig.connect();
    auto& s = *rig.driver;
    CHECK(rig.upstream.calls("connect") == 0);
    CHECK(rig.upstream.upstream_connected());
    CHECK(rig.upstream.calls("canasync") == 0);
    CHECK_FALSE(s.get_can_async(1));
    CHECK(s.get_state_change_complete(1));
    CHECK(rig.upstream.calls("statechangecomplete") == 0);
    require_alpaca_error([&] { s.set_async(1, true); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { s.set_async_value(1, 10.0); }, AlpacaError::MethodNotImplemented);
    CHECK(rig.upstream.calls("setasync") == 0);
    CHECK(rig.upstream.calls("setasyncvalue") == 0);
    // The synchronous writes are ISwitchV2's own: forwarded.
    s.set_switch_value(1, 10.0);
    CHECK(s.get_switch_value(1) == 10.0);
    rig.driver->set_connected(false);
    CHECK(rig.upstream.calls("disconnect") == 0);
    CHECK_FALSE(rig.upstream.upstream_connected());
}

TEST_CASE("Remote Alpaca Switch Driver - Connect and Disconnect are forwarded to an ISwitchV3 upstream",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    CHECK(rig.upstream.calls("interfaceversion") == 1);
    CHECK(rig.upstream.calls("connect") == 1);
    CHECK(rig.upstream.calls("maxswitch") == 1);
    CHECK(rig.upstream.upstream_connected());
    rig.driver->set_connected(false);
    CHECK(rig.upstream.calls("disconnect") == 1);
    CHECK_FALSE(rig.upstream.upstream_connected());
}

TEST_CASE("Remote Alpaca Switch Driver - the writes are refused while the link is faulted, the metadata answers",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    auto& s = *rig.driver;
    rig.fault();
    // Faulted: the frame is refused with DriverException, the Connect's metadata still answers (static metadata).
    CHECK(alpaca_error_message([&] { (void)s.get_switch(0); }, AlpacaError::DriverException)
              .find("communications compromised") != std::string::npos);
    require_alpaca_error([&] { (void)s.get_switch_value(1); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)s.get_state_change_complete(0); }, AlpacaError::DriverException);
    CHECK(s.get_max_switch() == 3);
    CHECK(s.get_switch_name(1) == "Dew Heater");
    CHECK(s.get_can_write(0));
    // DeviceState omits every id while the frame is refused: empty.
    CHECK(s.get_device_state().empty());
    // Every write needs the frame: refused, and nothing sent. An argument error still comes first.
    require_alpaca_error([&] { s.set_switch(0, false); }, AlpacaError::DriverException);
    require_alpaca_error([&] { s.set_switch_value(1, 10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { s.set_async(1, true); }, AlpacaError::DriverException);
    require_alpaca_error([&] { s.set_async_value(1, 10.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { s.set_switch_name(1, "Heater"); }, AlpacaError::DriverException);
    require_alpaca_error([&] { s.set_switch_value(1, 500.0); }, AlpacaError::InvalidValue);
    CHECK(rig.upstream.calls("setswitch") == 0);
    CHECK(rig.upstream.calls("setswitchvalue") == 0);
    CHECK(rig.upstream.calls("setasync") == 0);
    CHECK(rig.upstream.calls("setasyncvalue") == 0);
    CHECK(rig.upstream.calls("setswitchname") == 0);
    // The next good poll clears the fault.
    rig.poll();
    CHECK(s.get_link_fault().empty());
    CHECK(s.get_switch(0));
}

// Commands reach the upstream in caller order (UpstreamProxy's command order): a write sent while an earlier one's body
// is held before its PUT waits for it, so the upstream ends with the later write. Without the order, the later write
// supersedes the held body and reaches the upstream first, and the held body's PUT then lands after it.
TEST_CASE("Remote Alpaca Switch Driver - two writes to one switch reach the upstream in caller order",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    alpacacore::test::Latch first_body;
    std::atomic<bool> held{false};  // only the first body holds
    remote::set_command_hook_for_testing(*rig.driver, [&] {
        if (!held.exchange(true)) first_body.block();
    });
    std::thread first([&] { rig.driver->set_switch(0, false); });
    REQUIRE(first_body.wait_entered());
    alpacacore::test::Latch second_waits;
    remote::set_command_waiting_hook_for_testing(*rig.driver, [&] { second_waits.block(); });
    std::thread second([&] { rig.driver->set_switch(0, true); });
    CHECK(second_waits.wait_entered());           // CHECK, not REQUIRE: the threads are joined below either way
    CHECK(rig.upstream.calls("setswitch") == 0);  // the first is held before its PUT, and the second waits for it
    second_waits.release();
    first_body.release();
    first.join();
    second.join();
    remote::set_command_hook_for_testing(*rig.driver, {});
    remote::set_command_waiting_hook_for_testing(*rig.driver, {});
    CHECK(rig.upstream.calls("setswitch") == 2);
    // The upstream took the later write last, and the write-through is what that write set.
    CHECK(rig.driver->get_switch(0));
    CHECK(rig.driver->get_switch_value(0) == 1.0);
    rig.poll();
    CHECK(rig.driver->get_switch(0));
    CHECK(rig.driver->get_switch_value(0) == 1.0);
}

TEST_CASE("Remote Alpaca Switch Driver - SetSwitchName is forwarded and written through",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    auto& s = *rig.driver;
    s.set_switch_name(1, "Heater A");
    CHECK(rig.upstream.calls("setswitchname") == 1);
    CHECK(s.get_switch_name(1) == "Heater A");
    // An upstream whose names are fixed refuses it with its own error, unchanged, and the name stays.
    rig.upstream.set_put_error("setswitchname", 0x400, "Switch names are fixed");
    CHECK(alpaca_error_message([&] { s.set_switch_name(1, "Heater B"); }, AlpacaError::NotImplemented) ==
          kPrefix + "Switch names are fixed");
    CHECK(s.get_switch_name(1) == "Heater A");
    // The next Connect reads the upstream's name again.
    s.set_connected(false);
    rig.connect();
    CHECK(s.get_switch_name(1) == "Heater A");
    CHECK(rig.upstream.calls("getswitchname") == 6);
}

TEST_CASE("Remote Alpaca Switch Driver - State machine contracts", "[remotealpaca][switch][unit]") {
    Rig rig;
    auto& s = *rig.driver;
    CHECK_FALSE(s.get_connected());
    CHECK(s.get_device_state().empty());
    require_alpaca_error([&] { (void)s.get_switch(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { s.set_switch(0, true); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { s.set_async(0, true); }, AlpacaError::NotConnected);
    CHECK(rig.http.calls() == 0);

    rig.connect();
    CHECK(s.get_state_change_complete(1));  // nothing runs after Connect
    rig.driver->set_connected(true);        // idempotent: no second forward
    CHECK(rig.upstream.calls("connect") == 1);

    // A refused write is a lost link at once (decision 0009 point 2(a)): NotConnected.
    rig.http.refuse_next(1);
    require_alpaca_error([&] { s.set_switch(0, false); }, AlpacaError::NotConnected);
    CHECK_FALSE(s.get_connected());
    CHECK(s.get_device_state().empty());
    require_alpaca_error([&] { (void)s.get_max_switch(); }, AlpacaError::NotConnected);
    CHECK(s.get_link_fault().find("connection refused") != std::string::npos);
    // Nothing relinks by itself (decision 0009 rule 4): the poll ended.
    const int calls = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(60));
    CHECK(rig.http.calls() == calls);
    // Only the client's Connect relinks.
    rig.connect();
    CHECK(s.get_link_fault().empty());
    CHECK(s.get_switch(0));

    // Disconnected by the client: NotConnected again, and the poll ended.
    s.set_connected(false);
    require_alpaca_error([&] { (void)s.get_switch_value(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)s.get_switch_name(0); }, AlpacaError::NotConnected);
    const int after = rig.http.calls();
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.http.calls() == after);
}

TEST_CASE("Remote Alpaca Switch Driver - members it does not proxy or the upstream lacks are not implemented",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    // An upstream with no step for switch 2 answers its own NotImplemented; the Connect stands.
    rig.upstream.set_error_for("switchstep", "2", 0x400, "SwitchStep is not implemented");
    rig.upstream.set_error_for("getswitchvalue", "2", 0x400, "GetSwitchValue is not implemented");
    rig.connect();
    auto& s = *rig.driver;
    CHECK(alpaca_error_message([&] { (void)s.get_switch_step(2); }, AlpacaError::NotImplemented) ==
          kPrefix + "SwitchStep is not implemented");
    CHECK(alpaca_error_message([&] { (void)s.get_switch_value(2); }, AlpacaError::NotImplemented) ==
          kPrefix + "GetSwitchValue is not implemented");
    // DeviceState omits the id whose member throws, and keeps the others.
    const auto state = s.get_device_state();
    REQUIRE(state.size() == 7);
    CHECK(state[0].name == "GetSwitch0");
    CHECK(state[5].name == "StateChangeComplete1");
    // Actions and Command* are not forwarded.
    CHECK(s.get_supported_actions().empty());
    CHECK_FALSE(s.can_action("anything"));
    require_alpaca_error([&] { (void)s.action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)s.command_blind("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)s.command_bool("x", false); }, AlpacaError::NotImplemented);
    require_alpaca_error([&] { (void)s.command_string("x", false); }, AlpacaError::NotImplemented);
    CHECK(rig.upstream.calls("action") == 0);
    CHECK(rig.upstream.calls("commandblind") == 0);
}

TEST_CASE("Remote Alpaca Switch Driver - an upstream write error passes through unchanged with its host",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    // Switch 2 is read-only: the upstream refuses the write, and its error is the answer.
    CHECK(alpaca_error_message([&] { rig.driver->set_switch_value(2, 5.0); }, AlpacaError::NotImplemented) ==
          kPrefix + "Switch 2 cannot be written");
    // A refused write wrote nothing through, and an answered error is not a transport failure: the link stays up.
    CHECK(rig.driver->get_switch_value(2) == 2.5);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("Remote Alpaca Switch Driver - Connect fails for a MaxSwitch it cannot serve",
          "[remotealpaca][switch][unit]") {
    SECTION("more switches than one proxied Switch polls") {
        Rig rig;
        rig.upstream.set("maxswitch", std::to_string(remote::kMaxProxiedSwitches + 1));
        CHECK(alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException)
                  .find("outside 0 to 64") != std::string::npos);
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.upstream.calls("getswitchname") == 0);
        CHECK_FALSE(rig.upstream.upstream_connected());  // the forwarded Connect was undone
    }
    SECTION("an upstream error on MaxSwitch is the Connect's answer, unchanged") {
        Rig rig;
        rig.upstream.set_error("maxswitch", 0x40B, "switch bank offline");
        CHECK(alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::InvalidOperation) ==
              kPrefix + "switch bank offline");
        CHECK_FALSE(rig.driver->get_connected());
        CHECK_FALSE(rig.upstream.upstream_connected());
    }
}

TEST_CASE("Remote Alpaca Switch Driver - Connect refuses an upstream that is this server",
          "[remotealpaca][switch][unit]") {
    // This server listens on every address (a wildcard bind) on the upstream's port: 127.0.0.1 is one of its own.
    Rig rig(std::chrono::seconds(1), [] { return std::vector<remote::ServerAddress>{{"0.0.0.0", kUpstreamPort}}; });
    const std::string text =
        alpaca_error_message([&] { rig.driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK(text.find("is this server") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.http.calls() == 0);
}

// Issue #2796: a two-state switch's GetSwitch is derived from its GetSwitchValue (ASCOM: on is MaxSwitchValue, off is
// MinSwitchValue), so the poll asks GetSwitch only of the other switches. At 64 switches, a quarter each two-state,
// multi-state, asynchronous multi-state and asynchronous two-state, one poll was 160 calls and is 128.
TEST_CASE("Remote Alpaca Switch Driver - one poll of 64 switches asks GetSwitch only where it cannot derive it",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.upstream.set("maxswitch", std::to_string(remote::kMaxProxiedSwitches));
    for (int id = 0; id < remote::kMaxProxiedSwitches; ++id) {
        const bool async = id % 4 >= 2;
        if (id % 2 == 0) {
            rig.upstream.set_switch(id, "Port", 0.0, 1.0, 1.0, true, async, id % 8 < 4 ? 1.0 : 0.0);
        } else {
            rig.upstream.set_switch(id, "Heater", 0.0, 100.0, 1.0, true, async, 40.0);
        }
    }
    rig.connect();
    const int calls = rig.http.calls();
    const int values = rig.upstream.calls("getswitchvalue");
    const int states = rig.upstream.calls("getswitch");
    const int completes = rig.upstream.calls("statechangecomplete");
    rig.poll();
    CHECK(rig.upstream.calls("getswitchvalue") - values == 64);
    CHECK(rig.upstream.calls("getswitch") - states == 32);  // the multi-state switches only
    CHECK(rig.upstream.calls("statechangecomplete") - completes == 32);
    CHECK(rig.http.calls() - calls == 128);
    // The derived answers are the upstream's.
    for (int id = 0; id < remote::kMaxProxiedSwitches; ++id) {
        CHECK(rig.driver->get_switch(id) == (id % 2 == 1 || id % 8 < 4));
    }
}

TEST_CASE(
    "Remote Alpaca Switch Driver - a two-state switch's GetSwitch follows its value through the poll, the writes "
    "and a fault",
    "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    auto& s = *rig.driver;
    const int asked = rig.upstream.calls("getswitch");
    // Switch 0 is two-state (0 to 1 in steps of 1). The poll reads its value only.
    rig.upstream.set_for("getswitchvalue", "0", AlpacaUpstreamScript::number(0.0));
    rig.upstream.set_for("getswitch", "0", "false");
    rig.poll();
    CHECK_FALSE(s.get_switch(0));
    CHECK(s.get_switch_value(0) == 0.0);
    // Written through, then polled: the two agree.
    s.set_switch(0, true);
    CHECK(s.get_switch(0));
    CHECK(s.get_switch_value(0) == 1.0);
    rig.poll();
    CHECK(s.get_switch(0));
    s.set_switch_value(0, 0.0);
    CHECK_FALSE(s.get_switch(0));
    rig.poll();
    CHECK_FALSE(s.get_switch(0));
    // A latched fault refuses both; the next good poll derives GetSwitch again.
    rig.fault();
    require_alpaca_error([&] { (void)s.get_switch(0); }, AlpacaError::DriverException);
    rig.poll();
    CHECK_FALSE(s.get_switch(0));
    CHECK(rig.upstream.calls("getswitch") == asked + 2 * 4);  // four good polls, each asking switches 1 and 2 only
    // A value the derivation cannot place (between the range's ends, or an upstream error): GetSwitch is asked.
    rig.upstream.set_for("getswitchvalue", "0", AlpacaUpstreamScript::number(0.5));
    rig.upstream.set_for("getswitch", "0", "true");
    rig.poll();
    CHECK(s.get_switch(0));
    rig.upstream.set_error_for("getswitchvalue", "0", 0x40B, "value unknown");
    rig.upstream.set_for("getswitch", "0", "false");
    rig.poll();
    require_alpaca_error([&] { (void)s.get_switch_value(0); }, AlpacaError::InvalidOperation);
    CHECK_FALSE(s.get_switch(0));
    CHECK(rig.upstream.calls("getswitch") == asked + 2 * 4 + 3 + 3);
}

// ASCOM leaves a multi-state switch's GetSwitch to the driver: the poll asks it, whatever the value.
TEST_CASE("Remote Alpaca Switch Driver - a multi-state switch's GetSwitch is the upstream's",
          "[remotealpaca][switch][unit]") {
    Rig rig;
    rig.connect();
    const int asked = rig.upstream.calls("getswitch");
    // Switch 1 (0 to 100) at its maximum, and an upstream that answers GetSwitch false there.
    rig.upstream.set_for("getswitchvalue", "1", AlpacaUpstreamScript::number(100.0));
    rig.upstream.set_for("getswitch", "1", "false");
    rig.poll();
    CHECK(rig.driver->get_switch_value(1) == 100.0);
    CHECK_FALSE(rig.driver->get_switch(1));
    CHECK(rig.upstream.calls("getswitch") == asked + 2);  // switches 1 and 2
}

// The derivation needs the range and the step the Connect read: an upstream error on any of them keeps both reads.
TEST_CASE("Remote Alpaca Switch Driver - an upstream error on a switch's range or step keeps both reads",
          "[remotealpaca][switch][unit]") {
    for (const char* member : {"minswitchvalue", "maxswitchvalue", "switchstep"}) {
        DYNAMIC_SECTION(member) {
            Rig rig;
            rig.upstream.set_error_for(member, "0", 0x400, "not implemented");
            rig.connect();
            const int asked = rig.upstream.calls("getswitch");
            rig.upstream.set_for("getswitch", "0", "false");  // the upstream's own answer, at the value 1
            rig.poll();
            CHECK(rig.upstream.calls("getswitch") == asked + 3);
            CHECK_FALSE(rig.driver->get_switch(0));
            CHECK(rig.driver->get_switch_value(0) == 1.0);
        }
    }
}

// A poll that takes longer than the poll period logs one WARN per Connect, with the time and the switch count. Each
// upstream call takes its time on the fake task clock (the script's call hook); the rig's poll is 6 calls.
TEST_CASE("Remote Alpaca Switch Driver - a poll slower than its period logs one WARN per Connect",
          "[remotealpaca][switch][unit]") {
    SlowPollWarns warns;
    Rig rig;
    rig.connect();
    rig.upstream.set_call_hook([&] { rig.clock.advance(std::chrono::milliseconds(150)); });  // 900 ms
    rig.poll();
    CHECK(warns.count() == 0);
    rig.upstream.set_call_hook([&] { rig.clock.advance(std::chrono::milliseconds(250)); });  // 1500 ms
    rig.poll();
    CHECK(warns.count() == 1);
    CHECK(warns.last().find("1500 ms") != std::string::npos);
    CHECK(warns.last().find("3 switches") != std::string::npos);
    rig.poll();
    CHECK(warns.count() == 1);  // once per Connect
    rig.upstream.set_call_hook({});
    rig.driver->set_connected(false);
    rig.connect();
    rig.upstream.set_call_hook([&] { rig.clock.advance(std::chrono::milliseconds(250)); });
    rig.poll();
    CHECK(warns.count() == 2);
    rig.upstream.set_call_hook({});
}

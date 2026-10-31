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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using namespace alpacacore::test;

// Falsified by: driver_rig.h:40 `==` changed to `!=` in the code comparison (the expected code is then rejected).
TEST_CASE("driver_rig - require_alpaca_error accepts the expected code", "[driver_rig]") {
    require_alpaca_error([] { throw AlpacaException("x", AlpacaError::NotConnected); }, AlpacaError::NotConnected);
}

// A failed helper aborts its test case, so each failure mode is its own [!shouldfail] case: the case passes
// only while the helper fails.
// Falsified by: require_alpaca_error() dropping the FAIL() after fn() (a call that does not throw passes).
TEST_CASE("driver_rig - require_alpaca_error fails when nothing is thrown", "[driver_rig][!shouldfail]") {
    require_alpaca_error([] {}, AlpacaError::NotConnected);
}

// Falsified by: require_alpaca_error() not comparing error_code() with the expected code.
TEST_CASE("driver_rig - require_alpaca_error fails on the wrong code", "[driver_rig][!shouldfail]") {
    require_alpaca_error([] { throw AlpacaException("x", AlpacaError::InvalidValue); }, AlpacaError::NotConnected);
}

// Falsified by: require_alpaca_error() catching std::exception and treating it as a pass.
TEST_CASE("driver_rig - require_alpaca_error does not swallow a non-Alpaca exception", "[driver_rig][!shouldfail]") {
    require_alpaca_error([] { throw std::runtime_error("boom"); }, AlpacaError::NotConnected);
}

// Falsified by: timed() returning zero or the start time instead of the elapsed time.
TEST_CASE("driver_rig - timed returns the elapsed duration of the call", "[driver_rig]") {
    const auto spent = timed([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); });
    CHECK(spent >= std::chrono::milliseconds(20));
    CHECK(spent < std::chrono::seconds(5));
}

// Falsified by: parse_timestamp() dropping the milliseconds or reading the month as zero-based.
TEST_CASE("driver_rig - parse_timestamp reads the DeviceState TimeStamp format", "[driver_rig]") {
    const auto stamp = parse_timestamp("2026-10-10T21:46:30.626Z");
    const auto base = parse_timestamp("2026-10-10T21:46:30.000Z");
    CHECK(stamp - base == std::chrono::milliseconds(626));
    CHECK(parse_timestamp("2026-10-10T21:46:31.000Z") - base == std::chrono::seconds(1));
    CHECK(parse_timestamp("2026-11-10T21:46:30.000Z") - base == std::chrono::hours(24 * 31));
}

// Falsified by: parse_timestamp() accepting text that is not the TimeStamp format.
TEST_CASE("driver_rig - parse_timestamp rejects a malformed stamp", "[driver_rig][!shouldfail]") {
    (void)parse_timestamp("not a timestamp");
}

// Falsified by: time_stamp_of() returning another entry's value or a non-empty string when TimeStamp is absent.
TEST_CASE("driver_rig - time_stamp_of picks the TimeStamp entry", "[driver_rig]") {
    std::vector<alpacacore::DeviceState> state;
    CHECK(time_stamp_of(state).empty());
    state.push_back({"Other", std::string("no")});
    state.push_back({"TimeStamp", std::string("2026-10-10T21:46:30.626Z")});
    CHECK(time_stamp_of(state) == "2026-10-10T21:46:30.626Z");
}

namespace {

// A poll body on the fake clock: it wakes every period and counts the laps.
class ToyPoller {
public:
    ToyPoller(FakeTaskClock& clock, std::chrono::nanoseconds period)
        : clock_(clock), period_(period), thread_([this] {
              while (!stop_.load()) {
                  clock_.sleep_for(period_);
                  ++laps_;
              }
          }) {}
    // The poll body is parked on the clock: one last advance() lets it see stop_ and exit.
    ~ToyPoller() {
        stop_.store(true);
        clock_.wait_for_waiters(1, std::chrono::seconds(5));
        clock_.advance(period_);
        thread_.join();
    }
    int laps() const { return laps_.load(); }

private:
    FakeTaskClock& clock_;
    std::chrono::nanoseconds period_;
    std::atomic<bool> stop_{false};
    std::atomic<int> laps_{0};
    std::thread thread_;
};

// The smallest AlpacaDriver whose Connect starts a poll body that parks on the clock, as a real driver's does.
class PollingStub : public alpacacore::AlpacaDriver {
public:
    PollingStub(FakeTaskClock& clock, bool polls, std::atomic<bool>* disconnected = nullptr)
        : clock_(clock), polls_(polls), disconnected_(disconnected) {}
    void set_connected(bool connected) override {
        if (!connected && disconnected_) disconnected_->store(true);
        if (connected && polls_ && !poller_) {
            poller_ = std::make_unique<ToyPoller>(clock_, std::chrono::seconds(1));
        }
        connected_ = connected;
    }
    bool get_connected() const override { return connected_; }
    int get_device_number() const override { return 0; }
    std::string get_name() const override { return "stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Focuser; }
    std::string get_description() const override { return "stub"; }
    std::string get_driver_info() const override { return "stub"; }
    std::string get_driver_version() const override { return "0"; }
    int get_interface_version() const override { return 1; }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return {}; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return {}; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return {}; }

protected:
    std::string fallback_unique_id() const override { return "stub"; }

private:
    FakeTaskClock& clock_;
    bool polls_;
    std::atomic<bool>* disconnected_;
    bool connected_ = false;
    std::unique_ptr<ToyPoller> poller_;
};

}  // namespace

// Falsified by: tick() dropping the wait_for_woken_settled() (laps() is read before the poller finished its lap) or
// not advancing the clock (laps() stays 0).
TEST_CASE("driver_rig - tick returns once the poller completed its lap and parked again", "[driver_rig]") {
    FakeTaskClock clock;
    ToyPoller poller(clock, std::chrono::seconds(1));
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    for (int lap = 1; lap <= 3; ++lap) {
        tick(clock, std::chrono::seconds(1), std::chrono::seconds(5));
        CHECK(poller.laps() == lap);
        CHECK(clock.waiter_count() == 1);
    }
}

// A failed helper aborts its test case, so the failure is a [!shouldfail] case.
// Falsified by: tick() dropping the REQUIRE on wait_for_waiters() (nothing waits, the call returns).
TEST_CASE("driver_rig - tick fails at the bound when nothing waits", "[driver_rig][!shouldfail]") {
    FakeTaskClock clock;
    tick(clock, std::chrono::seconds(1), std::chrono::milliseconds(50));
}

// Falsified by: connect_and_park() not calling set_connected(true) (the poll never starts, it returns false).
TEST_CASE("driver_rig - connect_and_park connects and reports the poll parked", "[driver_rig]") {
    FakeTaskClock clock;
    PollingStub driver(clock, true);
    CHECK(connect_and_park(driver, clock, std::chrono::seconds(5)));
    CHECK(driver.get_connected());
    CHECK(clock.waiter_count() == 1);
}

// Falsified by: connect_and_park() returning true without waiting for the waiter.
TEST_CASE("driver_rig - connect_and_park returns false when the poll never parks", "[driver_rig]") {
    FakeTaskClock clock;
    PollingStub driver(clock, false);
    CHECK_FALSE(connect_and_park(driver, clock, std::chrono::milliseconds(50)));
    CHECK(driver.get_connected());
}

namespace {

struct StubFake {
    int tag = 7;
};

using StubRig = PolledRig<StubFake, PollingStub>;
constexpr RigTiming kRigTiming{std::chrono::seconds(1), std::chrono::seconds(5)};

}  // namespace

// Falsified by: PolledRig::connect() not waiting for the waiter (the CHECK on waiter_count() fails) or not calling
// set_connected(true).
TEST_CASE("driver_rig - PolledRig connect returns true and the poll is parked", "[driver_rig]") {
    StubRig rig(kRigTiming, [](StubFake&, FakeTaskClock& c) { return std::make_unique<PollingStub>(c, true); });
    CHECK_FALSE(rig.driver().get_connected());
    CHECK(rig.connect());
    CHECK(rig.driver().get_connected());
    CHECK(rig.clock().waiter_count() == 1);
}

// Falsified by: PolledRig::connect() always returning true.
TEST_CASE("driver_rig - PolledRig connect returns false when the poll never parks", "[driver_rig]") {
    StubRig rig({std::chrono::seconds(1), std::chrono::milliseconds(50)},
                [](StubFake&, FakeTaskClock& c) { return std::make_unique<PollingStub>(c, false); });
    CHECK_FALSE(rig.connect());
}

// Falsified by: PolledRig::tick(n) ticking once.
TEST_CASE("driver_rig - PolledRig tick(3) makes three laps", "[driver_rig]") {
    StubRig rig(kRigTiming, [](StubFake&, FakeTaskClock& c) { return std::make_unique<PollingStub>(c, true); });
    REQUIRE(rig.connect());
    const auto before = rig.clock().now();
    rig.tick(3);
    CHECK(rig.clock().now() - before == 3 * kRigTiming.period);
    CHECK(rig.clock().waiter_count() == 1);
}

// Falsified by: PolledRig::expect_error() swapping code and fn or dropping the code check.
TEST_CASE("driver_rig - PolledRig expect_error accepts the expected code", "[driver_rig]") {
    StubRig rig(kRigTiming, [](StubFake&, FakeTaskClock& c) { return std::make_unique<PollingStub>(c, false); });
    rig.expect_error(AlpacaError::NotConnected, [] { throw AlpacaException("x", AlpacaError::NotConnected); });
}

// Falsified by: PolledRig::expect_error() not comparing the code.
TEST_CASE("driver_rig - PolledRig expect_error fails on the wrong code", "[driver_rig][!shouldfail]") {
    StubRig rig(kRigTiming, [](StubFake&, FakeTaskClock& c) { return std::make_unique<PollingStub>(c, false); });
    rig.expect_error(AlpacaError::NotConnected, [] { throw AlpacaException("x", AlpacaError::InvalidValue); });
}

// Falsified by: ~PolledRig() without the set_connected(false).
TEST_CASE("driver_rig - PolledRig destructor disconnects a connected driver", "[driver_rig]") {
    std::atomic<bool> disconnected{false};
    {
        StubRig rig(kRigTiming,
                    [&](StubFake&, FakeTaskClock& c) { return std::make_unique<PollingStub>(c, true, &disconnected); });
        REQUIRE(rig.connect());
        CHECK_FALSE(disconnected.load());
    }
    CHECK(disconnected.load());
}

// Falsified by: PolledRig calling the factory with a copy of the fake or clock.
TEST_CASE("driver_rig - PolledRig hands the factory its own fake and clock", "[driver_rig]") {
    StubFake* seen_fake = nullptr;
    FakeTaskClock* seen_clock = nullptr;
    StubRig rig(kRigTiming, [&](StubFake& f, FakeTaskClock& c) {
        seen_fake = &f;
        seen_clock = &c;
        return std::make_unique<PollingStub>(c, false);
    });
    CHECK(seen_fake == &rig.fake());
    CHECK(seen_clock == &rig.clock());
}

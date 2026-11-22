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

// Tests for util::SoftPwmPorts, the port table and soft-PWM engine of the GPIO power-port boards
// (issue #1757, decision record docs/decisions/0027-gpio-soft-pwm-composition.md), over
// FakeGpioLineBackend and FakeTaskClock: the duty-cycle edges are checked in virtual time, and no case
// sleeps. Every blocking step carries a real-time bound, so a defect fails the case instead of hanging
// the binary. The set_value storm during close() is the [stress] case in
// test_zwo_asiair_concurrency_stress.cpp.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/soft_pwm_ports.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_gpio_line_backend.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeGpioLineBackend;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::require_alpaca_error;
using alpacacore::util::PortsConfig;
using alpacacore::util::PowerPort;
using alpacacore::util::SoftPwmPorts;
using Event = FakeGpioLineBackend::Event;
using namespace std::chrono_literals;

PortsConfig board(unsigned int hz, std::vector<PowerPort> ports) {
    PortsConfig cfg;
    cfg.device_path = "/dev/gpiochip-fake";
    cfg.ports = std::move(ports);
    cfg.pwm_frequency_hz = hz;
    cfg.model_name = "Fake Board";
    cfg.log_category = "SOFT_PWM_TEST";
    return cfg;
}

// Clock, backend and engine in the order they must die in: the engine first.
struct Rig {
    FakeTaskClock clock;
    FakeGpioLineBackend backend{clock};
    SoftPwmPorts ports;
    explicit Rig(PortsConfig cfg) : ports(std::move(cfg), backend, clock) {}
};

// Virtual time of an event since the clock's start, in microseconds.
long long at_us(const Event& e) {
    return std::chrono::duration_cast<std::chrono::microseconds>(e.at - FakeTaskClock::clock::time_point{}).count();
}

// Blocks until the clock has registered @p n waits in total (a worker is parked in its next timed wait).
void wait_for_waits(Rig& rig, std::uint64_t n) { REQUIRE(rig.clock.wait_for_wait_count(n, 2s)); }

// The ERROR lines logged so far; the engine's workers log from their own threads.
struct ErrorCapture {
    std::vector<std::string> errors;
    std::mutex mutex;
    std::condition_variable cv;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    ErrorCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level != alpacacore::logging::LogLevel::Error) return;
                std::lock_guard<std::mutex> lock(mutex);
                errors.emplace_back(message);
                cv.notify_all();
            });
    }
    // Blocks until @p n errors were logged, or 2 s of real time pass.
    bool wait_for_errors(std::size_t n) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return errors.size() >= n; });
    }
    ~ErrorCapture() { alpacacore::logging::set_log_sink(previous); }
    ErrorCapture(const ErrorCapture&) = delete;
    ErrorCapture& operator=(const ErrorCapture&) = delete;
    ErrorCapture(ErrorCapture&&) = delete;
    ErrorCapture& operator=(ErrorCapture&&) = delete;
};

}  // namespace

// Falsified by: soft_pwm_ports.cpp open() passing a line-less or read-only port to the backend, or writing a
// level when it claims the lines.
TEST_CASE("SoftPwmPorts - open claims the writable lines once and writes nothing", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Cam", 12u, true, false},
                       {"Pass-through", std::nullopt, false, false},
                       {"Read-only", 13u, false, false},
                       {"Dew", 26u, true, true}}));
    REQUIRE_FALSE(rig.ports.is_open());
    rig.ports.open();

    CHECK(rig.ports.is_open());
    CHECK(rig.backend.opened_path() == "/dev/gpiochip-fake");
    CHECK(rig.backend.opened_lines() == std::vector<unsigned int>{12u, 26u});
    CHECK(rig.backend.write_count() == 0);
    CHECK(rig.backend.events().front().kind == Event::Kind::Open);
    // Reopening an open engine claims nothing again.
    rig.ports.open();
    CHECK(rig.backend.events().size() == 1);
}

// Falsified by: soft_pwm_ports.cpp open() skipping reconcile_cached_value, or arming a port on a backend that
// cannot read its lines.
TEST_CASE("SoftPwmPorts - open reads each line into the cache, or keeps the on cache when it cannot",
          "[util][soft-pwm][unit]") {
    {
        Rig rig(
            board(50, {{"A", 1u, true, false}, {"B", 2u, true, false}, {"C", 3u, true, true}, {"D", 4u, true, true}}));
        rig.backend.set_level(1, 0);  // B's line was left low
        rig.backend.set_level(3, 0);  // D's line was left low
        rig.ports.open();
        CHECK(rig.ports.get_value(0) == 1);
        CHECK(rig.ports.get_value(1) == 0);
        CHECK(rig.ports.get_value(2) == 100);  // PWM port, line high: full duty
        CHECK(rig.ports.get_value(3) == 0);    // PWM port, line low: 0 %, not 100 %
        CHECK(rig.backend.write_count() == 0);
    }
    {
        Rig rig(board(50, {{"A", 1u, true, false}, {"C", 3u, true, true}}));
        rig.backend.set_unreadable();
        rig.ports.open();
        CHECK(rig.ports.get_value(0) == 1);
        CHECK(rig.ports.get_value(1) == 100);
        CHECK(rig.backend.write_count() == 0);
    }
}

// Falsified by: soft_pwm_ports.cpp set_value() caching the boolean write (skipping an unchanged value), or not
// storing the value after a successful write.
TEST_CASE("SoftPwmPorts - a boolean port writes each set and reads the cache back", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"A", 12u, true, false}, {"B", 13u, true, false}}));
    rig.ports.open();

    rig.ports.set_value(1, 0);
    REQUIRE(rig.backend.writes_to(1).size() == 1);
    CHECK_FALSE(rig.backend.writes_to(1)[0].high);
    CHECK(rig.ports.get_value(1) == 0);
    CHECK(rig.ports.get_value(0) == 1);  // the other port is untouched
    CHECK(rig.backend.writes_to(0).empty());

    // Each set is written: an unreadable line may not match the cache.
    rig.ports.set_value(1, 0);
    CHECK(rig.backend.writes_to(1).size() == 2);
    rig.ports.set_value(1, 1);
    REQUIRE(rig.backend.writes_to(1).size() == 3);
    CHECK(rig.backend.writes_to(1)[2].high);
    CHECK(rig.ports.get_value(1) == 1);
}

// Falsified by: soft_pwm_ports.cpp pwm_loop() computing the on time from anything but period * duty / 100, or
// writing the edges off the clock's now().
TEST_CASE("SoftPwmPorts - PWM 50 % at 50 Hz is one high at t0 and one low 10 ms later", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Dew", 5u, true, true}}));
    rig.ports.open();
    const auto base = rig.clock.wait_count();
    rig.ports.set_value(0, 50);
    wait_for_waits(rig, base + 1);

    auto writes = rig.backend.writes_to(0);
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].high);
    CHECK(at_us(writes[0]) == 0);

    rig.clock.advance(10ms);
    wait_for_waits(rig, base + 2);
    writes = rig.backend.writes_to(0);
    REQUIRE(writes.size() == 2);
    CHECK_FALSE(writes[1].high);
    CHECK(at_us(writes[1]) == 10000);

    // The next period starts on the 20 ms edge.
    rig.clock.advance(10ms);
    wait_for_waits(rig, base + 3);
    writes = rig.backend.writes_to(0);
    REQUIRE(writes.size() == 3);
    CHECK(writes[2].high);
    CHECK(at_us(writes[2]) == 20000);
    CHECK(rig.ports.get_value(0) == 50);
}

// Falsified by: soft_pwm_ports.cpp pwm_loop() starting each period at the clock's now() instead of at the previous
// period's start plus the period (the late wake then moves every later edge).
TEST_CASE("SoftPwmPorts - edges are absolute deadlines, a late wake does not shift the next period",
          "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Dew", 5u, true, true}}));
    rig.ports.open();
    const auto base = rig.clock.wait_count();
    rig.ports.set_value(0, 50);
    wait_for_waits(rig, base + 1);
    rig.clock.advance(12ms);  // the worker wakes 2 ms after its low edge was due
    wait_for_waits(rig, base + 2);
    rig.clock.advance(13ms);  // and 5 ms after the next period's high edge was due (20 ms)
    wait_for_waits(rig, base + 3);
    rig.clock.advance(5ms);   // the low edge is due at 20 ms + 10 ms
    wait_for_waits(rig, base + 4);
    const auto writes = rig.backend.writes_to(0);
    REQUIRE(writes.size() == 4);
    CHECK(at_us(writes[1]) == 12000);
    CHECK(at_us(writes[2]) == 25000);
    CHECK(at_us(writes[3]) == 30000);
}

// Falsified by: soft_pwm_ports.cpp pwm_loop() using a fixed half period instead of duty / 100 of the period.
TEST_CASE("SoftPwmPorts - PWM 25 % puts the low edge at 5 ms", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Dew", 5u, true, true}}));
    rig.ports.open();
    const auto base = rig.clock.wait_count();
    rig.ports.set_value(0, 25);
    wait_for_waits(rig, base + 1);
    rig.clock.advance(5ms);
    wait_for_waits(rig, base + 2);
    const auto writes = rig.backend.writes_to(0);
    REQUIRE(writes.size() == 2);
    CHECK_FALSE(writes[1].high);
    CHECK(at_us(writes[1]) == 5000);
}

// Falsified by: soft_pwm_ports.cpp period_ taking another frequency than config.pwm_frequency_hz.
TEST_CASE("SoftPwmPorts - the period follows the configured frequency", "[util][soft-pwm][unit]") {
    // The board defaults: ASIAIR Pro 1000 Hz (1 ms period), ASIAIR Plus 50 Hz (20 ms), StellaVita 100 Hz (10 ms).
    for (const auto& [hz, low_edge_us] :
         std::vector<std::pair<unsigned int, long long>>{{1000, 500}, {50, 10000}, {100, 5000}}) {
        INFO(hz << " Hz");
        Rig rig(board(hz, {{"Dew", 5u, true, true}}));
        rig.ports.open();
        const auto base = rig.clock.wait_count();
        rig.ports.set_value(0, 50);
        wait_for_waits(rig, base + 1);
        rig.clock.advance(std::chrono::microseconds(low_edge_us));
        wait_for_waits(rig, base + 2);
        const auto writes = rig.backend.writes_to(0);
        REQUIRE(writes.size() == 2);
        CHECK(at_us(writes[1]) == low_edge_us);
    }
}

// Falsified by: soft_pwm_ports.cpp pwm_loop() looping the steady branch on the clock, or dropping the level
// check that skips the write at 0 % and 100 %.
TEST_CASE("SoftPwmPorts - steady 0 % and 100 % write once and then nothing", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Full", 5u, true, true}, {"Off", 6u, true, true}}));
    rig.backend.set_level(0, 0);  // both lines were left low
    rig.backend.set_level(1, 0);
    rig.ports.open();
    REQUIRE(rig.ports.get_value(0) == 0);

    rig.ports.set_value(0, 100);
    REQUIRE(rig.backend.wait_for_writes(1, 2s));
    rig.ports.set_value(1, 0);  // the line already holds low: nothing to write
    for (int period = 0; period < 10; ++period) {
        rig.clock.advance(20ms);
    }
    // A write that must not come: bounded real wait.
    CHECK_FALSE(rig.backend.wait_for_writes(2, 100ms));
    CHECK(rig.backend.writes_to(0).size() == 1);
    CHECK(rig.backend.writes_to(0)[0].high);
    CHECK(rig.backend.writes_to(1).empty());
}

// Falsified by: soft_pwm_ports.cpp close_locked() skipping the settle write, writing the opposite level, or
// writing a boolean line.
TEST_CASE("SoftPwmPorts - close settles each armed PWM line at its steady level and joins the workers",
          "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Half", 5u, true, true}, {"Off", 6u, true, true}, {"Bool", 7u, true, false}}));
    rig.ports.open();
    auto base = rig.clock.wait_count();
    rig.ports.set_value(0, 50);
    rig.ports.set_value(1, 0);
    wait_for_waits(rig, base + 1);
    rig.clock.advance(10ms);  // the 50 % port is in the low half of its cycle
    wait_for_waits(rig, base + 2);
    const auto bool_writes = rig.backend.writes_to(2).size();

    rig.ports.close();

    CHECK_FALSE(rig.ports.is_open());
    CHECK_FALSE(rig.backend.is_open());
    CHECK(rig.backend.events().back().kind == Event::Kind::Close);
    REQUIRE_FALSE(rig.backend.writes_to(0).empty());
    REQUIRE_FALSE(rig.backend.writes_to(1).empty());
    CHECK(rig.backend.writes_to(0).back().high);            // duty 50: released high
    CHECK_FALSE(rig.backend.writes_to(1).back().high);      // duty 0: released low
    CHECK(rig.backend.writes_to(2).size() == bool_writes);  // a boolean line is left as commanded
    // Closed: nothing is written any more, and a second close is a no-op.
    const auto events = rig.backend.events().size();
    rig.clock.advance(100ms);
    rig.ports.close();
    CHECK(rig.backend.events().size() == events);
}

// Falsified by: soft_pwm_ports.cpp close_locked() settling a port no set_value armed, or open() arming a port on
// a backend that cannot read its lines.
TEST_CASE("SoftPwmPorts - on a backend that cannot read, an unarmed PWM port is neither written nor settled",
          "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Unset", 5u, true, true}, {"Set", 6u, true, true}}));
    rig.backend.set_unreadable();
    rig.ports.open();
    CHECK(rig.ports.get_value(0) == 100);
    // The worker's first pass is immediate: a write that must not come.
    CHECK_FALSE(rig.backend.wait_for_writes(1, 100ms));

    rig.ports.set_value(1, 40);
    rig.ports.close();
    CHECK(rig.backend.writes_to(0).empty());  // never driven, never settled
    REQUIRE_FALSE(rig.backend.writes_to(1).empty());
    CHECK(rig.backend.writes_to(1).back().high);  // settled at the steady level of 40 %
    // The next open() writes nothing and cannot read the line back, so the cache reports the settled level.
    rig.ports.open();
    CHECK(rig.ports.get_value(1) == 100);
    CHECK(rig.ports.get_value(0) == 100);
}

// Falsified by: soft_pwm_ports.cpp pwm_loop() not logging at ERROR, not marking the port failed, or writing
// again after a failed write; set_value() answering success for a failed port.
TEST_CASE("SoftPwmPorts - a failed PWM write logs ERROR, stops that port's worker and no other",
          "[util][soft-pwm][unit]") {
    ErrorCapture log;
    Rig rig(board(50, {{"Dew", 5u, true, true}, {"Cam", 6u, true, false}, {"Dew2", 7u, true, true}}));
    rig.backend.fail_write(0);
    rig.ports.open();
    rig.ports.set_value(0, 50);
    REQUIRE(log.wait_for_errors(1));
    // The worker has stopped: advancing the clock brings no second write.
    rig.clock.advance(100ms);
    CHECK_FALSE(rig.backend.wait_for_writes(2, 100ms));
    CHECK(rig.backend.writes_to(0).size() == 1);
    {
        std::lock_guard<std::mutex> lock(log.mutex);
        REQUIRE(log.errors.size() == 1);
        CHECK(log.errors[0].find("Dew") != std::string::npos);
    }
    // The port reports the failure instead of success, until the next open().
    require_alpaca_error([&] { rig.ports.set_value(0, 60); }, AlpacaError::DriverException);

    // Other ports are unaffected.
    rig.ports.set_value(1, 0);
    CHECK(rig.ports.get_value(1) == 0);
    const auto base = rig.clock.wait_count();
    rig.ports.set_value(2, 50);
    wait_for_waits(rig, base + 1);
    CHECK(rig.backend.writes_to(2).size() == 1);
    CHECK(rig.ports.get_value(2) == 50);

    // A new open() starts both workers again, once the line takes writes: each holds its 50 % duty and
    // writes its first edge at once.
    rig.ports.close();
    rig.backend.fail_write(0, false);
    const auto before = rig.backend.write_count();
    rig.ports.open();
    REQUIRE(rig.backend.wait_for_writes(before + 2, 2s));
    CHECK(rig.backend.writes_to(0).back().ok);
    rig.ports.set_value(0, 60);
}

// Falsified by: soft_pwm_ports.cpp set_value() storing the value or returning when the backend write failed.
TEST_CASE("SoftPwmPorts - a failed boolean write throws DriverException and keeps the cache",
          "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"A", 12u, true, false}}));
    rig.backend.fail_write(0);
    rig.ports.open();
    require_alpaca_error([&] { rig.ports.set_value(0, 0); }, AlpacaError::DriverException);
    CHECK(rig.ports.get_value(0) == 1);
}

// Falsified by: soft_pwm_ports.cpp set_value() checking the connection before writability, or letting a write
// reach the backend for a read-only port.
TEST_CASE("SoftPwmPorts - a read-only port answers NotImplemented open or not", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Pass-through", std::nullopt, false, false}, {"Cam", 12u, true, false}}));
    require_alpaca_error([&] { rig.ports.set_value(0, 1); }, AlpacaError::NotImplemented);
    rig.ports.open();
    require_alpaca_error([&] { rig.ports.set_value(0, 0); }, AlpacaError::NotImplemented);
    CHECK(rig.backend.write_count() == 0);
}

// Falsified by: soft_pwm_ports.cpp set_value() dropping the range check, or ordering it after the connection check.
TEST_CASE("SoftPwmPorts - value and index errors precede the connection check", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Cam", 12u, true, false}, {"Dew", 13u, true, true}}));
    require_alpaca_error([&] { rig.ports.set_value(0, 2); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.ports.set_value(0, -1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.ports.set_value(1, 101); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.ports.set_value(2, 0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)rig.ports.get_value(2); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.ports.set_value(0, 1); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { rig.ports.set_value(1, 50); }, AlpacaError::NotConnected);
    CHECK(rig.backend.write_count() == 0);
}

// Falsified by: soft_pwm_ports.cpp open() leaving is_open() or the workers set after the backend refused.
TEST_CASE("SoftPwmPorts - a refused open leaves the engine closed", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"Dew", 5u, true, true}}));
    rig.backend.fail_open();
    require_alpaca_error([&] { rig.ports.open(); }, AlpacaError::NotConnected);
    CHECK_FALSE(rig.ports.is_open());
    require_alpaca_error([&] { rig.ports.set_value(0, 10); }, AlpacaError::NotConnected);
    rig.backend.fail_open(false);
    rig.ports.open();
    CHECK(rig.ports.is_open());
}

// Falsified by: soft_pwm_ports.cpp constructor dropping any of its three table checks.
TEST_CASE("SoftPwmPorts - the constructor rejects a bad table", "[util][soft-pwm][unit]") {
    FakeTaskClock clock;
    FakeGpioLineBackend backend(clock);
    require_alpaca_error([&] { SoftPwmPorts(board(50, {}), backend, clock); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { SoftPwmPorts(board(0, {{"A", 1u, true, false}}), backend, clock); },
                         AlpacaError::InvalidValue);
    require_alpaca_error([&] { SoftPwmPorts(board(100001, {{"A", 1u, true, false}}), backend, clock); },
                         AlpacaError::InvalidValue);
    // A writable port needs a line to drive.
    require_alpaca_error([&] { SoftPwmPorts(board(50, {{"A", std::nullopt, true, false}}), backend, clock); },
                         AlpacaError::InvalidValue);
    CHECK_NOTHROW(SoftPwmPorts(board(100000, {{"A", 1u, true, false}}), backend, clock));
}

// Issue #772 (CC-15), the ASIAIR Pro wrapper case moved onto the fake backend with the same assertions:
// close() stops the PWM workers wherever they are in the cycle, so without a settle step a port at 50 % duty is
// released low about half the time and the heater on it loses power at disconnect. ascom-contract.instructions.md
// "Never power-cycle on disconnect": duty > 0 is released high, duty 0 low. Virtual time varies where in the
// 10 ms period close() lands, where the real-time case slept 30-49 ms.
// Falsified by: soft_pwm_ports.cpp close_locked() writing nothing or the opposite level when it settles a PWM line.
TEST_CASE("SoftPwmPorts - close releases each PWM line at its steady level wherever the cycle is",
          "[util][soft-pwm][unit]") {
    constexpr int kCloses = 20;
    int half_released_low = 0;
    int off_released_high = 0;
    int not_released = 0;
    for (int i = 0; i < kCloses; ++i) {
        Rig rig(board(100, {{"DC1", 5u, true, true}, {"DC2", 6u, true, true}}));
        rig.ports.open();
        const auto base = rig.clock.wait_count();
        rig.ports.set_value(0, 50);
        rig.ports.set_value(1, 0);
        wait_for_waits(rig, base + 1);
        // Land close() on an edge, or between two, by whole microseconds of a 10 ms period.
        rig.clock.advance(std::chrono::microseconds(((i * 7) % 20) * 500));
        rig.clock.wait_for_woken_settled(2s);
        rig.ports.close();
        const auto half = rig.backend.writes_to(0);
        const auto off = rig.backend.writes_to(1);
        not_released += half.empty() ? 1 : 0;
        half_released_low += !half.empty() && !half.back().high ? 1 : 0;
        off_released_high += !off.empty() && off.back().high ? 1 : 0;
    }
    INFO(half_released_low << "/" << kCloses << " closes released the 50 % port low");
    CHECK(not_released == 0);
    CHECK(half_released_low == 0);
    CHECK(off_released_high == 0);
}

// Issue #300, the ASIAIR Pro wrapper case moved onto the fake backend with the same assertions: claiming the
// lines reads each one and changes none. Before, open() requested every line high, so a connect powered on a
// port the boot left off.
// Falsified by: soft_pwm_ports.cpp open() writing a level while it claims, or close_locked() settling the PWM
// line at the wrong level so a reconnect loses the duty.
TEST_CASE("SoftPwmPorts - open claims each line at its level and changes none, a reconnect resumes the duty",
          "[util][soft-pwm][unit][power-on]") {
    Rig rig(board(100, {{"Port 1", 12u, true, false},
                        {"Port 2", 13u, true, false},
                        {"Port 3", 26u, true, true},
                        {"Port 4", 18u, true, true}}));
    rig.backend.set_level(1, 0);
    rig.backend.set_level(3, 0);
    rig.ports.open();
    CHECK(rig.ports.get_value(0) == 1);
    CHECK(rig.ports.get_value(1) == 0);
    CHECK(rig.ports.get_value(2) == 100);  // PWM port, line high: full duty
    CHECK(rig.ports.get_value(3) == 0);    // PWM port, line low: 0 %, not 100 %
    CHECK(rig.backend.write_count() == 0);

    // close() leaves a 40 % port at its steady level (high); a reconnect resumes 40 %.
    const auto base = rig.clock.wait_count();
    rig.ports.set_value(2, 40);
    wait_for_waits(rig, base + 1);
    rig.ports.close();
    REQUIRE_FALSE(rig.backend.writes_to(2).empty());
    CHECK(rig.backend.writes_to(2).back().high);
    rig.ports.open();
    CHECK(rig.ports.get_value(2) == 40);
    CHECK(rig.ports.get_value(1) == 0);
    rig.ports.close();
}

// close() must not release the backend while a boolean write is in flight: the I/O lock is held from the open check
// to the end of the write (a write through a released request is refused by the libgpiod stub chip, and is a
// use-after-close on a real one). The case holds one write inside the backend with the fake's before_write hook.
// Falsified by: soft_pwm_ports.cpp set_value() releasing io_mutex_ between its open check and the backend write.
TEST_CASE("SoftPwmPorts - close waits for a boolean write in flight", "[util][soft-pwm][unit]") {
    Rig rig(board(50, {{"A", 12u, true, false}}));
    rig.ports.open();
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool in_write = false;
    bool release = false;
    rig.backend.set_before_write([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        in_write = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release; });
    });
    std::thread writer([&] { rig.ports.set_value(0, 0); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, 2s, [&] { return in_write; }));
    }
    bool closed = false;
    std::thread closer([&] {
        rig.ports.close();
        std::lock_guard<std::mutex> lock(gate_mutex);
        closed = true;
        gate_cv.notify_all();
    });
    {
        // A close that must not finish yet: bounded real wait.
        std::unique_lock<std::mutex> lock(gate_mutex);
        CHECK_FALSE(gate_cv.wait_for(lock, 100ms, [&] { return closed; }));
        release = true;
        gate_cv.notify_all();
    }
    writer.join();
    closer.join();
    const auto events = rig.backend.events();
    REQUIRE(events.size() == 3);
    CHECK(events[1].kind == Event::Kind::Write);
    CHECK(events[2].kind == Event::Kind::Close);
    CHECK_FALSE(rig.ports.is_open());
}

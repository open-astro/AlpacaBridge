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

// open-astro#1368, iOptron: the connect-time clock-sync body runs on an operation slot. A disconnect and the
// destructor cancel and join it, and a body that throws is kept by the slot until the next start.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "catch2_compat.h"
#include "fake_ioptron_mount.h"
#include "fake_task_clock.h"

namespace {

namespace ioptron = alpacacore::vendor::ioptron;

ioptron::ConnectionInfo endpoint(int port) {
    ioptron::ConnectionInfo conn;
    conn.type = ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    return conn;
}

// A hook the body blocks in until release(); entered() reports it got there.
class Gate {
public:
    void block() {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
        finished_ = true;
    }
    bool wait_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return entered_; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }
    bool finished() {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
    bool finished_ = false;
};

}  // namespace

TEST_CASE("iOptron Telescope Driver - a clock-sync body that throws is kept until the next start",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    CHECK_FALSE(ioptron::clock_sync_failure_for_testing(*driver).has_value());

    ioptron::set_clock_sync_hook_for_testing(*driver, [] { throw std::runtime_error("clock sync boom"); });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));

    // The body runs on its own thread: wait for the slot to keep the throw.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ioptron::clock_sync_failure_for_testing(*driver) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto failure = ioptron::clock_sync_failure_for_testing(*driver);
    REQUIRE(failure.has_value());
    CHECK(*failure == "clock sync boom");
    CHECK(driver->get_connected());  // clock sync is best effort: the connection stands

    // Disconnect keeps the failure; the next start clears it.
    driver->set_connected(false);
    CHECK(ioptron::clock_sync_failure_for_testing(*driver).has_value());
    ioptron::set_clock_sync_hook_for_testing(*driver, {});
    driver->set_connected(true);
    CHECK_FALSE(ioptron::clock_sync_failure_for_testing(*driver).has_value());
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - a disconnect joins a clock-sync body that is mid-run",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    Gate gate;
    ioptron::set_clock_sync_hook_for_testing(*driver, [&gate] { gate.block(); });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(gate.wait_entered());

    std::promise<void> disconnected;
    auto done = disconnected.get_future();
    std::thread disconnecter([&] {
        driver->set_connected(false);
        disconnected.set_value();
    });
    // The join must wait for the running body, not abandon it.
    CHECK(done.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);
    gate.release();
    CHECK(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    disconnecter.join();
    CHECK(gate.finished());
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("iOptron Telescope Driver - a disconnect during a clock-sync command stops the remaining commands",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    // The first site command is in flight (reply held) when the disconnect lands.
    mount.hold_next_reply(std::chrono::milliseconds(800));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));
    driver->set_connected(false);
    CHECK(mount.count(":SLO") == 0);
    CHECK(mount.count(":SUT") == 0);
}

TEST_CASE("iOptron Telescope Driver - the destructor joins a clock-sync body that is mid-run",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    Gate gate;
    ioptron::set_clock_sync_hook_for_testing(*driver, [&gate] { gate.block(); });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(gate.wait_entered());

    std::thread releaser([&gate] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gate.release();
    });
    driver.reset();  // joins the body: returns only once the gate is released
    CHECK(gate.finished());
    releaser.join();
}

TEST_CASE("iOptron Telescope Driver - a second clock-sync start supersedes a body blocked in the first",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    // The first body blocks in the hook, before it sends any command; once released the hook returns at once.
    Gate gate;
    ioptron::set_clock_sync_hook_for_testing(*driver, [&gate] { gate.block(); });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(gate.wait_entered());

    // The second start must return while the first body is still blocked (a start that joins would wait for it).
    std::promise<void> started;
    auto started_future = started.get_future();
    std::thread starter([&] {
        ioptron::start_clock_sync_for_testing(*driver);
        started.set_value();
    });
    const bool returned = started_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!returned) gate.release();  // let a start that joined finish, so the case fails instead of hanging
    starter.join();
    REQUIRE(returned);
    CHECK_FALSE(gate.finished());

    // Released, the first body sees it was superseded and sends nothing; the second runs its commands.
    gate.release();
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (mount.count(":SUT") < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    driver->set_connected(false);  // joins both bodies
    CHECK(mount.count(":SLA") == 1);
    CHECK(mount.count(":SLO") == 1);
    CHECK(mount.count(":SUT") == 1);
}

TEST_CASE("iOptron Telescope Driver - a superseded clock-sync body sends no further mount command",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    // The first body's first site command is in flight (reply held) when the second start lands. The driver
    // reads for 200 ms only, so the body gives up on that command and meets its stop between commands.
    mount.hold_next_reply(std::chrono::milliseconds(1500));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));
    ioptron::start_clock_sync_for_testing(*driver);

    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (mount.count(":SUT") < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    driver->set_connected(false);  // joins both bodies
    // Only the second body reaches the longitude and the clock commands.
    CHECK(mount.count(":SLO") == 1);
    CHECK(mount.count(":SUT") == 1);
}

#endif  // _WIN32

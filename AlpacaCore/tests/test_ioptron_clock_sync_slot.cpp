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
// open-astro#2419: the body writes the mount clock only from a trusted host clock, and a host clock step or a
// trust edge after the connect re-syncs it on the same slot.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
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

// The ":GUT#" reply for a mount clock at `utc` (v3.10: sign, timezone, DST, then UTC ms since J2000).
std::string gut_reply_for(std::chrono::system_clock::time_point utc) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        utc - std::chrono::system_clock::time_point(std::chrono::seconds(946728000)))
                        .count();
    char text[32];
    std::snprintf(text, sizeof text, "+0000%013lld#", static_cast<long long>(ms));
    return text;
}

// The UTC a ":SUT" command carries.
std::chrono::system_clock::time_point sut_time(const std::string& command) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(946728000)) +
           std::chrono::milliseconds(std::stoll(command.substr(4, 13)));
}

template <typename Pred>
bool eventually(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// The iOptron log lines at Info and above, for the case's lifetime.
class IoptronLog {
public:
    IoptronLog() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel, std::string_view component, std::string_view message) {
                if (component != "iOptron") return;
                std::lock_guard<std::mutex> lock(mutex_);
                lines_.emplace_back(message);
            });
    }
    ~IoptronLog() { alpacacore::logging::set_log_sink(previous_); }
    IoptronLog(const IoptronLog&) = delete;
    IoptronLog& operator=(const IoptronLog&) = delete;
    bool contains(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(lines_.begin(), lines_.end(),
                           [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }

private:
    alpacacore::logging::LogSink previous_ = alpacacore::logging::get_log_sink();
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

// RC51 on 2026-11-19: the Pi booted on 09:05:57 (its last shutdown) and was stepped to 15:54:24; the mount had
// lost its time at power-up and read 2017-01-01.
const auto kStaleHost = std::chrono::system_clock::time_point(std::chrono::seconds(1795079157));
const auto kRealHost = std::chrono::system_clock::time_point(std::chrono::seconds(1795103664));
const auto kMountPowerUp = std::chrono::system_clock::time_point(std::chrono::seconds(1483272042));
constexpr std::chrono::milliseconds kRc51Step{24507000};

// A mount connected on an untrusted host clock: the connect's body has sent its site commands and no clock command.
// The host UTC seam reads `host`; the clock-sync UTC hook counts the body's finished passes in `passes`.
struct UntrustedConnect {
    alpacacore::test::FakeIoptronMount mount{"0036", 0.0};
    alpacacore::test::FakeTaskClock clock;
    std::atomic<long long> host_s{
        std::chrono::duration_cast<std::chrono::seconds>(kStaleHost.time_since_epoch()).count()};
    std::atomic<int> passes{0};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    void connect() {
        REQUIRE(mount.ok());
        mount.set_utc_reply(gut_reply_for(kMountPowerUp));
        driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                             std::nullopt, true, clock);
        ioptron::set_host_utc_source_for_testing(
            *driver, [this] { return std::chrono::system_clock::time_point(std::chrono::seconds(host_s.load())); });
        ioptron::set_clock_sync_utc_hook_for_testing(*driver, [this](bool after_send) {
            if (after_send) ++passes;
        });
        driver->set_connected(true);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::milliseconds(250));
        REQUIRE(eventually([&] { return passes.load() == 1; }));
    }
    void set_host(std::chrono::system_clock::time_point utc) {
        host_s.store(std::chrono::duration_cast<std::chrono::seconds>(utc.time_since_epoch()).count());
    }
    // Sends the event and lets the re-sync body past its start delay.
    void host_clock_changed(alpacacore::util::HostClockEvent event) {
        driver->on_host_clock_changed(event);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::milliseconds(250));
    }
};

const alpacacore::util::HostClockEvent kStepped{alpacacore::util::HostClockEvent::Kind::Stepped, kRc51Step};
const alpacacore::util::HostClockEvent kBecameTrusted{alpacacore::util::HostClockEvent::Kind::BecameTrusted,
                                                      std::chrono::milliseconds(0)};

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
    alpacacore::test::HostClockTrustGuard trusted(true);  // the connect writes the mount clock (#2419)
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
    alpacacore::test::HostClockTrustGuard trusted(true);  // the connect writes the mount clock (#2419)
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

TEST_CASE("iOptron Telescope Driver - a client UTCDate write after the clock sync's :SUT keeps the anchor",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);  // the connect writes the mount clock (#2419)
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    // The client's write lands after the body's :SUT and before the body publishes its state.
    const auto client_utc = std::chrono::system_clock::time_point(std::chrono::seconds(1900000000));
    auto* raw = driver.get();
    ioptron::set_clock_sync_utc_hook_for_testing(*driver, [raw, client_utc](bool after_send) {
        if (after_send) raw->set_utc_date(client_utc);
    });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    // The body's publication also caches the site latitude; it is the signal that the body has published.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::abs(driver->get_site_latitude() - -33.856784) > 1e-6 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(std::abs(driver->get_site_latitude() - -33.856784) < 1e-6);
    CHECK(mount.count(":SUT") == 2);  // the body's, then the client's
    const auto utc = driver->get_utc_date();
    CHECK(utc >= client_utc);
    CHECK(utc - client_utc < std::chrono::seconds(2));
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - client site writes after the clock sync's site commands keep their values",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);  // the connect writes the mount clock (#2419)
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297, 10.0,
                                                              true, clock);
    // The client's writes land after the body's :SLA/:SLO and before the body publishes its state.
    auto* raw = driver.get();
    ioptron::set_clock_sync_utc_hook_for_testing(*driver, [raw](bool after_send) {
        if (!after_send) return;
        raw->set_site_latitude(51.477833);
        raw->set_site_longitude(-0.001500);
        raw->set_site_elevation(42.0);
    });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    // The body's publication replaces the fake mount's year-2000 UTC with host time: the signal it has published.
    const auto published = [&] {
        try {
            return driver->get_utc_date() > std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
        } catch (const std::exception&) {
            return false;
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!published() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(published());
    CHECK(std::abs(driver->get_site_latitude() - 51.477833) < 1e-6);
    CHECK(std::abs(driver->get_site_longitude() - -0.001500) < 1e-6);
    CHECK(driver->get_site_elevation() == 42.0);
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - a :SUT retry sends the host time of that attempt",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);  // the connect writes the mount clock (#2419)
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    // Attempt 1 fails before the send. Attempt 2 waits until the host clock has moved on by 2 ms, then notes it.
    std::atomic<int> attempts{0};
    std::atomic<long long> floor_ms{0};
    ioptron::set_clock_sync_utc_hook_for_testing(*driver, [&](bool after_send) {
        if (after_send) return;
        if (attempts.fetch_add(1) == 0) throw std::runtime_error("first attempt fails");
        const auto start = std::chrono::system_clock::now();
        while (std::chrono::system_clock::now() - start < std::chrono::milliseconds(2)) {
        }
        floor_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now() -
                           std::chrono::system_clock::time_point(std::chrono::seconds(946728000)))
                           .count());
    });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    const auto first = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (attempts.load() < 1 && std::chrono::steady_clock::now() < first) std::this_thread::yield();
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the retry wait
    clock.advance(std::chrono::milliseconds(750));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (mount.count(":SUT") < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    driver->set_connected(false);
    REQUIRE(mount.count(":SUT") == 1);
    for (const auto& cmd : mount.commands()) {
        if (cmd.rfind(":SUT", 0) != 0) continue;
        CHECK(std::stoll(cmd.substr(4, 13)) >= floor_ms.load());
    }
}

// ── Issue #2419: the mount clock waits for a trusted host clock, and follows it when it steps ────────────────

TEST_CASE("iOptron Telescope Driver - an untrusted host clock leaves the mount clock alone at connect",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    // The site still goes to the mount; no timezone, DST or UTC command does.
    CHECK(rig.mount.count(":SLA") == 1);
    CHECK(rig.mount.count(":SLO") == 1);
    CHECK(rig.mount.count(":SG") == 0);
    CHECK(rig.mount.count(":SDS") == 0);
    CHECK(rig.mount.count(":SUT") == 0);
    CHECK(log.contains("Host clock is not trusted"));
    CHECK(log.contains("2017-01-01T12:00:42Z"));  // the mount clock it leaves alone, by name
    // UTCDate is still the mount's own clock, not the stale host's.
    const auto utc = rig.driver->get_utc_date();
    CHECK(utc >= kMountPowerUp);
    CHECK(utc - kMountPowerUp < std::chrono::seconds(2));
    rig.driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - a host clock step after an untrusted connect re-syncs the mount clock",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    REQUIRE(rig.mount.count(":SUT") == 0);
    const auto connect_commands = rig.mount.commands().size();

    // The router saw the step and latched trust before it called the driver.
    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));

    // :GUT# (the old mount clock), the full body, :SUT with the host time, then :GUT# again (the new one).
    const auto commands = rig.mount.commands();
    std::vector<std::string> resync(commands.begin() + static_cast<std::ptrdiff_t>(connect_commands), commands.end());
    REQUIRE(std::count_if(resync.begin(), resync.end(), [](const auto& c) { return c.rfind(":SUT", 0) == 0; }) == 1);
    const auto sut = std::find_if(resync.begin(), resync.end(), [](const auto& c) { return c.rfind(":SUT", 0) == 0; });
    CHECK(sut_time(*sut) == kRealHost);
    CHECK(std::find(resync.begin(), sut, ":GUT#") != sut);
    CHECK(std::find(sut, resync.end(), ":GUT#") != resync.end());
    CHECK(std::count_if(resync.begin(), resync.end(), [](const auto& c) { return c.rfind(":SG", 0) == 0; }) == 1);
    CHECK(std::count_if(resync.begin(), resync.end(), [](const auto& c) { return c.rfind(":SDS", 0) == 0; }) == 1);
    CHECK(std::count_if(resync.begin(), resync.end(), [](const auto& c) { return c.rfind(":SLA", 0) == 0; }) == 1);
    CHECK(log.contains("Host clock stepped by 24507000 ms; mount UTC 2017-01-01T12:00:42Z -> "));

    // UTCDate now follows the host clock.
    REQUIRE(eventually([&] {
        const auto utc = rig.driver->get_utc_date();
        return utc >= kRealHost && utc - kRealHost < std::chrono::seconds(2);
    }));
    rig.driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - a host clock that becomes trusted with no step re-syncs the mount clock",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    UntrustedConnect rig;
    rig.connect();
    REQUIRE(rig.mount.count(":SUT") == 0);

    // NTP slewed the clock in, or a client confirmed it: trusted, never stepped.
    alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    rig.host_clock_changed(kBecameTrusted);
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    rig.driver->set_connected(false);
    REQUIRE(rig.mount.count(":SUT") == 1);
    for (const auto& command : rig.mount.commands()) {
        if (command.rfind(":SUT", 0) == 0) CHECK(sut_time(command) == kStaleHost);
    }
}

TEST_CASE("iOptron Telescope Driver - a host clock change leaves a mount clock that already agrees alone",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    const auto sla = rig.mount.count(":SLA");
    // Counted from the connect: on 6.0 a connect reads :GUT# for the UTC sample and again for the site (main's
    // #1784 reads it once), so the case checks what the re-sync adds.
    const auto gut = rig.mount.count(":GUT");

    // A client's UTCDate already set the mount (the router's own step): 1.5 s apart is inside the 2 s threshold.
    rig.mount.set_utc_reply(gut_reply_for(kStaleHost + std::chrono::milliseconds(1500)));
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("already agrees, no re-sync"); }));
    rig.driver->set_connected(false);
    CHECK(rig.mount.count(":GUT") == gut + 1);  // the re-sync's check
    CHECK(rig.mount.count(":SLA") == sla);
    CHECK(rig.mount.count(":SUT") == 0);
}

TEST_CASE("iOptron Telescope Driver - a re-sync that supersedes the connect's pass still sends the site",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    // The host clock changes in the first second after Connect, while the connect's body is still in its start
    // delay: the re-sync supersedes it, so it must do the connect's work, even on a mount whose clock agrees.
    alpacacore::test::HostClockTrustGuard trusted(true);
    IoptronLog log;
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_utc_reply(gut_reply_for(kRealHost));
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    ioptron::set_host_utc_source_for_testing(*driver, [] { return kRealHost; });
    std::atomic<int> passes{0};
    ioptron::set_clock_sync_utc_hook_for_testing(*driver, [&](bool after_send) {
        if (after_send) ++passes;
    });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the connect's body, parked in its delay
    driver->on_host_clock_changed(kStepped);
    REQUIRE(clock.drain_until([&] { return passes.load() == 1 || log.contains("already agrees"); },
                              std::chrono::milliseconds(250), std::chrono::seconds(10)));
    driver->set_connected(false);
    CHECK_FALSE(log.contains("already agrees"));
    CHECK(passes.load() == 1);
    CHECK(mount.count(":SLA") == 1);
    CHECK(mount.count(":SLO") == 1);
    CHECK(mount.count(":SUT") == 1);
}

TEST_CASE("iOptron Telescope Driver - a host clock re-sync waits for a MoveAxis to end",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    rig.driver->move_axis(0, rig.driver->get_axis_rate_range(0).first);

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("waits for the motion to end"); }));
    // Parked in its 1 s wait: nothing went to the mount clock while the axis moved.
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    CHECK(rig.mount.count(":SUT") == 0);
    rig.clock.advance(std::chrono::seconds(1));  // still moving: it waits again
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    CHECK(rig.mount.count(":SUT") == 0);

    rig.driver->move_axis(0, 0.0);
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    rig.driver->set_connected(false);
    CHECK(rig.mount.count(":SUT") == 1);
}

// Issue #2432: the motion wait is a check, and a MoveAxis (or a GOTO, Park or FindHome) can start right after it.
// The re-sync's writes check again inside their own exchange, under the operation lock every motion command takes,
// so the clock and the site are not written under that motion: the writes wait for it to end.
TEST_CASE("iOptron Telescope Driver - a MoveAxis that starts after the re-sync's motion wait gets no clock write",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    const auto sla = rig.mount.count(":SLA");
    std::atomic<bool> moved{false};
    // The race window: the mount was still when the re-sync looked, and a client's MoveAxis lands before its writes.
    ioptron::set_clock_sync_resync_hook_for_testing(*rig.driver, [&] {
        if (!moved.exchange(true)) rig.driver->move_axis(0, rig.driver->get_axis_rate_range(0).first);
    });

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("waits for the motion to end") || rig.passes.load() == 2; }));
    CHECK(moved.load());
    CHECK(rig.mount.count(":SUT") == 0);
    CHECK(rig.mount.count(":SG") == 0);
    CHECK(rig.mount.count(":SLA") == sla);

    // The axis stops: the re-sync goes on and writes the clock.
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    rig.driver->move_axis(0, 0.0);
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    rig.driver->set_connected(false);
    CHECK(rig.mount.count(":SUT") == 1);
    CHECK(rig.mount.count(":SLA") == sla + 1);
}

// Issue #2432: a host clock change while set_connected() holds transition_mutex_ (here a redundant Connected=true,
// as ConformU sends) is not lost. The hook leaves it pending, and set_connected() starts the re-sync as it returns.
TEST_CASE("iOptron Telescope Driver - a host clock change during a held set_connected() re-syncs as that call returns",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    alpacacore::test::Latch held;
    ioptron::set_connected_entry_hook_for_testing(*rig.driver, [&] { held.block(); });
    std::thread client([&] { rig.driver->set_connected(true); });
    REQUIRE(held.wait_entered());

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);  // returns at once: transition_mutex_ is held
    CHECK_FALSE(rig.clock.wait_for_waiters(1, std::chrono::milliseconds(300)));  // nothing started yet
    CHECK(rig.mount.count(":SUT") == 0);

    held.release();
    client.join();
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    rig.clock.advance(std::chrono::milliseconds(250));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    CHECK(log.contains("Host clock changed during a connect or disconnect; mount UTC 2017-01-01T12:00:42Z -> "));
    rig.driver->set_connected(false);
    CHECK(rig.mount.count(":SUT") == 1);
}

// Issue #2419: the timer finds transition_mutex_ held, and the holder's whole exit (its pending check and the unlock)
// runs before the timer goes on. The change is stored before the lock is tried, so the holder's drain still starts
// the re-sync; stored after the failed try, it would wait for the next set_connected().
TEST_CASE("iOptron host clock re-sync - a change whose lock try fails as set_connected() returns is not lost",
          "[ioptron][telescope][unit][utcdate][taskclock][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    UntrustedConnect rig;
    rig.connect();
    alpacacore::test::Latch held;
    ioptron::set_connected_entry_hook_for_testing(*rig.driver, [&] { held.block(); });
    std::promise<void> returned;
    auto returned_future = returned.get_future();
    std::thread client([&] {
        rig.driver->set_connected(true);  // a redundant Connected=true, as ConformU sends
        returned.set_value();
    });
    REQUIRE(held.wait_entered());
    std::atomic<bool> busy_seen{false};
    std::atomic<bool> client_returned{false};
    ioptron::set_host_clock_busy_hook_for_testing(*rig.driver, [&] {
        if (busy_seen.exchange(true)) return;
        held.release();  // the holder finishes, drains and lets go before this call returns
        client_returned = returned_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    });

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.driver->on_host_clock_changed(kStepped);
    client.join();
    REQUIRE(busy_seen.load());
    REQUIRE(client_returned.load());

    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the re-sync, parked in its start delay
    rig.clock.advance(std::chrono::milliseconds(250));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    rig.driver->set_connected(false);
    REQUIRE(rig.mount.count(":SUT") == 1);
    for (const auto& command : rig.mount.commands()) {
        if (command.rfind(":SUT", 0) == 0) CHECK(sut_time(command) == kRealHost);
    }
}

// Issue #2419: the change arrives in the last moment of set_connected(), with transition_mutex_ still held but after
// everything else the call did. set_connected() drains the pending change after it lets go of the lock, so a change
// stored then is acted on; a check made while the lock was still held would have missed it.
TEST_CASE("iOptron host clock re-sync - a change stored just before set_connected() lets go is not lost",
          "[ioptron][telescope][unit][utcdate][taskclock][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    UntrustedConnect rig;
    rig.connect();
    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    std::atomic<bool> delivered{false};
    ioptron::set_connected_exit_hook_for_testing(*rig.driver, [&] {
        if (delivered.exchange(true)) return;
        // The timer thread: its lock try fails (this call still holds transition_mutex_), so it leaves the change
        // pending and returns at once.
        std::thread timer([&] { rig.driver->on_host_clock_changed(kStepped); });
        timer.join();
    });
    rig.driver->set_connected(true);  // a redundant Connected=true, as ConformU sends
    REQUIRE(delivered.load());
    ioptron::set_connected_exit_hook_for_testing(*rig.driver, {});

    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the re-sync, parked in its start delay
    rig.clock.advance(std::chrono::milliseconds(250));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    rig.driver->set_connected(false);
    CHECK(rig.mount.count(":SUT") == 1);
}

// Issue #2419: a client's UTCDate write that lands after a re-sync was decided is newer than the host clock the
// re-sync would write. Here it lands while the re-sync waits out a GOTO; once the GOTO ends the re-sync sends its
// site and timezone commands but no :SUT, and UTCDate keeps the client's time.
TEST_CASE("iOptron host clock re-sync - a client UTCDate write while the re-sync waits out a GOTO wins",
          "[ioptron][telescope][unit][utcdate][taskclock][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    const auto sla = rig.mount.count(":SLA");
    rig.driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(eventually([&] { return rig.mount.count(":MS1#") == 1; }));

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("waits for the motion to end"); }));
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));  // parked in its 1 s motion wait

    // An hour from the host clock, so the re-sync's agree check cannot be what keeps it.
    const auto client_utc = kRealHost + std::chrono::hours(1);
    rig.driver->set_utc_date(client_utc);
    REQUIRE(rig.mount.count(":SUT") == 1);

    rig.driver->abort_slew();  // the GOTO ends
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    CHECK(rig.mount.count(":SLA") == sla + 1);  // the re-sync ran its writes
    CHECK(rig.mount.count(":SUT") == 1);        // the client's only
    for (const auto& command : rig.mount.commands()) {
        if (command.rfind(":SUT", 0) == 0) CHECK(sut_time(command) == client_utc);
    }
    CHECK(log.contains("Host clock stepped by 24507000 ms; a client wrote UTCDate since"));
    const auto utc = rig.driver->get_utc_date();
    CHECK(utc >= client_utc);
    CHECK(utc - client_utc < std::chrono::seconds(2));
    rig.driver->set_connected(false);
}

// Issue #2419: here the GOTO starts after the re-sync's first motion wait, just before its UTC write (the seam), so
// the UTC write's own exchange finds it and waits it out, and the client's UTCDate write lands during that wait. The
// write-generation check runs inside that exchange, under the operation lock, so it sees the client's write; a check
// made before the exchange would have passed, and the :SUT would go out over the client's time once the GOTO ended.
TEST_CASE("iOptron host clock re-sync - a client UTCDate write while the re-sync's UTC write waits out a GOTO wins",
          "[ioptron][telescope][unit][utcdate][taskclock][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    const auto sla = rig.mount.count(":SLA");
    std::atomic<bool> goto_started{false};
    ioptron::set_clock_sync_before_utc_exchange_hook_for_testing(*rig.driver, [&] {
        if (goto_started.load()) return;
        rig.driver->slew_to_coordinates_async(13.0, 20.0);
        goto_started = true;
    });

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return goto_started.load(); }));
    REQUIRE(eventually([&] { return log.contains("waits for the motion to end"); }));
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the UTC write, parked in its 1 s motion wait
    CHECK(rig.mount.count(":SLA") == sla + 1);                        // the site went out before the GOTO
    CHECK(rig.mount.count(":SUT") == 0);

    const auto client_utc = kRealHost + std::chrono::hours(1);
    rig.driver->set_utc_date(client_utc);
    REQUIRE(rig.mount.count(":SUT") == 1);

    rig.driver->abort_slew();  // the GOTO ends
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return rig.passes.load() == 2; }));
    CHECK(rig.mount.count(":SUT") == 1);  // the client's only
    for (const auto& command : rig.mount.commands()) {
        if (command.rfind(":SUT", 0) == 0) CHECK(sut_time(command) == client_utc);
    }
    CHECK(log.contains("Host clock stepped by 24507000 ms; a client wrote UTCDate since"));
    const auto utc = rig.driver->get_utc_date();
    CHECK(utc >= client_utc);
    CHECK(utc - client_utc < std::chrono::seconds(2));
    rig.driver->set_connected(false);
}

// Issue #2419: a re-sync waits out driver motion for at most util::kHostClockResyncMotionBound. A MoveAxis held past
// it ends the re-sync with nothing written: the axis stopping afterwards brings no late :SUT, and the mount clock is
// left for the next host clock change.
TEST_CASE("iOptron host clock re-sync - a MoveAxis held past the motion bound ends the re-sync with no clock write",
          "[ioptron][telescope][unit][utcdate][taskclock][clock]") {
    alpacacore::test::HostClockTrustGuard untrusted(false);
    IoptronLog log;
    UntrustedConnect rig;
    rig.connect();
    const auto sla = rig.mount.count(":SLA");
    rig.driver->move_axis(0, rig.driver->get_axis_rate_range(0).first);

    rig.set_host(kRealHost);
    alpacacore::util::HostClockTrust::mark_set("test step");
    rig.host_clock_changed(kStepped);
    REQUIRE(eventually([&] { return log.contains("waits for the motion to end"); }));
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    // One poll short of the bound: it still waits.
    rig.clock.advance(alpacacore::util::kHostClockResyncMotionBound - alpacacore::util::kHostClockResyncMotionPoll);
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    CHECK_FALSE(log.contains("gives up"));

    // At the bound: it gives up and the body ends.
    rig.clock.advance(alpacacore::util::kHostClockResyncMotionPoll);
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    CHECK(log.contains("still moving after 15 min; the mount clock re-sync gives up"));

    // The axis stops later: a body still waiting would now write the clock.
    rig.driver->move_axis(0, 0.0);
    rig.clock.advance(alpacacore::util::kHostClockResyncMotionPoll);
    rig.clock.wait_for_woken_settled(std::chrono::seconds(1));
    rig.driver->set_connected(false);  // joins the clock-sync slot
    CHECK(rig.passes.load() == 1);
    CHECK(rig.mount.count(":SUT") == 0);
    CHECK(rig.mount.count(":SLA") == sla);
}

// Issue #2432: a connect pass that throws before it publishes leaves clock_sync_connect_pass_done_ false, so the
// next host clock re-sync runs as a connect pass (the site goes out again, no agree check). It is still a re-sync: it
// waits for the motion the driver started, and its writes check for motion, so no :SUT goes out under a MoveAxis.
TEST_CASE("iOptron Telescope Driver - a re-sync after a failed connect pass waits for a MoveAxis to end",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    IoptronLog log;
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_utc_reply(gut_reply_for(kMountPowerUp));
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                              std::nullopt, true, clock);
    ioptron::set_host_utc_source_for_testing(*driver, [] { return kRealHost; });
    std::atomic<int> passes{0};
    ioptron::set_clock_sync_utc_hook_for_testing(*driver, [&](bool after_send) {
        if (after_send) ++passes;
    });
    std::atomic<bool> failed_once{false};
    ioptron::set_clock_sync_hook_for_testing(*driver, [&] {
        if (!failed_once.exchange(true)) throw std::runtime_error("connect pass refused");
    });
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(eventually([&] { return ioptron::clock_sync_failure_for_testing(*driver).has_value(); }));
    CHECK(mount.count(":SLA") == 0);
    CHECK(mount.count(":SUT") == 0);

    driver->move_axis(0, driver->get_axis_rate_range(0).first);
    driver->on_host_clock_changed(kStepped);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(eventually([&] { return log.contains("waits for the motion to end") || passes.load() == 1; }));
    // Neither the site nor the clock went out while the axis moved.
    CHECK(passes.load() == 0);
    CHECK(mount.count(":SLA") == 0);
    CHECK(mount.count(":SUT") == 0);

    // The axis stops: the re-sync, parked in its 1 s wait, does the connect's work, the site included, and writes
    // the clock.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    driver->move_axis(0, 0.0);
    clock.advance(std::chrono::seconds(1));
    REQUIRE(eventually([&] { return passes.load() == 1; }));
    driver->set_connected(false);
    CHECK(mount.count(":SLA") == 1);
    CHECK(mount.count(":SUT") == 1);
}

TEST_CASE("iOptron Telescope Driver - a host clock change starts nothing while disconnected or with clock sync off",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    {
        // Never connected.
        alpacacore::test::FakeIoptronMount mount("0036", 0.0);
        REQUIRE(mount.ok());
        alpacacore::test::FakeTaskClock clock;
        auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), -33.856784, 151.215297,
                                                                  std::nullopt, true, clock);
        driver->on_host_clock_changed(kStepped);
        CHECK_FALSE(clock.wait_for_waiters(1, std::chrono::milliseconds(300)));
        CHECK(mount.commands().empty());
    }
    {
        // Connected with syncTimeOnConnect=false.
        alpacacore::test::FakeIoptronMount mount("0036", 0.0);
        REQUIRE(mount.ok());
        alpacacore::test::FakeTaskClock clock;
        auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), std::nullopt, std::nullopt,
                                                                  std::nullopt, false, clock);
        std::atomic<int> passes{0};
        ioptron::set_clock_sync_utc_hook_for_testing(*driver, [&](bool after_send) {
            if (after_send) ++passes;
        });
        driver->set_connected(true);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::milliseconds(250));
        REQUIRE(eventually([&] { return passes.load() == 1; }));
        const auto gut = mount.count(":GUT");  // the connect's reads (two on 6.0, one on main after #1784)
        driver->on_host_clock_changed(kStepped);
        driver->on_host_clock_changed(kBecameTrusted);
        CHECK_FALSE(clock.wait_for_waiters(1, std::chrono::milliseconds(300)));
        driver->set_connected(false);
        CHECK(mount.count(":GUT") == gut);  // no re-sync read
        CHECK(mount.count(":SUT") == 0);
    }
}

TEST_CASE("iOptron Telescope Driver - a host clock change racing a disconnect leaves no clock-sync body behind",
          "[ioptron][telescope][unit][utcdate][taskclock]") {
    alpacacore::test::HostClockTrustGuard trusted(true);
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), std::nullopt, std::nullopt,
                                                              std::nullopt, true, clock);
    for (int round = 0; round < 20; ++round) {
        driver->set_connected(true);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the connect's body, parked in its delay
        std::atomic<bool> stop{false};
        // The timer thread, hammering the hook while the disconnect runs: every start it makes either lands before
        // the disconnect's join (and is joined) or sees the mount disconnected (and starts nothing).
        std::thread timer([&] {
            for (int call = 0; call < 200 && !stop.load(); ++call) driver->on_host_clock_changed(kStepped);
        });
        driver->set_connected(false);
        stop = true;
        timer.join();
        CHECK_FALSE(clock.wait_for_waiters(1, std::chrono::milliseconds(50)));
    }
    CHECK(mount.count(":SUT") == 0);
}

#endif  // _WIN32

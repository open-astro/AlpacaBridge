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

// Bisque getters and DeviceState served from the polled snapshot (decision 0010, Bisque adoption; link-loss policy of
// decision 0009). The driver runs on a FakeTaskClock, so its status poll runs only when a case advances the clock (or
// steps it through the test seam), and the fake TheSkyX answers on the wall clock after an injected latency per
// transaction: a getter that asked TheSkyX would pay it and show in the transaction count.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_bisque_mount.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace bq = alpacacore::vendor::bisque;
using alpacacore::test::FakeTaskClock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kLatency = milliseconds(100);
// A getter that reached TheSkyX would pay kLatency, so the budget only has to sit below it. Sanitized builds run
// several times slower on a loaded runner and get more room; the transaction count is the strict check.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALPACACORE_TESTS_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define ALPACACORE_TESTS_SANITIZED 1
#endif
#endif
#if defined(ALPACACORE_TESTS_SANITIZED)
constexpr auto kGetterBudget = milliseconds(60);
#else
constexpr auto kGetterBudget = milliseconds(20);
#endif

bq::ConnectionInfo loopback(int port, int timeout_ms = 300) {
    bq::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = timeout_ms;
    return info;
}

// A connected driver on a FakeTaskClock over the stateful TheSkyX fake. The poll is paused: a case steps it.
struct Rig {
    alpacacore::test::FakeBisqueMount mount;
    FakeTaskClock clock;
    alpacacore::test::FakeMountServer server{mount.responder()};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit Rig(int timeout_ms = 300) {
        REQUIRE(server.ok());
        driver = bq::create_bisque_telescope(0, loopback(server.port(), timeout_ms), clock);
        REQUIRE_NOTHROW(driver->set_connected(true));
        bq::set_status_poll_paused_for_testing(*driver, true);
    }
    ~Rig() { driver->set_connected(false); }
    bool poll() { return bq::poll_status_now_for_testing(*driver); }
};

bool has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == name) return true;
    }
    return false;
}

using alpacacore::test::parse_timestamp;
using alpacacore::test::time_stamp_of;
using alpacacore::test::timed;

}  // namespace

TEST_CASE("Bisque UTCDate - remote clock is seeded and advances without getter I/O",
          "[bisque][telescope][snapshot][utcdate]") {
    Rig rig;
    const auto remote = std::chrono::system_clock::time_point(milliseconds(946684800123));
    REQUIRE(rig.mount.utc_reads == 1);
    CHECK(rig.driver->get_utc_date() == remote);
    const int before = rig.mount.transactions;
    rig.clock.advance(milliseconds(5500));
    CHECK(rig.driver->get_utc_date() == remote + milliseconds(5500));
    CHECK(rig.mount.transactions == before);
    CHECK(has_entry(*rig.driver, "UTCDate"));

    rig.mount.utc_millis = 978307199999;  // Remote clock step to 2000-12-31 23:59:59.999 UTC.
    REQUIRE(rig.poll());
    rig.clock.advance(milliseconds(2));
    CHECK(rig.driver->get_utc_date() == std::chrono::system_clock::time_point(milliseconds(978307200001)));
}

TEST_CASE("Bisque UTCDate - unavailable mode clears time across polls and reconnects",
          "[bisque][telescope][snapshot][utcdate]") {
    using alpacacore::test::require_alpaca_error;
    Rig rig;
    rig.mount.use_computer_clock = false;
    REQUIRE(rig.poll());
    const int before = rig.mount.transactions;
    require_alpaca_error([&] { (void)rig.driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(has_entry(*rig.driver, "UTCDate"));
    CHECK(rig.driver->get_right_ascension() == 5.5);
    CHECK(rig.driver->get_tracking());
    CHECK(rig.mount.transactions == before);

    rig.driver->set_connected(false);
    require_alpaca_error([&] { (void)rig.driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { rig.driver->set_utc_date({}); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE_NOTHROW(rig.driver->set_connected(true));
    require_alpaca_error([&] { (void)rig.driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    rig.mount.use_computer_clock = true;
    rig.mount.utc_millis = 978307200456;
    REQUIRE(rig.poll());
    CHECK(rig.driver->get_utc_date() == std::chrono::system_clock::time_point(milliseconds(978307200456)));
    rig.driver->set_connected(false);
    rig.mount.reject_connection = true;
    CHECK_THROWS_AS(rig.driver->set_connected(true), alpacacore::AlpacaException);
    require_alpaca_error([&] { (void)rig.driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
    rig.mount.reject_connection = false;
    rig.mount.use_computer_clock = false;
    rig.driver->set_connected(true);
    require_alpaca_error([&] { (void)rig.driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE("Bisque UTCDate - bad remote replies retain errors and a good poll recovers",
          "[bisque][telescope][snapshot][utcdate]") {
    using alpacacore::test::require_alpaca_error;
    alpacacore::test::FakeBisqueMount mount;
    FakeTaskClock clock;
    const std::string bad[] = {"",      "OK",     "946684800123junk",    "NaN",           "Infinity",
                               "1.5",   "1e12",   "9223372036854775808", "9223372036855", "-9223372036855",
                               "error", "timeout"};
    std::atomic<int> reply_index{-1};
    alpacacore::test::FakeMountServer server([&](const std::string& command) {
        const auto normal = mount.responder()(command);
        const int index = reply_index;
        if (command.find("new Date().getTime()") != std::string::npos && index >= 0) {
            if (bad[index] == "error") return std::string("|Time query failed. Error = 1.#");
            if (bad[index] == "timeout") return std::string{};
            return "|No error. Error = 0." + bad[index] + "#";
        }
        return normal;
    });
    REQUIRE(server.ok());
    auto driver = bq::create_bisque_telescope(0, loopback(server.port()), clock);
    REQUIRE_NOTHROW(driver->set_connected(true));
    bq::set_status_poll_paused_for_testing(*driver, true);
    for (int i = 0; i < static_cast<int>(std::size(bad)); ++i) {
        CAPTURE(bad[i]);
        driver->set_connected(false);
        reply_index = i;
        REQUIRE_NOTHROW(driver->set_connected(true));
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::DriverException);
        CHECK(driver->get_connected());
        CHECK(driver->get_right_ascension() == 5.5);
        CHECK_FALSE(has_entry(*driver, "UTCDate"));
        reply_index = -1;
        REQUIRE(bq::poll_status_now_for_testing(*driver));
        const auto anchor = std::chrono::system_clock::time_point(milliseconds(mount.utc_millis));
        CHECK(driver->get_utc_date() == anchor);
        clock.advance(milliseconds(1500));
        reply_index = i;
        REQUIRE(bq::poll_status_now_for_testing(*driver));
        CHECK(driver->get_utc_date() == anchor + milliseconds(1500));
        CHECK(has_entry(*driver, "UTCDate"));
        reply_index = -1;
        REQUIRE(bq::poll_status_now_for_testing(*driver));
        CHECK(driver->get_utc_date() == anchor);
    }
}

TEST_CASE("Bisque UTCDate - elapsed time cannot overflow a remote timestamp at the clock limit",
          "[bisque][telescope][snapshot][utcdate]") {
    Rig rig;
    rig.mount.utc_millis = std::chrono::duration_cast<milliseconds>(std::chrono::system_clock::duration::max()).count();
    REQUIRE(rig.poll());
    CHECK_NOTHROW(rig.driver->get_utc_date());
    rig.clock.advance(milliseconds(1));
    alpacacore::test::require_alpaca_error([&] { (void)rig.driver->get_utc_date(); },
                                           alpacacore::AlpacaError::DriverException);
}

TEST_CASE("Bisque UTCDate - elapsed time starts at the time reply before remaining telemetry",
          "[bisque][telescope][snapshot][utcdate]") {
    alpacacore::test::FakeBisqueMount mount;
    FakeTaskClock clock;
    alpacacore::test::FakeMountServer server([&](const std::string& command) {
        if (command.find("GetAzAlt") != std::string::npos) clock.advance(seconds(3));
        return mount.responder()(command);
    });
    REQUIRE(server.ok());
    auto driver = bq::create_bisque_telescope(0, loopback(server.port()), clock);
    REQUIRE_NOTHROW(driver->set_connected(true));
    bq::set_status_poll_paused_for_testing(*driver, true);
    CHECK(driver->get_utc_date() == std::chrono::system_clock::time_point(milliseconds(mount.utc_millis)) + seconds(3));
}

TEST_CASE("Bisque UTCDate - held time reply cannot block getters or overwrite a newer transition",
          "[bisque][telescope][snapshot][utcdate]") {
    using alpacacore::test::require_alpaca_error;
    alpacacore::test::FakeBisqueMount mount;
    FakeTaskClock clock;
    std::atomic<bool> hold{false};
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto released = release.get_future().share();
    alpacacore::test::FakeMountServer server([&](const std::string& command) {
        const auto reply = mount.responder()(command);
        if (command.find("new Date().getTime()") != std::string::npos && hold.exchange(false)) {
            entered.set_value();
            released.wait_for(seconds(3));  // Bound teardown even if an assertion fails.
        }
        return reply;
    });
    REQUIRE(server.ok());
    auto driver = bq::create_bisque_telescope(0, loopback(server.port(), 5000), clock);
    driver->set_connected(true);
    bq::set_status_poll_paused_for_testing(*driver, true);
    const auto original = driver->get_utc_date();
    driver->slew_to_coordinates_async(6.0, 10.0);
    mount.utc_millis = 978307200456;
    hold = true;
    auto poll = std::async(std::launch::async, [&] { return bq::poll_status_now_for_testing(*driver); });
    REQUIRE(entered_future.wait_for(seconds(2)) == std::future_status::ready);
    CHECK(timed([&] { CHECK(driver->get_utc_date() == original); }) < milliseconds(500));
    SECTION("abort invalidates the in-flight sample") {
        auto abort = std::async(std::launch::async, [&] { driver->abort_slew(); });
        const auto deadline = std::chrono::steady_clock::now() + seconds(2);
        while (driver->get_slewing() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        CHECK_FALSE(driver->get_slewing());
        release.set_value();
        poll.get();
        abort.get();
        CHECK(driver->get_utc_date() == original);
        return;
    }
    SECTION("disconnect discards the sample before an unavailable reconnect") {
        auto disconnect = std::async(std::launch::async, [&] { driver->set_connected(false); });
        const auto deadline = std::chrono::steady_clock::now() + seconds(2);
        while (driver->get_connected() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        CHECK_FALSE(driver->get_connected());
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
        release.set_value();
        poll.get();
        disconnect.get();
        mount.use_computer_clock = false;
        driver->set_connected(true);
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    }
}

TEST_CASE("Bisque snapshot - getters and DeviceState make no TheSkyX transaction at 100 ms per transaction",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    rig.mount.latency_ms = static_cast<int>(kLatency.count());
    const int before = rig.mount.transactions.load();

    const std::pair<const char*, std::function<void()>> reads[] = {
        {"Altitude", [&] { (void)rig.driver->get_altitude(); }},
        {"Azimuth", [&] { (void)rig.driver->get_azimuth(); }},
        {"RightAscension", [&] { (void)rig.driver->get_right_ascension(); }},
        {"Declination", [&] { (void)rig.driver->get_declination(); }},
        {"Tracking", [&] { (void)rig.driver->get_tracking(); }},
        {"SideOfPier", [&] { (void)rig.driver->get_side_of_pier(); }},
        {"Slewing", [&] { (void)rig.driver->get_slewing(); }},
        {"AtPark", [&] { (void)rig.driver->get_at_park(); }},
        {"AtHome", [&] { (void)rig.driver->get_at_home(); }},
        {"IsPulseGuiding", [&] { (void)rig.driver->get_is_pulse_guiding(); }},
        {"UTCDate", [&] { (void)rig.driver->get_utc_date(); }},
        {"DeviceState", [&] { (void)rig.driver->get_device_state(); }},
    };
    for (const auto& [name, read] : reads) {
        CAPTURE(name);
        CHECK(timed(read) < kGetterBudget);
    }
    CHECK(rig.mount.transactions.load() == before);
    // DeviceState drops a getter that throws, so a read that quietly failed would still be fast: name what must be in
    // it.
    for (const char* name :
         {"RightAscension", "Declination", "Altitude", "Azimuth", "SideOfPier", "Slewing", "Tracking", "TimeStamp"}) {
        CAPTURE(name);
        CHECK(has_entry(*rig.driver, name));
    }
}

TEST_CASE("Bisque snapshot - a setter's value is read back at once and a stale poll does not undo it",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.driver->get_tracking());

    // Tracking: TheSkyX still answers 1 (the fake ignores the command), so a read that reached it would say true.
    rig.driver->set_tracking(false);
    CHECK_FALSE(rig.driver->get_tracking());
    rig.driver->set_tracking(true);
    CHECK(rig.driver->get_tracking());

    // A poll that sampled before the write must not replace it: the write lands while the tick is between exchanges.
    rig.mount.latency_ms = static_cast<int>(kLatency.count());
    const int before = rig.mount.transactions.load();
    std::thread tick([&] { rig.poll(); });
    while (rig.mount.transactions.load() == before) std::this_thread::sleep_for(milliseconds(1));
    rig.driver->set_tracking(false);
    tick.join();
    CHECK_FALSE(rig.driver->get_tracking());
    rig.mount.latency_ms = 0;

    // The next tick measures the mount again.
    CHECK(rig.poll());
    CHECK(rig.driver->get_tracking());

    // The other values ConformU writes and reads back are driver state, served without TheSkyX: read back at once.
    rig.driver->set_target_right_ascension(7.25);
    rig.driver->set_target_declination(-12.5);
    CHECK(rig.driver->get_target_right_ascension() == 7.25);
    CHECK(rig.driver->get_target_declination() == -12.5);
    alpacacore::GuideRate rate{};
    rate.ra = 0.002;
    rate.dec = 0.001;
    rig.driver->set_guide_rate(rate);
    CHECK(rig.driver->get_guide_rate().ra == 0.002);
    CHECK(rig.driver->get_guide_rate().dec == 0.001);
    rig.driver->set_slew_settle_time(3);
    CHECK(rig.driver->get_slew_settle_time() == 3);
    rig.driver->set_tracking_rate(0);
    CHECK(rig.driver->get_tracking_rate() == 0);
    rig.driver->set_site_latitude(-33.856784);
    rig.driver->set_site_longitude(151.215297);
    rig.driver->set_site_elevation(42.0);
    CHECK(rig.driver->get_site_latitude() == -33.856784);
    CHECK(rig.driver->get_site_longitude() == 151.215297);
    CHECK(rig.driver->get_site_elevation() == 42.0);
}

TEST_CASE("Bisque snapshot - a sync, a slew start and an abort are read back at once",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    rig.driver->sync_to_coordinates(9.0, -30.0);
    CHECK(rig.driver->get_right_ascension() == 9.0);
    CHECK(rig.driver->get_declination() == -30.0);

    rig.mount.slew_polls_needed = 100000;
    rig.driver->slew_to_coordinates_async(5.0, 10.0);
    CHECK(rig.driver->get_slewing());
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("Bisque snapshot - an async slew ends with Slewing false and the landed position in one step",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    rig.mount.land_on_target = true;
    rig.mount.slew_polls_needed = 0;
    rig.driver->slew_to_coordinates_async(5.0, 10.0);
    rig.clock.advance(seconds(9));  // past the 8 s force window

    // While the window held, no frame measured the landing: the position is the old one and Slewing is true.
    CHECK(rig.driver->get_right_ascension() == 5.5);
    CHECK(rig.driver->get_slewing());

    CHECK(rig.poll());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_right_ascension() == 5.0);
    CHECK(rig.driver->get_declination() == 10.0);
}

TEST_CASE("Bisque snapshot - DeviceState TimeStamp is when the poll measured, not when it was asked",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    auto stamp = parse_timestamp(time_stamp_of(*rig.driver));
    CHECK(std::abs(std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count()) < 1.0);

    // The poll is paused while the task clock moves on 5 s (inside the 30 s bound, no fault latched): the stamp keeps
    // the age of the frame.
    rig.clock.advance(seconds(5));
    REQUIRE(rig.driver->get_connected());
    stamp = parse_timestamp(time_stamp_of(*rig.driver));
    const double age = std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count();
    CHECK(age > 4.5);
    CHECK(age < 5.5);
}

TEST_CASE("Bisque snapshot - past the staleness bound DeviceState is empty with no TimeStamp and a relink recovers",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    CHECK(has_entry(*rig.driver, "TimeStamp"));

    rig.mount.mute = true;
    for (int i = 0; i < 3; ++i) CHECK(rig.poll());
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_connected());  // a young fault keeps Connected
    alpacacore::test::require_alpaca_error([&] { (void)rig.driver->get_utc_date(); },
                                           alpacacore::AlpacaError::DriverException);
    alpacacore::test::require_alpaca_error([&] { rig.driver->set_utc_date({}); },
                                           alpacacore::AlpacaError::DriverException);
    try {
        (void)rig.driver->get_right_ascension();
        FAIL("a faulted link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()).find("communications compromised") != std::string::npos);
    }

    rig.clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    CHECK_FALSE(rig.poll());  // the poll body ends: the link is lost
    CHECK_FALSE(rig.driver->get_connected());
    alpacacore::test::require_alpaca_error([&] { (void)rig.driver->get_utc_date(); },
                                           alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(
        rig.driver->get_link_fault().empty());  // decision 0009 point 5: the text stays until Connect/Disconnect
    CHECK(rig.driver->get_device_state().empty());
    CHECK_FALSE(has_entry(*rig.driver, "TimeStamp"));
    try {
        (void)rig.driver->get_right_ascension();
        FAIL("a lost link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    rig.mount.mute = false;

    // Decision 0009 point 4: Connected=true against a lost link relinks; it is not the idempotent no-op.
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_right_ascension() == 5.5);
    CHECK(rig.driver->get_link_fault().empty());
    bq::set_status_poll_paused_for_testing(*rig.driver, true);
    CHECK(rig.driver->get_utc_date() == std::chrono::system_clock::time_point(milliseconds(rig.mount.utc_millis)));

    // The async route relinks too: it is not the idempotent no-op either.
    rig.mount.mute = true;
    for (int i = 0; i < 3; ++i) rig.poll();
    rig.clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    rig.poll();
    REQUIRE_FALSE(rig.driver->get_connected());
    rig.mount.mute = false;
    rig.driver->connect();
    CHECK(alpacacore::test::settle_connected(*rig.driver, true, seconds(10)));
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_right_ascension() == 5.5);
    bq::set_status_poll_paused_for_testing(*rig.driver, true);
}

TEST_CASE("Bisque snapshot - a reply clears a latched fault without a reconnect", "[bisque][telescope][snapshot]") {
    Rig rig;
    rig.mount.mute = true;
    for (int i = 0; i < 3; ++i) rig.poll();
    REQUIRE_FALSE(rig.driver->get_link_fault().empty());
    rig.mount.mute = false;
    rig.mount.ra_hours = 6.5;
    CHECK(rig.poll());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_right_ascension() == 6.5);
}

TEST_CASE("Bisque snapshot - the slew Tracking gate falls back to the frame when the live read fails",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.poll());
    REQUIRE(rig.driver->get_tracking());
    rig.mount.fail_tracking_reads = 1;
    // The live IsTracking read errors; the frame says Tracking is on, as the Tracking getter does, so no refusal.
    CHECK_NOTHROW(rig.driver->slew_to_coordinates_async(12.0, 10.0));
    CHECK(rig.mount.fail_tracking_reads.load() == 0);
    CHECK(rig.mount.slew_commands.load() == 1);
}

// Falsified by: bisque_telescope_driver.cpp, the pulse-guide body: replace publish_measured_locked(token, measured)
// with drop_poll_in_flight_locked() after a successful pulse.
TEST_CASE("Bisque snapshot - the position after a pulse guide is measured at pulse end, not a poll later",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.poll());
    REQUIRE(rig.driver->get_right_ascension() == 5.5);
    rig.mount.guide_ra_step_hours = 0.25;
    rig.driver->pulse_guide(2, 100);
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.driver->get_is_pulse_guiding() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE_FALSE(rig.driver->get_is_pulse_guiding());
    // No poll ran: the frame holds the position the pulse ended at.
    CHECK(rig.driver->get_right_ascension() == 5.75);
}

// Falsified by: bisque_telescope_driver.cpp, the pulse-guide body: call measure_frame() with mutex_ held, so the
// four sample exchanges block every getter.
TEST_CASE("Bisque snapshot - a getter is not blocked by the position measurement at pulse end",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.poll());
    rig.mount.latency_ms = static_cast<int>(kLatency.count());
    rig.mount.guide_ra_step_hours = 0.25;
    const int before = rig.mount.transactions.load();
    rig.driver->pulse_guide(2, 100);
    // The guide is one transaction; the next one is the first exchange of the measurement.
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + 2);
    CHECK(timed([&] { (void)rig.driver->get_is_pulse_guiding(); }) < kGetterBudget);
    CHECK(timed([&] { (void)rig.driver->get_right_ascension(); }) < kGetterBudget);
    const auto done = std::chrono::steady_clock::now() + seconds(5);
    while (rig.driver->get_is_pulse_guiding() && std::chrono::steady_clock::now() < done) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(rig.driver->get_right_ascension() == 5.75);
}

// Falsified by: bisque_telescope_driver.cpp, get_tracking_locked: remove the snapshot_.write on the success branch.
TEST_CASE("Bisque snapshot - the slew Tracking gate writes a successful live read through to the frame",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.poll());
    REQUIRE(rig.driver->get_tracking());
    rig.mount.tracking = false;  // the poll has not run since
    try {
        rig.driver->slew_to_coordinates_async(12.0, 10.0);
        FAIL("a slew with Tracking off must be refused");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
    // The refused slew read Tracking live; the getter agrees without a poll.
    CHECK_FALSE(rig.driver->get_tracking());
}

// Falsified by: bisque_telescope_driver.cpp, own_slew_in_flight_locked: call publish_measurement_locked() instead of
// drop_poll_in_flight_locked() once IsSlewComplete answers true.
TEST_CASE("Bisque snapshot - a retarget after a finished slew makes no measurement exchanges in the initiator",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    rig.mount.slew_polls_needed = 0;
    rig.driver->slew_to_coordinates_async(5.0, 10.0);
    rig.clock.advance(seconds(9));  // past the 8 s force window, no poll tick ran
    rig.mount.latency_ms = static_cast<int>(kLatency.count());
    const int reads_before = rig.mount.position_reads.load();
    const int transactions_before = rig.mount.transactions.load();
    rig.driver->slew_to_coordinates_async(6.0, 11.0);
    CHECK(rig.mount.position_reads.load() == reads_before);
    // IsSlewComplete, IsTracking and the slew command only.
    CHECK(rig.mount.transactions.load() - transactions_before <= 3);
    CHECK(rig.driver->get_slewing());
}

// Runs `body` on another thread and waits until the fake has started answering the exchange `started` counts, so the
// case times its getters while the driver is parked in TheSkyX.
template <typename Body>
std::thread run_while_parked(alpacacore::test::FakeBisqueMount& mount, Body body) {
    const int before = mount.transactions.load();
    std::thread worker(std::move(body));
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (mount.transactions.load() < before + 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    return worker;
}

// Falsified by: bisque_telescope_driver.cpp, find_home: hold mutex_ across protocol.find_home().
TEST_CASE("Bisque snapshot - Name, Connected and RightAscension answer while FindHome runs in TheSkyX",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.find_home_ms = 1500;
    auto worker = run_while_parked(rig.mount, [&] { rig.driver->find_home(); });
    const auto budget = milliseconds(500);
    CHECK(timed([&] { (void)rig.driver->get_name(); }) < budget);
    CHECK(timed([&] { (void)rig.driver->get_connected(); }) < budget);
    CHECK(timed([&] { (void)rig.driver->get_right_ascension(); }) < budget);
    worker.join();
    // The control path read the mount after FindHome: a read right after sees it.
    CHECK(rig.driver->get_at_home());
}

// Falsified by: bisque_telescope_driver.cpp, sync_to_coordinates: call publish_measurement_locked() with mutex_ held.
TEST_CASE("Bisque snapshot - RightAscension answers while the read at the end of SyncToCoordinates is slow",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.position_ms = 1000;
    std::thread worker([&] { rig.driver->sync_to_coordinates(7.25, 15.0); });
    // The Sync exchange is fast; the position read that follows is the slow one.
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.ra_hours.load() != 7.25 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(timed([&] { (void)rig.driver->get_right_ascension(); }) < milliseconds(500));
    worker.join();
    CHECK(rig.driver->get_right_ascension() == 7.25);
}

// Falsified by: bisque_telescope_driver.cpp, set_tracking: hold mutex_ across protocol.set_tracking().
TEST_CASE("Bisque snapshot - Name, Connected and Tracking answer while a SetTracking exchange is slow",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.set_tracking_ms = 1000;
    auto worker = run_while_parked(rig.mount, [&] { rig.driver->set_tracking(false); });
    CHECK(timed([&] { (void)rig.driver->get_name(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_connected(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_tracking(); }) < milliseconds(500));
    worker.join();
}

namespace {

// Starts `initiator` on a thread, waits until its TheSkyX command (the `exchanges`-th transaction after the call) is
// on the wire, calls AbortSlew, then checks that Slewing is false: the abort reached TheSkyX after the command, so the
// mount is stopped and the driver must say so. The fake counts a transaction when the command arrives, before its
// 300 ms latency, so the abort starts while the initiator is still inside that exchange.
void abort_during_initiator_exchange(int exchanges,
                                     const std::function<void(alpacacore::TelescopeDriver&)>& initiator) {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.latency_ms = 300;
    rig.mount.slew_polls_needed = 100000;
    const int before = rig.mount.transactions.load();
    std::thread worker([&] { initiator(*rig.driver); });
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + exchanges && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + exchanges);
    rig.driver->abort_slew();
    worker.join();
    CHECK_FALSE(rig.driver->get_slewing());
}

}  // namespace

// Falsified by: bisque_telescope_driver.cpp, move_axis: drop initiator_mutex_ so the start exchange runs outside it.
TEST_CASE("Bisque snapshot - AbortSlew during the MoveAxis start exchange leaves Slewing false",
          "[bisque][telescope][snapshot]") {
    abort_during_initiator_exchange(1, [](alpacacore::TelescopeDriver& d) { d.move_axis(0, 0.5); });
}

// Falsified by: bisque_telescope_driver.cpp, slew_to_coordinates_async: drop initiator_mutex_ so the slew command
// and note_slew_started_locked() run outside it.
TEST_CASE("Bisque snapshot - AbortSlew during the SlewToCoordinatesAsync exchange leaves Slewing false",
          "[bisque][telescope][snapshot]") {
    abort_during_initiator_exchange(2, [](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(5.0, 10.0); });
}

// Falsified by: bisque_telescope_driver.cpp, wait_for_slew_complete: clear slewing_cached_ without the
// `epoch == slew_epoch_` check.
TEST_CASE("Bisque snapshot - a SlewToCoordinates that completes does not clear the Slewing of a retarget",
          "[bisque][telescope][snapshot]") {
    Rig rig;
    rig.mount.slew_polls_needed = 0;  // the first IsSlewComplete of the sync slew answers complete
    bool retargeted = false;
    // The seam runs on the slewing thread after the completion answer and the position read, before the wait applies
    // Slewing false: the window a second client's retarget can land in.
    bq::set_slew_complete_hook_for_testing(*rig.driver, [&] {
        if (retargeted) return;
        retargeted = true;
        rig.mount.slew_polls_needed = 100000;
        rig.driver->slew_to_coordinates_async(6.0, 11.0);
    });
    rig.driver->slew_to_coordinates(5.0, 10.0);
    bq::set_slew_complete_hook_for_testing(*rig.driver, {});
    REQUIRE(retargeted);
    REQUIRE(rig.mount.slew_commands.load() == 2);
    REQUIRE(rig.mount.slewing.load());
    // The completion was about the first slew; the retarget is still in flight.
    CHECK(rig.driver->get_slewing());
}

// Falsified by: bisque_telescope_driver.cpp, set_connected: move lock.unlock() below the seed sample_frame(), so
// mutex_ is held across the handshake, IsParked and the seed.
TEST_CASE("Bisque snapshot - Name and Connected answer while the connect waits on IsParked",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    rig.driver->set_connected(false);
    rig.mount.is_parked_ms = 1500;
    const int before = rig.mount.transactions.load();
    std::thread worker([&] { rig.driver->set_connected(true); });
    // The handshake is one transaction; the next one is the IsParked query the fake holds.
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + 2);
    CHECK(timed([&] { (void)rig.driver->get_name(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_connected(); }) < milliseconds(500));
    worker.join();
    CHECK(rig.driver->get_connected());
}

// Falsified by: bisque_telescope_driver.cpp, park: hold mutex_ across protocol.is_parked() in the poll loop.
TEST_CASE("Bisque snapshot - Connected and AtPark answer while a Park polls IsParked",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.is_parked_ms = 1500;
    const int before = rig.mount.transactions.load();
    std::thread worker([&] { rig.driver->park(); });
    // The park command is one transaction; the next one is the first IsParked poll the fake holds.
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + 2);
    CHECK(timed([&] { (void)rig.driver->get_connected(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_at_park(); }) < milliseconds(500));
    worker.join();
    CHECK(rig.driver->get_at_park());
}

// Falsified by: bisque_telescope_driver.cpp, set_connected(false): call protocol.disconnect() before lock.unlock(), so
// mutex_ is held while the close waits for the guide reply on the wrapper's lock.
TEST_CASE("Bisque snapshot - Connected answers while a disconnect waits for a PulseGuide reply",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.latency_ms = 1500;
    const int before = rig.mount.transactions.load();
    rig.driver->pulse_guide(2, 100);
    const auto on_wire = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + 1 && std::chrono::steady_clock::now() < on_wire) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + 1);
    std::thread worker([&] { rig.driver->set_connected(false); });
    // Connected reads true until the disconnect clears the state, then false; no read waits for the guide reply.
    bool connected = true;
    auto slowest = std::chrono::microseconds(0);
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (connected && std::chrono::steady_clock::now() < deadline) {
        slowest = std::max(slowest, timed([&] { connected = rig.driver->get_connected(); }));
    }
    worker.join();
    CHECK_FALSE(connected);
    CHECK(slowest < milliseconds(500));
}

// Falsified by: bisque_telescope_driver.cpp, wait_for_slew_complete: hold mutex_ across
// BisqueProtocolWrapper::instance().is_slew_complete().
TEST_CASE("Bisque snapshot - RightAscension, Slewing and Connected answer while SlewToCoordinates polls IsSlewComplete",
          "[bisque][telescope][snapshot]") {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.slew_polls_needed = 0;  // the first poll answers complete, so the wait sleeps on no task clock
    rig.mount.slew_complete_ms = 1500;
    const int before = rig.mount.transactions.load();
    std::thread worker([&] { rig.driver->slew_to_coordinates(5.0, 10.0); });
    // IsTracking and the slew command come first; the third transaction is the IsSlewComplete poll the fake holds.
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + 3);
    CHECK(timed([&] { (void)rig.driver->get_right_ascension(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_slewing(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_connected(); }) < milliseconds(500));
    worker.join();
    CHECK_FALSE(rig.driver->get_slewing());
}

namespace {

// Starts `initiator` on a thread with every TheSkyX reply held 1 s, waits until its `exchanges`-th transaction is on
// the wire, and times Connected and Slewing against it.
void getters_answer_during_exchange(int exchanges, const std::function<void(alpacacore::TelescopeDriver&)>& initiator) {
    Rig rig(5000);
    REQUIRE(rig.poll());
    rig.mount.latency_ms = 1000;
    rig.mount.slew_polls_needed = 100000;
    const int before = rig.mount.transactions.load();
    std::thread worker([&] { initiator(*rig.driver); });
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (rig.mount.transactions.load() < before + exchanges && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(rig.mount.transactions.load() >= before + exchanges);
    CHECK(timed([&] { (void)rig.driver->get_connected(); }) < milliseconds(500));
    CHECK(timed([&] { (void)rig.driver->get_slewing(); }) < milliseconds(500));
    worker.join();
}

}  // namespace

// Falsified by: bisque_telescope_driver.cpp, move_axis: hold mutex_ across protocol.start_open_loop_motion().
TEST_CASE("Bisque snapshot - Connected and Slewing answer while the MoveAxis start exchange is slow",
          "[bisque][telescope][snapshot]") {
    getters_answer_during_exchange(1, [](alpacacore::TelescopeDriver& d) { d.move_axis(0, 0.5); });
}

// Falsified by: bisque_telescope_driver.cpp, slew_to_coordinates_async: hold mutex_ across protocol.slew_to_ra_dec().
TEST_CASE("Bisque snapshot - Connected and Slewing answer while the SlewToCoordinatesAsync command is slow",
          "[bisque][telescope][snapshot]") {
    getters_answer_during_exchange(2, [](alpacacore::TelescopeDriver& d) { d.slew_to_coordinates_async(5.0, 10.0); });
}

// Falsified by: bisque_telescope_driver.cpp, unpark: hold mutex_ across protocol.unpark().
TEST_CASE("Bisque snapshot - Connected and Slewing answer while the Unpark command is slow",
          "[bisque][telescope][snapshot]") {
    getters_answer_during_exchange(1, [](alpacacore::TelescopeDriver& d) { d.unpark(); });
}

// Falsified by: bisque_telescope_driver.cpp, set_park: hold mutex_ across protocol.set_park_position().
TEST_CASE("Bisque snapshot - Connected and Slewing answer while the SetPark command is slow",
          "[bisque][telescope][snapshot]") {
    getters_answer_during_exchange(1, [](alpacacore::TelescopeDriver& d) { d.set_park(); });
}

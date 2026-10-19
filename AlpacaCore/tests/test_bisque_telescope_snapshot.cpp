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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include "catch2_compat.h"
#include "concurrency_stress.h"
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

    Rig() {
        REQUIRE(server.ok());
        driver = bq::create_bisque_telescope(0, loopback(server.port()), clock);
        REQUIRE_NOTHROW(driver->set_connected(true));
        bq::set_status_poll_paused_for_testing(*driver, true);
    }
    ~Rig() { driver->set_connected(false); }
    bool poll() { return bq::poll_status_now_for_testing(*driver); }
};

template <typename Fn>
std::chrono::microseconds timed(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
}

bool has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == name) return true;
    }
    return false;
}

std::string time_stamp_of(const alpacacore::TelescopeDriver& driver) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == "TimeStamp") return std::get<std::string>(entry.value);
    }
    return {};
}

std::chrono::system_clock::time_point parse_timestamp(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    REQUIRE(std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi, &s, &ms) == 7);
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
}

}  // namespace

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
    rig.driver->set_does_refraction(true);
    CHECK(rig.driver->get_does_refraction());
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

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

// OnStep getters and DeviceState served from the polled snapshot (decision 0010, OnStep adoption; link-loss policy
// of decision 0009; issue #1783). The driver runs on a FakeTaskClock, so its status poll ticks only when a case
// advances the clock or calls poll_status_now_for_testing(), and the fake mount counts every exchange: a getter
// that asked the mount would show in the count (and pay the injected latency).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace onstep = alpacacore::vendor::onstep;

namespace {

using alpacacore::test::FakeTaskClock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kLatency = milliseconds(100);

// An OnStepX that answers what the driver asks, idle and tracking unless told otherwise, and counts every exchange.
struct SnapshotMount {
    std::atomic<int> exchanges{0};
    std::atomic<int> latency_ms{0};
    std::atomic<bool> mute{false};
    std::atomic<bool> garble_site{false};
    std::atomic<int> ra_seconds{0};            // :GRa# answers 12:00:<ra_seconds>
    std::atomic<bool> refuse_park{false};      // :hP# answers 0 and :GE# 22, "mount still in motion"
    std::atomic<bool> garble_position{false};  // :GRa# answers something that does not parse
    std::atomic<bool> garble_altaz{false};     // :GA# answers something that does not parse
    std::atomic<bool> block_status{false};     // :GU# holds its reply until this is cleared
    std::atomic<bool> handshake_only{false};   // answers the handshake and nothing else (not :GU#, position, Alt/Az)
    std::atomic<bool> silent_status{false};    // :GU# gets no reply, so its read times out
    std::atomic<bool> overlong_goto{false};    // :MS# answers more than one reply may hold, so its send throws
    std::atomic<int> alt_seconds{0};           // :GA# answers +45*00:<alt_seconds>
    std::mutex mutex;
    std::string status = "NpEW180#";  // tracking, no GOTO, not parked (under `mutex`)
    std::vector<std::string> log;     // under `mutex`
    // Runs on the server thread for each command before it is answered (under `mutex` to set, called outside it).
    std::function<void(const std::string&)> on_command;

    void set_on_command(std::function<void(const std::string&)> hook) {
        std::lock_guard<std::mutex> lock(mutex);
        on_command = std::move(hook);
    }
    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
    }
    std::size_t count(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(log.begin(), log.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }

    std::string answer(const std::string& chunk) {
        if (chunk.rfind(":GVP", 0) == 0) return "OnStep#";
        if (chunk.rfind(":GVN", 0) == 0) return "10.23a#";
        if (chunk.rfind(":GXEE", 0) == 0) return "1";
        if (silent_status.load() && chunk.rfind(":GU", 0) == 0) return "";
        if (overlong_goto.load() && chunk.rfind(":MS", 0) == 0) return std::string(65600, '0');
        if (handshake_only.load()) {
            if (chunk.rfind(":GU", 0) == 0) return "";  // no reply: the status parser would take "?#"
            if (chunk.rfind(":GR", 0) == 0 || chunk.rfind(":GD", 0) == 0 || chunk.rfind(":GA", 0) == 0 ||
                chunk.rfind(":GZ", 0) == 0) {
                return "?#";
            }
        }
        if (chunk.rfind(":GU", 0) == 0) {
            while (block_status.load()) std::this_thread::sleep_for(milliseconds(1));
            std::lock_guard<std::mutex> lock(mutex);
            return status;
        }
        if (chunk.rfind(":GRa", 0) == 0) {
            if (garble_position.load()) return "?#";
            char out[32];
            std::snprintf(out, sizeof(out), "12:00:%02d.0000#", ra_seconds.load());
            return out;
        }
        if (chunk.rfind(":GDe", 0) == 0) return "+45*00:00.000#";
        if (chunk.rfind(":GA", 0) == 0) {
            if (garble_altaz.load()) return "?#";
            char out[32];
            std::snprintf(out, sizeof(out), "+45*00:%02d#", alt_seconds.load());
            return out;
        }
        if (chunk.rfind(":GZ", 0) == 0) return "180*00:00#";
        if (chunk.rfind(":GL", 0) == 0) return "10:20:30#";
        if (chunk.rfind(":GC", 0) == 0) return "10/28/26#";
        if (chunk.rfind(":GG", 0) == 0) return "+00:00#";
        if (chunk.rfind(":Gt", 0) == 0) return garble_site.load() ? "?#" : "+40*30#";
        if (chunk.rfind(":Gg", 0) == 0) return garble_site.load() ? "?#" : "+074*00#";
        if (chunk.rfind(":Gh", 0) == 0) return "-10*#";
        if (chunk.rfind(":Go", 0) == 0) return "90*#";
        if (chunk.rfind(":GE", 0) == 0) return refuse_park.load() ? "22#" : "00#";
        if (chunk.rfind(":hP", 0) == 0 && refuse_park.load()) return "0";
        if (chunk.rfind(":CM", 0) == 0) return "N/A#";
        if (chunk.rfind(":MS", 0) == 0) return "0";
        if (chunk.rfind(":Q", 0) == 0 || chunk.rfind(":M", 0) == 0 || chunk.rfind(":R", 0) == 0) return "";
        if (chunk.rfind(":hC", 0) == 0) return "";
        if (chunk.rfind(":S", 0) == 0 || chunk.rfind(":h", 0) == 0) return "1";
        return "0#";
    }

    std::string reply(const std::string& chunk) {
        exchanges.fetch_add(1);
        std::function<void(const std::string&)> hook;
        {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(chunk);
            hook = on_command;
        }
        if (hook) hook(chunk);
        if (mute.load()) return "";
        std::string r = answer(chunk);
        if (!r.empty() && latency_ms.load() > 0) std::this_thread::sleep_for(milliseconds(latency_ms.load()));
        return r;
    }
};

struct Rig {
    std::shared_ptr<SnapshotMount> mount = std::make_shared<SnapshotMount>();
    FakeTaskClock clock;
    alpacacore::test::FakeMountServer server{[m = mount](const std::string& chunk) { return m->reply(chunk); }};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    // Without a site in the config the connect reads the mount's (:Gt#/:Gg#).
    explicit Rig(bool with_site = true, int response_timeout_ms = 300) {
        onstep::ConnectionInfo info;
        info.type = onstep::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = response_timeout_ms;
        driver = onstep::create_onstep_telescope_with_site(
            0, info, with_site ? std::optional<double>(-33.856784) : std::nullopt,
            with_site ? std::optional<double>(151.215297) : std::nullopt, std::nullopt, false, clock);
    }
    ~Rig() {
        mount->mute = false;
        mount->block_status = false;
        mount->latency_ms = 0;
        try {
            driver->set_connected(false);
        } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort teardown
        }
        driver.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void connect() {
        REQUIRE(server.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
    }
    void pause() { onstep::set_status_poll_paused_for_testing(*driver, true); }
    bool tick(bool full = true) { return onstep::poll_status_now_for_testing(*driver, full); }
};

bool has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    const auto state = driver.get_device_state();
    return std::any_of(state.begin(), state.end(), [&](const auto& e) { return e.name == name; });
}

std::chrono::system_clock::time_point time_stamp(const alpacacore::TelescopeDriver& driver) {
    const std::string text = alpacacore::test::time_stamp_of(driver);
    REQUIRE_FALSE(text.empty());
    return alpacacore::test::parse_timestamp(text);
}

// Runs a synchronous SlewToCoordinates on a thread and steps the clock in the slew loop's 250 ms waits until it
// returns (the poll is paused, so the slew's wait is the only waiter). Rethrows the slew's exception.
void synchronous_slew(Rig& rig, double ra, double dec) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread slew([&] {
        try {
            rig.driver->slew_to_coordinates(ra, dec);
        } catch (...) {
            error = std::current_exception();
        }
        done = true;
    });
    const auto give_up = std::chrono::steady_clock::now() + seconds(20);
    while (!done.load() && std::chrono::steady_clock::now() < give_up) {
        if (rig.clock.wait_for_waiters(1, milliseconds(50))) {
            rig.clock.advance(milliseconds(250));
            (void)rig.clock.wait_for_woken_settled(seconds(5));
        }
    }
    slew.join();
    REQUIRE(done.load());
    if (error) std::rethrow_exception(error);
}

int error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        return e.error_code();
    }
    return 0;
}

}  // namespace

TEST_CASE("OnStep snapshot - getters and DeviceState make no mount exchange at 100 ms per exchange",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll is parked: the connect's reads are over
    // Past 2 s: a getter that still had a time-to-live would ask the mount from here on. The tick
    // this wakes runs to its end before the count is taken.
    rig.clock.advance(seconds(3));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));

    rig.mount->latency_ms = static_cast<int>(kLatency.count());
    const int before = rig.mount->exchanges.load();
    const std::vector<std::pair<const char*, std::function<void()>>> reads = {
        {"RightAscension", [&] { (void)rig.driver->get_right_ascension(); }},
        {"Declination", [&] { (void)rig.driver->get_declination(); }},
        {"Altitude", [&] { (void)rig.driver->get_altitude(); }},
        {"Azimuth", [&] { (void)rig.driver->get_azimuth(); }},
        {"Slewing", [&] { (void)rig.driver->get_slewing(); }},
        {"Tracking", [&] { (void)rig.driver->get_tracking(); }},
        {"AtPark", [&] { (void)rig.driver->get_at_park(); }},
        {"AtHome", [&] { (void)rig.driver->get_at_home(); }},
        {"SideOfPier", [&] { (void)rig.driver->get_side_of_pier(); }},
        {"SiteLatitude", [&] { (void)rig.driver->get_site_latitude(); }},
        {"SiderealTime", [&] { (void)rig.driver->get_sidereal_time(); }},
        {"UTCDate", [&] { (void)rig.driver->get_utc_date(); }},
        {"IsPulseGuiding", [&] { (void)rig.driver->get_is_pulse_guiding(); }},
        {"DeviceState", [&] { (void)rig.driver->get_device_state(); }},
    };
    for (const auto& [name, read] : reads) {
        CAPTURE(name);
        read();
    }
    CHECK(rig.mount->exchanges.load() == before);
    // DeviceState drops a getter that throws, so a read that quietly failed would still be fast: name what must
    // be in it.
    for (const char* name :
         {"RightAscension", "Declination", "Altitude", "Azimuth", "Slewing", "Tracking", "SideOfPier", "TimeStamp"}) {
        CAPTURE(name);
        CHECK(has_entry(*rig.driver, name));
    }
    rig.mount->latency_ms = 0;
}

TEST_CASE("OnStep snapshot - a write is read back at once and a poll that sampled before it does not undo it",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(rig.driver->get_tracking());

    rig.driver->set_tracking(false);
    CHECK_FALSE(rig.driver->get_tracking());  // read-after-write, with no poll between (the fake still says on)

    // A tick that sampled before a write publishes after it: the write wins.
    rig.driver->set_tracking(true);
    std::atomic<bool> once{false};
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (!once.exchange(true)) rig.driver->set_tracking(false);
    });
    REQUIRE(rig.tick());  // samples :GU# with tracking on, then the hook writes off
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    CHECK_FALSE(rig.driver->get_tracking());

    // A sync, written through: the mount still answers 12:00 RA until the next tick.
    rig.driver->sync_to_coordinates(3.5, 12.25);
    CHECK(std::abs(rig.driver->get_right_ascension() - 3.5) < 1e-9);
    CHECK(std::abs(rig.driver->get_declination() - 12.25) < 1e-9);
    // A GOTO start reads as Slewing past its grace, though the fake still answers no GOTO, until a tick says so.
    rig.driver->set_tracking(true);
    rig.mount->set_status("NpEW180#");
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.clock.advance(seconds(3));  // past the 2 s slew grace; the poll is paused, so no tick runs
    CHECK(rig.driver->get_slewing());
    REQUIRE(rig.tick());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-6);  // the mount's own answer again
}

TEST_CASE("OnStep snapshot - DeviceState TimeStamp is when the poll measured, not when it was asked",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(rig.tick());
    auto age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age < milliseconds(1000));

    // No tick while the task clock moves 5 s (inside the 30 s bound, no fault): the stamp keeps the frame's age.
    rig.clock.advance(seconds(5));
    REQUIRE(rig.driver->get_connected());
    age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age > milliseconds(4500));
    CHECK(age < milliseconds(6000));
    // A write-through is not a measurement: the stamp stays.
    rig.driver->set_tracking(false);
    age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age > milliseconds(4500));
    // The next tick is.
    REQUIRE(rig.tick());
    age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age < milliseconds(1000));
}

TEST_CASE(
    "OnStep snapshot - a stale frame is served, three failed ticks refuse it, and past the bound the link is "
    "lost",
    "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    const double ra = rig.driver->get_right_ascension();

    // Stale is not lost: past kSnapshotMaxAge with no failed tick the frame is still served (decision 0010 rule 6).
    rig.clock.advance(seconds(10));
    CHECK(rig.driver->get_right_ascension() == ra);
    CHECK(rig.driver->get_link_fault().empty());

    rig.mount->mute = true;
    rig.mount->ra_seconds = 30;  // what an answering mount would now say
    CHECK(rig.tick());           // two failed ticks: the frame is still served
    CHECK(rig.tick());
    CHECK(rig.driver->get_right_ascension() == ra);
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.tick());  // the third latches the fault
    CHECK(rig.driver->get_connected());
    CHECK(error_of([&] { (void)rig.driver->get_right_ascension(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_of([&] { (void)rig.driver->get_tracking(); }) == alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(has_entry(*rig.driver, "RightAscension"));

    SECTION("a good tick clears the fault") {
        rig.mount->mute = false;
        CHECK(rig.tick());
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_right_ascension() > ra);  // the new measurement
    }
    SECTION("a fault standing past the bound is a lost link") {
        rig.clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
        CHECK_FALSE(rig.tick());  // the tick finds the bound and ends the body
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_device_state().empty());
        CHECK(error_of([&] { (void)rig.driver->get_right_ascension(); }) == alpacacore::AlpacaError::NotConnected);
        rig.mount->mute = false;
        rig.driver->set_connected(true);  // only the client relinks
        CHECK(rig.driver->get_connected());
        CHECK(rig.driver->get_link_fault().empty());
    }
}

TEST_CASE("OnStep snapshot - the poll starts with the connect and ends with the disconnect",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    CHECK(rig.clock.waiter_count() == 0);
    rig.driver->set_connected(true);
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll body parks for its first tick
    const auto polls = rig.mount->count(":GU#");
    rig.clock.advance(seconds(1));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GU#") == polls + 1);  // one tick

    rig.driver->set_connected(false);
    CHECK(rig.clock.waiter_count() == 0);  // the body was joined
    const int exchanges = rig.mount->exchanges.load();
    rig.clock.advance(seconds(3));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));  // a live poll would wake and read first
    CHECK(rig.mount->exchanges.load() == exchanges);        // nothing reads the closed link

    rig.driver->set_connected(true);  // a reconnect starts it again
    CHECK(rig.clock.wait_for_waiters(1, seconds(2)));
}

TEST_CASE("OnStep snapshot - the site getters throw until the site is known", "[onstep][telescope][snapshot]") {
    Rig rig(/*with_site=*/false);
    rig.mount->garble_site = true;  // the connect cannot read the mount's site
    rig.connect();
    rig.pause();
    for (const auto& read : std::vector<std::function<void()>>{[&] { (void)rig.driver->get_site_latitude(); },
                                                               [&] { (void)rig.driver->get_site_longitude(); }}) {
        try {
            read();
            FAIL("a site getter answered a site nobody set or read");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("Invalid OnStep site info response") != std::string::npos);
        }
    }
    // A client write makes that coordinate known; the other stays unknown.
    rig.clock.advance(seconds(3));  // past the site retry delay: the write's own pair read fails again
    rig.driver->set_site_latitude(12.5);
    CHECK(rig.driver->get_site_latitude() == 12.5);
    CHECK(error_of([&] { (void)rig.driver->get_site_longitude(); }) == alpacacore::AlpacaError::InvalidOperation);
    // The poll's full tick retries the pair once the mount answers it.
    rig.mount->garble_site = false;
    rig.clock.advance(seconds(3));
    REQUIRE(rig.tick(true));
    CHECK(std::abs(rig.driver->get_site_longitude() - (-74.0)) < 1e-9);  // west-positive on the wire
}

TEST_CASE(
    "OnStep snapshot - the poll restores tracking after a GOTO that ended without it, but not while a Park is "
    "pending",
    "[onstep][telescope][snapshot]") {
    // Declared before the rig: the held Park thread polls it until the rig's teardown joins that thread.
    std::atomic<bool> held{true};
    Rig rig;
    rig.connect();
    rig.pause();
    rig.driver->slew_to_coordinates_async(2.0, 20.0);  // starts with tracking on: a restore is owed
    rig.mount->set_status("nNpEW180#");                // the GOTO ended with tracking off
    rig.clock.advance(seconds(3));                     // past the slew grace

    SECTION("the tick that reads the end restores it") {
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":ST60") == 1);
        CHECK(rig.driver->get_tracking());
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":ST60") == 1);  // once
    }
    SECTION("a Park refused in motion and still retrying gets none") {
        // The park's first try is refused "in motion" (22), so its body is to retry; the spawn seam holds that
        // body, and the tick runs while the Park is pending.
        onstep::set_park_home_spawn_for_testing(*rig.driver, [&held](std::function<void()> body) {
            return std::thread([&held, body = std::move(body)] {
                while (held.load()) std::this_thread::sleep_for(milliseconds(1));
                body();
            });
        });
        struct Release {
            std::atomic<bool>& held;
            ~Release() { held = false; }
        } release{held};
        rig.mount->refuse_park = true;
        rig.driver->park();
        REQUIRE(rig.mount->count(":hP#") == 1);
        REQUIRE(rig.driver->get_slewing());  // the Park is pending
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":ST60") == 0);
        rig.mount->refuse_park = false;
        held = false;
    }
}

// A poll body cancelled while it samples touches neither the link's health nor the mount (decision 0006 rule 6): the
// tick that would send the GOTO's tracking restore, cancelled by a disconnect inside its :GU#, sends none.
TEST_CASE("OnStep snapshot - a poll body cancelled mid-sample sends no tracking restore",
          "[onstep][telescope][snapshot]") {
    Rig rig(/*with_site=*/true, /*response_timeout_ms=*/20000);  // the held :GU# must not time out
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll body parks for its first tick
    rig.driver->slew_to_coordinates_async(2.0, 20.0);    // starts with tracking on: a restore is owed
    rig.mount->set_status("nNpEW180#");                  // the GOTO ended with tracking off
    const auto polls = rig.mount->count(":GU#");
    rig.mount->block_status = true;
    rig.clock.advance(seconds(3));  // past the slew grace: the tick reads the end
    for (int i = 0; i < 2000 && rig.mount->count(":GU#") == polls; ++i) std::this_thread::sleep_for(milliseconds(1));
    REQUIRE(rig.mount->count(":GU#") == polls + 1);  // the tick is inside its :GU#, holding the operation lock

    auto disconnect = std::async(std::launch::async, [&] { rig.driver->set_connected(false); });
    for (int i = 0; i < 5000 && onstep::status_poll_running_for_testing(*rig.driver); ++i) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    REQUIRE_FALSE(onstep::status_poll_running_for_testing(*rig.driver));  // cancelled, still sampling
    rig.mount->block_status = false;
    REQUIRE(disconnect.wait_for(seconds(20)) == std::future_status::ready);
    disconnect.get();
    CHECK(rig.mount->count(":ST60") == 0);
}

TEST_CASE("OnStep snapshot - IsPulseGuiding stays true until a frame sampled after the pulse is published",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(2, 500);  // East, 0.5 s: the pulse starts its own slot (pulse_measurement_)
    // The status poll body is not touched; the pulse-end body's first wait is the next one counted.
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));
    CHECK(rig.driver->get_is_pulse_guiding());
    const auto polls = rig.mount->count(":GRa#");

    // At the pulse's end the frame is still the one from before it.
    rig.clock.advance(milliseconds(500));
    CHECK(rig.driver->get_is_pulse_guiding());
    // 50 ms later the poll measures; then the pulse reads ended.
    rig.mount->ra_seconds = 7;
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GRa#") == polls + 1);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 7.0 / 3600.0)) < 1e-6);
}

// The 1 s ticks run on through a pulse guide; one extra tick 50 ms after its end ends the IsPulseGuiding hold.
TEST_CASE("OnStep snapshot - a pulse guide keeps the poll's 1 s ticks and adds one at the pulse's end",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll waits for its first tick, 1 s away
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(0, 3000);                               // North, 3 s
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));  // the pulse-end tick waits too

    for (int second = 1; second <= 3; ++second) {
        CAPTURE(second);
        const auto polls = rig.mount->count(":GU#");
        rig.clock.advance(seconds(1));
        REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
        CHECK(rig.mount->count(":GU#") == polls + 1);  // a regular tick every second, the pulse's end included
        CHECK(rig.driver->get_is_pulse_guiding());     // the tick at the end sampled before end + 50 ms
    }
    const auto polls = rig.mount->count(":GU#");
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GU#") == polls + 1);  // the pulse-end tick
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// The synchronous slew publishes where it ended (decision 0010): read right after it returns, with no poll
// tick between, the getters serve the slew's end, not the frame from before it.
TEST_CASE("OnStep snapshot - a synchronous slew publishes the position where it ended",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-9);
    rig.mount->ra_seconds = 33;  // where the GOTO ends
    const auto polls = rig.mount->count(":GRa#");
    synchronous_slew(rig, 12.0, 45.0);
    CHECK(rig.mount->count(":GRa#") == polls + 1);  // the slew's own end read; the poll is paused
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 33.0 / 3600.0)) < 1e-6);
    CHECK(std::abs(rig.driver->get_declination() - 45.0) < 1e-6);
    CHECK(std::abs(rig.driver->get_altitude() - 45.0) < 1e-6);
    CHECK(std::abs(rig.driver->get_azimuth() - 180.0) < 1e-6);
    CHECK_FALSE(rig.driver->get_slewing());
}

// A failed end read is a failed poll tick: the last values stay served and the failure counts toward the fault.
TEST_CASE("OnStep snapshot - a failed end-of-slew read keeps the last values and counts toward the fault",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    const double ra = rig.driver->get_right_ascension();
    const double alt = rig.driver->get_altitude();
    rig.mount->garble_position = true;
    REQUIRE_NOTHROW(synchronous_slew(rig, 12.0, 45.0));  // the slew itself ended
    CHECK(rig.driver->get_right_ascension() == ra);
    CHECK(rig.driver->get_altitude() == alt);
    CHECK(rig.driver->get_link_fault().empty());  // one failure
    CHECK(rig.tick());                            // the second
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.tick());  // the third latches: the end read counted as the first
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    rig.mount->garble_position = false;
}

// A failed :GA#/:GZ# read is not a failed tick: the poll keeps the last Alt/Az and the link fault stays clear.
TEST_CASE("OnStep snapshot - a failed Alt/Az read keeps the last value and does not count toward the fault",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    const double alt = rig.driver->get_altitude();
    REQUIRE(std::abs(alt - 45.0) < 1e-6);
    rig.mount->garble_altaz = true;
    rig.mount->alt_seconds = 36;  // what :GA# would say were it readable
    rig.mount->ra_seconds = 9;    // RA still answers: the new value is served
    // Alt/Az is read on every second tick: three full ticks, so at least the failed reads below happen.
    const auto reads = rig.mount->count(":GA#");
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        REQUIRE(rig.tick(true));
        CHECK(rig.driver->get_link_fault().empty());
    }
    CHECK(rig.mount->count(":GA#") == reads + 3);  // each full tick asked and failed
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 9.0 / 3600.0)) < 1e-6);
    CHECK(rig.driver->get_altitude() == alt);
    rig.mount->garble_altaz = false;
}

// The connect's seed counts each failed read toward the fault, so a mount that passes the handshake but
// answers none of them connects with the fault latched (behaviour change 13).
TEST_CASE("OnStep snapshot - a mount that answers none of the connect's seed reads connects faulted",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->handshake_only = true;
    rig.connect();
    rig.pause();
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    CHECK_THROWS(rig.driver->get_right_ascension());
    CHECK_THROWS(rig.driver->get_altitude());
    rig.mount->handshake_only = false;
    REQUIRE(rig.tick(true));  // one good tick clears it
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-6);
}

// A control path's :GU# read writes through only a status that differs from the frame's: an equal one leaves
// the poll sample in flight to publish, a different one drops it (decision 0010 point 4).
TEST_CASE("OnStep snapshot - a control-path status read drops the poll sample in flight only when it differs",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->set_status("nNPEW180#");  // parked: Park() reads :GU# and returns, a no-op
    rig.connect();
    rig.pause();
    const double ra = rig.driver->get_right_ascension();
    rig.mount->ra_seconds = 10;  // what the tick samples
    std::string during;          // the :GU# Park() reads inside the tick's window
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        rig.mount->set_status(during);
        rig.driver->park();
    });

    SECTION("an equal status") {
        during = "nNPEW180#";
        REQUIRE(rig.tick());
        CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 10.0 / 3600.0)) < 1e-6);  // published
    }
    SECTION("a different status") {
        during = "NPEW180#";  // still parked, now tracking
        REQUIRE(rig.tick());
        CHECK(rig.driver->get_right_ascension() == ra);  // the tick was dropped
        CHECK(rig.driver->get_tracking());               // the read was written through
    }
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
}

// The regular tick and the pulse-end tick both publish after they release the operation lock. A tick whose sample
// is older than the held frame's is skipped, so a stalled older tick cannot land its frame on top of a newer one.
TEST_CASE("OnStep snapshot - a tick's publish drops an older tick that is still between its sample and its publish",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->ra_seconds = 10;  // what tick A samples
    std::atomic<bool> once{false};
    bool b_published = false;
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (once.exchange(true)) {
            return;  // tick B's own after-sample call
        }
        rig.mount->ra_seconds = 20;  // what tick B samples
        b_published = rig.tick();    // B samples and publishes inside A's gap
    });
    REQUIRE(rig.tick());  // A: samples 10, then B publishes 20, then A's publish is dropped
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(b_published);
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 20.0 / 3600.0)) < 1e-6);
}

// A pulse-end body of an earlier pulse that takes the operation lock in the gap between a newer PulseGuide's
// unlock and its start() has not been replaced yet (decision 0006 rule 6): the newer pulse supersedes it under the
// lock, so the body's own check sees it and it sends no tracking restore and publishes no frame.
TEST_CASE("OnStep snapshot - a pulse-end body superseded by a newer PulseGuide sends no restore and publishes nothing",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    rig.driver->slew_to_coordinates_async(2.0, 20.0);  // starts with tracking on: a restore is owed
    rig.mount->set_status("npEW180#");                 // the GOTO runs, tracking off: the poll ticks restore nothing
    rig.clock.advance(seconds(3));                     // past the slew grace
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(2, 500);  // the old pulse-end body waits 550 ms
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));
    rig.mount->set_status("nNpEW180#");  // the GOTO ended with tracking off: only a tick that reads it restores
    rig.mount->ra_seconds = 9;           // what a published frame would show
    const auto restores = rig.mount->count(":ST") + rig.mount->count(":Te");
    const auto reads = rig.mount->count(":GU#");
    bool body_returned = false;
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, [&] {
        // The newer pulse has unlocked and not yet started: the old body wakes now and takes the operation lock.
        rig.clock.advance(milliseconds(550));
        (void)rig.clock.wait_for_woken_settled(seconds(5));
        for (int i = 0; i < 5000 && onstep::pulse_measurement_running_for_testing(*rig.driver); ++i) {
            std::this_thread::sleep_for(milliseconds(1));  // real time for the body to return, not a driver timer
        }
        body_returned = !onstep::pulse_measurement_running_for_testing(*rig.driver);
    });
    rig.driver->pulse_guide(3, 500);  // the newer pulse supersedes it
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(body_returned);
    CHECK(rig.mount->count(":ST") + rig.mount->count(":Te") == restores);  // no restore from the old body
    CHECK(rig.mount->count(":GU#") == reads);                              // it did not even sample
    CHECK(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-6);      // its frame is not published
}

// Decision 0022 rule 2: the slew's hold window is armed before the :MS# goes out, so Slewing reads true while the
// GOTO is being sent and once the call returns, before any :GU# has been read.
TEST_CASE("OnStep snapshot - Slewing is true as soon as SlewToCoordinatesAsync returns",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");  // no GOTO
    std::atomic<int> during{-1};        // Slewing read while the :MS# is in flight
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":MS", 0) == 0) during = rig.driver->get_slewing() ? 1 : 0;
    });
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.mount->set_on_command(nullptr);
    CHECK(during.load() == 1);
    CHECK(rig.driver->get_slewing());
    rig.mount->set_status("npEW180#");  // the mount shows the GOTO from its first :GU# (rig, 2026-10-29)
    REQUIRE(rig.tick());
    CHECK(rig.driver->get_slewing());
}

// Decision 0022 rule 3: a :GU# read with no GOTO ends the slew at once, inside the 2 s grace. The mount shows the
// GOTO on one read and then reports stopped: the synchronous call returns on the next 250 ms poll, not at 2 s.
TEST_CASE("OnStep snapshot - a synchronous slew that lands inside the grace returns before the grace ends",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");
    std::atomic<int> reads_after_goto{-1};  // -1: the :MS# has not gone out
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":MS", 0) == 0) {
            reads_after_goto = 0;
        } else if (chunk.rfind(":GU", 0) == 0 && reads_after_goto.load() >= 0) {
            rig.mount->set_status(reads_after_goto.fetch_add(1) == 0 ? "npEW180#" : "NpEW180#");
        }
    });
    const auto start = rig.clock.now();
    REQUIRE_NOTHROW(synchronous_slew(rig, 2.0, 20.0));
    const auto elapsed = rig.clock.now() - start;
    CHECK(elapsed < seconds(2));
    CHECK_FALSE(rig.driver->get_slewing());
}

// Decision 0022 rule 2: AbortSlew clears the window, so Slewing reads false at once, inside the 2 s.
TEST_CASE("OnStep snapshot - AbortSlew inside the hold clears Slewing at once",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    REQUIRE(rig.driver->get_slewing());
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_slewing());
    rig.clock.advance(milliseconds(500));
    CHECK_FALSE(rig.driver->get_slewing());
}

// Decision 0022 rule 2: a reconnect starts without the old session's window.
TEST_CASE("OnStep snapshot - a reconnect inside the hold leaves Slewing false",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    REQUIRE(rig.driver->get_slewing());
    rig.driver->set_connected(false);
    rig.connect();
    CHECK_FALSE(rig.driver->get_slewing());
}

// Decision 0022 rule 4: the pulse's window is on the task clock; a frame sampled before the pulse's end keeps it.
TEST_CASE("OnStep snapshot - a pulse keeps its axis while virtual time has not reached its end",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.driver->pulse_guide(0, 1000);  // North, 1 s
    rig.clock.advance(milliseconds(999));
    REQUIRE(rig.tick());
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.clock.advance(milliseconds(51));  // the end plus kPulseGuideSettle
    REQUIRE(rig.tick());
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// Each axis has its own window (OnStepX times each axis on its own): a shorter North pulse sent during a longer
// East one does not end the East pulse's IsPulseGuiding.
TEST_CASE("OnStep snapshot - a shorter pulse on one axis leaves the other axis's longer pulse guiding",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.driver->pulse_guide(2, 3000);  // East, 3 s
    rig.driver->pulse_guide(0, 500);   // North, 0.5 s
    rig.clock.advance(milliseconds(600));
    REQUIRE(rig.tick());  // North has ended and is measured
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.clock.advance(milliseconds(2400));
    REQUIRE(rig.tick());  // East's end, sampled before end + 50 ms
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.tick());
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// Decision 0022 rule 3: a reading taken before the :MS# answer does not end the slew, even once the window has run
// out. A tick samples "no GOTO" and, between its sample and its publish, a SlewToCoordinatesAsync goes out: the
// dispatch's write-through drops that tick's publish, and Slewing stays true past the 2 s window with no restore.
// A tick sampled after the answer that shows no GOTO is the landing.
TEST_CASE("OnStep snapshot - a reading taken before the :MS# answer does not end the slew",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    std::atomic<int> calls{0};
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (calls.fetch_add(1) != 0) return;
        rig.driver->slew_to_coordinates_async(2.0, 20.0);  // tracking on: a restore is owed at the end
    });
    REQUIRE(rig.tick());  // sampled "no GOTO" before the :MS#
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    rig.mount->set_status("nNpEW180#");  // no GOTO, tracking off
    rig.clock.advance(seconds(3));       // past the window
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount->count(":ST60") == 0);
    REQUIRE(rig.tick());  // sampled after the answer: the landing, with no window to wait for
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.mount->count(":ST60") == 1);
}

// Decision 0022 rule 3 (the Sky-Watcher check, "a short goto reports landed before the 8 s hold ends"): landing is
// the mount's answer, never the 2 s window. The mount shows the GOTO (1 ms after :MS# answers on the rig), stops at
// 0.5 s, and the next poll tick reads Slewing false at 1 s.
TEST_CASE("OnStep snapshot - a short goto reports landed before the 2 s hold ends",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.mount->set_status("npEW180#");  // the GOTO runs
    REQUIRE(rig.driver->get_slewing());
    rig.clock.advance(milliseconds(500));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.driver->get_slewing());   // still running
    rig.mount->set_status("NpEW180#");  // the GOTO ends at about 0.5 s
    rig.clock.advance(milliseconds(500));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));  // the 1 s tick
    CHECK_FALSE(rig.driver->get_slewing());                 // 1 s, inside the 2 s window
}

// A new pulse keeps the running pulse-end tick when the other axis's pulse ends later. With the poll running: East
// 3 s, then North 0.5 s. East's IsPulseGuiding stays true through the regular 1 s ticks and the 2 s bound, and ends
// on the tick 50 ms after 3 s; North adds no tick of its own.
TEST_CASE("OnStep snapshot - a shorter pulse on the other axis keeps the longer pulse's end tick",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(2, 3000);  // East, 3 s: its end tick waits 3.05 s
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));
    rig.driver->pulse_guide(0, 500);  // North, 0.5 s: must not replace it with a tick at 0.55 s

    for (int second = 1; second <= 3; ++second) {
        CAPTURE(second);
        const auto polls = rig.mount->count(":GU#");
        rig.clock.advance(seconds(1));
        REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
        CHECK(rig.mount->count(":GU#") == polls + 1);  // the regular tick only
        CHECK(rig.driver->get_is_pulse_guiding());
    }
    const auto polls = rig.mount->count(":GU#");
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GU#") == polls + 1);  // East's pulse-end tick
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// Publishes are ordered by sample: a tick that sampled after an older tick, and publishes after it, is published
// (before, the older tick's publish dropped every sample in flight, the newer one included).
TEST_CASE("OnStep snapshot - a newer sample is not dropped by an older tick's publish",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->ra_seconds = 10;  // what tick A samples
    std::atomic<int> calls{0};
    std::atomic<bool> b_sampled{false};
    std::atomic<bool> a_published{false};
    std::atomic<bool> b_result{false};
    std::thread b;
    const auto await = [](const std::atomic<bool>& flag) {
        for (int i = 0; i < 5000 && !flag.load(); ++i) std::this_thread::sleep_for(milliseconds(1));
    };
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (calls.fetch_add(1) == 0) {
            // Tick A, between its sample and its publish: tick B samples now and publishes after A.
            rig.mount->ra_seconds = 20;
            b = std::thread([&] { b_result = rig.tick(); });
            await(b_sampled);
            return;
        }
        b_sampled = true;  // tick B, between its sample and its publish
        await(a_published);
    });
    REQUIRE(rig.tick());  // A publishes 10
    a_published = true;
    if (b.joinable()) b.join();
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(b_sampled.load());
    REQUIRE(b_result.load());
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 20.0 / 3600.0)) < 1e-6);
}

// A Disconnect that lands between a PulseGuide's unlock and its start() has already joined the pulse-end slot, so
// the body that start() then installs belongs to the old session. The next Connect joins it before the new
// session starts, so it neither waits on into the new session nor samples or publishes there.
TEST_CASE("OnStep snapshot - a reconnect joins a pulse-end body started after the disconnect's join",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the status poll
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, [&] { rig.driver->set_connected(false); });
    rig.driver->pulse_guide(2, 500);  // unlocks, the Disconnect runs, then the pulse-end body starts
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, nullptr);
    REQUIRE_FALSE(rig.driver->get_connected());
    REQUIRE(onstep::pulse_measurement_running_for_testing(*rig.driver));  // waiting its 550 ms
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    CHECK(rig.clock.waiter_count() == 1);  // only that body: the poll was joined

    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK_FALSE(onstep::pulse_measurement_running_for_testing(*rig.driver));  // joined by the connect
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));                       // the new session's poll
    CHECK(rig.clock.waiter_count() == 1);                                     // and nothing else
}

// #1893: with the site unknown, the members that use it refuse as the site getters do, instead of using latitude and
// longitude 0. An Alt/Az slew retries the site read first and sends nothing when it still fails.
TEST_CASE("OnStep snapshot - members that use the site refuse while it is unknown", "[onstep][telescope][snapshot]") {
    Rig rig(/*with_site=*/false);
    rig.mount->garble_site = true;       // the connect cannot read the mount's site
    rig.mount->set_status("nNpEW180#");  // Tracking off, so an Alt/Az slew is not refused for Tracking
    rig.connect();
    rig.pause();
    const auto refuses = [](const std::function<void()>& call, const std::string& member) {
        try {
            call();
            FAIL(member + " answered with a site nobody set or read");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find(member + " needs the site") != std::string::npos);
            CHECK(std::string(ex.what()).find("Invalid OnStep site info response") != std::string::npos);
        }
    };
    refuses([&] { (void)rig.driver->get_side_of_pier(); }, "SideOfPier");
    refuses([&] { (void)rig.driver->get_destination_side_of_pier(1.0, 20.0); }, "DestinationSideOfPier");
    refuses([&] { (void)rig.driver->get_sidereal_time(); }, "SiderealTime");
    CHECK_FALSE(has_entry(*rig.driver, "SideOfPier"));
    CHECK_FALSE(has_entry(*rig.driver, "SiderealTime"));

    rig.clock.advance(seconds(3));  // past the site retry delay: the slew reads the site again first
    const auto site_reads = rig.mount->count(":Gt");
    refuses([&] { rig.driver->slew_to_alt_az_async(45.0, 180.0); }, "SlewToAltAzAsync");
    CHECK(rig.mount->count(":Gt") == site_reads + 1);
    refuses([&] { rig.driver->slew_to_alt_az(45.0, 180.0); }, "SlewToAltAz");
    CHECK(rig.mount->count(":Sr") == 0);
    CHECK(rig.mount->count(":Sd") == 0);
    CHECK(rig.mount->count(":MS") == 0);

    rig.mount->garble_site = false;  // the poll's full tick reads the site, and the members answer
    rig.clock.advance(seconds(3));
    REQUIRE(rig.tick(true));
    CHECK_NOTHROW((void)rig.driver->get_side_of_pier());
    CHECK_NOTHROW((void)rig.driver->get_sidereal_time());
}

// #1895: a poll that cannot start (no thread) fails the connect, which is undone as a Disconnect is, rather than
// serving the first frame for the whole session.
TEST_CASE("OnStep snapshot - a connect whose status poll cannot start fails and closes the port",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    std::atomic<bool> refuse{true};
    onstep::set_status_poll_spawn_for_testing(*rig.driver, [&refuse](std::function<void()> body) {
        if (refuse.load()) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return std::thread(std::move(body));
    });
    std::string message;
    try {
        rig.driver->set_connected(true);
    } catch (const alpacacore::AlpacaException& ex) {
        message = ex.what();
    }
    CHECK(message.find("status poll did not start") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(onstep::OnStepProtocolWrapper::instance().is_connected());  // the port was closed
    CHECK(error_of([&] { (void)rig.driver->get_right_ascension(); }) == alpacacore::AlpacaError::NotConnected);
    CHECK(rig.clock.waiter_count() == 0);

    refuse = false;  // the next Connect starts the poll
    rig.connect();
    CHECK(rig.clock.wait_for_waiters(1, seconds(2)));
}

// #1895: a Tracking write on a connect whose :GU# read failed reads :GU# first, so the write-through lands in a
// status the getter serves, instead of in a frame with no status until the next good tick.
TEST_CASE("OnStep snapshot - a Tracking write after a connect that read no status is served at once",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    REQUIRE(error_of([&] { (void)rig.driver->get_tracking(); }) == alpacacore::AlpacaError::DriverException);
    rig.mount->silent_status = false;
    rig.driver->set_tracking(false);  // the mount still tracks ("NpEW180#") until the next tick reads it
    CHECK_FALSE(rig.driver->get_tracking());
}

// #1895: Unpark on a connect whose :GU# read failed reads :GU# first, so its write-through lands in a status the
// getter serves; while :GU# stays silent it refuses instead of unparking blind.
TEST_CASE("OnStep snapshot - an Unpark after a connect that read no status is served at once",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    rig.mount->set_status("nNPEW180#");  // parked, but nothing has read it
    CHECK(error_of([&] { rig.driver->unpark(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(rig.mount->count(":hR") == 0);  // nothing was sent while the status was unknown

    rig.mount->silent_status = false;
    rig.driver->unpark();
    CHECK_FALSE(rig.driver->get_at_park());
}

// #1895: an unread status is not taken for "not parked": the commands that check for a park read :GU# first and
// refuse while it does not answer. The stops still go out.
TEST_CASE("OnStep snapshot - a status the connect could not read refuses the commands that check for a park",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    rig.mount->set_status("nNPEW180#");  // parked, but nothing has read it
    CHECK(error_of([&] { rig.driver->pulse_guide(0, 100); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_of([&] { rig.driver->move_axis(0, 1.0); }) == alpacacore::AlpacaError::DriverException);
    CHECK(rig.mount->count(":M") == 0);  // no :Mg, :RA or :Me went out
    CHECK(rig.mount->count(":RA") == 0);
    CHECK_NOTHROW(rig.driver->move_axis(0, 0.0));  // a stop is not held up by the unread status
    CHECK_NOTHROW(rig.driver->abort_slew());

    rig.mount->silent_status = false;  // once :GU# answers, the guard reads it
    CHECK(error_of([&] { rig.driver->find_home(); }) == alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(error_of([&] { rig.driver->set_tracking(true); }) == alpacacore::AlpacaError::InvalidWhileParked);
}

// Turning tracking off is a stop: an unread status never holds it up, as for AbortSlew and MoveAxis(axis, 0).
TEST_CASE("OnStep snapshot - Tracking off after a connect that read no status still stops tracking",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    CHECK_NOTHROW(rig.driver->set_tracking(false));  // the :GU# read inside it fails too
    CHECK(rig.mount->count(":ST0") == 1);
}

// #1887 (decision 0022 rule 2): the slew's hold window is cleared when the :MS# send itself throws, so Slewing
// reads false right after, with no virtual time advanced (the 2 s hold would otherwise still be open). A silent
// :MS# does not reach that path: it reads as an empty reply, which the refusal branch clears.
TEST_CASE("OnStep snapshot - a GOTO whose :MS# send fails leaves Slewing false at once",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");  // tracking, no GOTO
    rig.mount->overlong_goto = true;
    CHECK(error_of([&] { rig.driver->slew_to_coordinates_async(2.0, 20.0); }) ==
          alpacacore::AlpacaError::DriverException);
    REQUIRE(rig.mount->count(":MS") == 1);
    CHECK_FALSE(rig.driver->get_slewing());
}

// #1887 (decision 0022 rule 2): the pulse's hold window is cleared when the :Mg send throws, so IsPulseGuiding reads
// false right after, with no virtual time advanced. :Mg is sent blind, so a silent mount is not a failed send: the
// connection is dropped instead, which fails the write.
TEST_CASE("OnStep snapshot - a pulse guide whose :Mg send fails leaves IsPulseGuiding false at once",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(rig.server.drop_connections());
    CHECK(error_of([&] { rig.driver->pulse_guide(0, 2000); }) == alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

#endif  // !_WIN32

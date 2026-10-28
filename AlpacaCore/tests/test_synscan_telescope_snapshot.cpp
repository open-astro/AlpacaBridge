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

// SynScan getters and DeviceState served from the polled snapshot (decision 0010, SynScan adoption; link-loss
// policy of decision 0009; issue #1247). The driver runs on a FakeTaskClock, so its status poll runs only when a
// case advances the clock, and the fake handset answers on the wall clock after an injected latency per
// transaction: a getter that asked the handset would pay it and show in the transaction count.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace sc = alpacacore::vendor::synscan;
using alpacacore::test::FakeTaskClock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kLatency = milliseconds(100);
// A getter that reached the handset would pay kLatency, so the budget only has to sit below it. Sanitized builds
// run several times slower on a loaded runner and get more room; the transaction count is the strict check.
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

struct Handset {
    std::atomic<int> transactions{0};
    std::atomic<int> latency_ms{0};
    std::atomic<bool> mute{false};
    std::atomic<bool> slewing{false};
    std::atomic<int> tracking_mode{1};
    std::atomic<char> pier{'E'};       // 0 = the handset does not answer 'p'
    std::atomic<int> w_latency_ms{0};  // a handset slow on `w` (site) only, answering it unparseably
    std::atomic<bool> w_entered{false};
    std::atomic<bool> w_silent{false};  // the handset ignores `w` (site) entirely
    std::atomic<int> w_count{0};
    std::atomic<bool> w_site{false};  // `w` answers a site (33d51'24" S, 151d12'54" E) instead of an unparseable '#'
    std::atomic<uint32_t> ra_counts{0x5CECE2};  // the position `e` answers (24-bit); a GOTO (`r`) lands on its aim
    std::atomic<uint32_t> dec_counts{0xD001B9};
};

std::string position_reply(const Handset& st) {
    char out[32];
    std::snprintf(out, sizeof(out), "%06X00,%06X00#", st.ra_counts.load() & 0xFFFFFFu,
                  st.dec_counts.load() & 0xFFFFFFu);
    return out;
}

std::string reply(Handset& st, const std::string& chunk) {
    st.transactions.fetch_add(1);
    if (chunk.empty()) return "0#";
    if (st.mute.load() && chunk[0] != 'K' && chunk[0] != 'V') return "";
    if (st.latency_ms.load() > 0) std::this_thread::sleep_for(milliseconds(st.latency_ms.load()));
    switch (chunk[0]) {
        case 'K':
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        case 'V':
            return "042A00#";
        case 'e':
            return position_reply(st);
        case 'r':  // "r" + 6 hex + "00" + "," + 6 hex + "00": the handset lands exactly on the aim
            if (chunk.size() < 18) return "";
            st.ra_counts.store(static_cast<uint32_t>(std::strtoul(chunk.substr(1, 6).c_str(), nullptr, 16)));
            st.dec_counts.store(static_cast<uint32_t>(std::strtoul(chunk.substr(10, 6).c_str(), nullptr, 16)));
            return "#";
        case 'z':
            return "8A4FDA00,0D527400#";
        case 'L':
            return st.slewing.load() ? "1#" : "0#";
        case 'm':  // mount model: an EQ mount
            return std::string(1, '\x32') + "#";
        case 'p':
            return st.pier.load() == 0 ? std::string("#") : std::string(1, st.pier.load()) + "#";
        case 't':
            return std::string(1, static_cast<char>(st.tracking_mode.load())) + "#";
        case 'T':
            if (chunk.size() > 1) st.tracking_mode.store(chunk[1]);
            return "#";
        case 'w':
            st.w_count.fetch_add(1);
            if (st.w_silent.load()) {
                st.w_entered.store(true);
                return "";
            }
            if (st.w_site.load()) return std::string("\x21\x33\x18\x01\x97\x0c\x36\x00#", 9);
            if (st.w_latency_ms.load() > 0) {
                st.w_entered.store(true);
                std::this_thread::sleep_for(milliseconds(st.w_latency_ms.load()));
            }
            return "#";
        default:
            return "#";
    }
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(alpacacore::test::FakeMountServer& server,
                                                              FakeTaskClock& clock) {
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 300;
    auto driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                         false, sc::SynScanAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    return driver;
}

// No configured site: with a `w` the handset cannot answer, the driver has no site to compute from.
std::unique_ptr<alpacacore::TelescopeDriver> connected_driver_without_site(alpacacore::test::FakeMountServer& server,
                                                                           FakeTaskClock& clock, int timeout_ms) {
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = timeout_ms;
    auto driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, std::nullopt, std::nullopt,
                                                         std::nullopt, false, sc::SynScanAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    return driver;
}

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

TEST_CASE("SynScan snapshot - getters and DeviceState make no handset transaction at 100 ms per transaction",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, milliseconds(2000)));  // the poll is parked: the connect's reads are over
    driver->set_utc_date(std::chrono::system_clock::time_point{seconds(1800000000)});

    st->latency_ms = static_cast<int>(kLatency.count());
    const int before = st->transactions.load();

    const std::vector<std::pair<const char*, std::function<void()>>> reads = {
        {"RightAscension", [&] { (void)driver->get_right_ascension(); }},
        {"Declination", [&] { (void)driver->get_declination(); }},
        {"Azimuth", [&] { (void)driver->get_azimuth(); }},
        {"Altitude", [&] { (void)driver->get_altitude(); }},
        {"Slewing", [&] { (void)driver->get_slewing(); }},
        {"Tracking", [&] { (void)driver->get_tracking(); }},
        {"SideOfPier", [&] { (void)driver->get_side_of_pier(); }},
        {"UTCDate", [&] { (void)driver->get_utc_date(); }},
        {"SiderealTime", [&] { (void)driver->get_sidereal_time(); }},
        {"AtPark", [&] { (void)driver->get_at_park(); }},
        {"AtHome", [&] { (void)driver->get_at_home(); }},
        {"IsPulseGuiding", [&] { (void)driver->get_is_pulse_guiding(); }},
        {"TrackingRate", [&] { (void)driver->get_tracking_rate(); }},
        {"DeviceState", [&] { (void)driver->get_device_state(); }},
    };
    for (const auto& [name, read] : reads) {
        CAPTURE(name);
        CHECK(timed(read) < kGetterBudget);
    }
    CHECK(st->transactions.load() == before);
    // DeviceState drops a getter that throws, so a read that quietly failed would still be fast: name what must be in
    // it.
    for (const char* name : {"RightAscension", "Declination", "Slewing", "Tracking", "TimeStamp"}) {
        CAPTURE(name);
        CHECK(has_entry(*driver, name));
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a setter's value is read back at once and a stale poll does not undo it",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_tracking());

    driver->set_tracking(false);
    CHECK_FALSE(driver->get_tracking());  // read-after-write, with no poll between

    // A poll that sampled before a write publishes after it: the write wins.
    driver->set_tracking(true);
    sc::set_status_poll_paused_for_testing(*driver, true);
    std::atomic<bool> once{false};
    sc::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        if (!once.exchange(true)) driver->set_tracking(false);
    });
    sc::poll_status_now_for_testing(*driver);  // samples tracking on, then the hook writes off
    CHECK_FALSE(driver->get_tracking());
    sc::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    sc::poll_status_now_for_testing(*driver);  // the next poll reads the handset, which holds off
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a sync, a slew start and a clock write are read back at once",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);

    // The fake handset keeps answering 08h42m RA / -67 deg Dec. The pulse-guide position override holds the synced
    // value for 10 s of task clock; past it the frame is what answers, so it must carry the synced position too.
    const auto past_override = seconds(11);
    driver->sync_to_coordinates(3.5, 12.25);
    clock.advance(past_override);
    CHECK(std::abs(driver->get_right_ascension() - 3.5) < 1e-9);
    CHECK(std::abs(driver->get_declination() - 12.25) < 1e-9);

    // A poll that sampled before the sync publishes after it: the sync wins.
    std::atomic<bool> once{false};
    sc::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        if (!once.exchange(true)) driver->sync_to_coordinates(6.0, -20.0);
    });
    sc::poll_status_now_for_testing(*driver);
    clock.advance(past_override);
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 1e-9);
    CHECK(std::abs(driver->get_declination() - (-20.0)) < 1e-9);
    sc::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);

    // The next poll reads the handset (Dec only: the precise RA read is corrected against the last known RA).
    sc::poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_declination() - (-67.490537)) < 1e-5);

    // A clock write is read back, not re-read from the handset.
    const auto when = std::chrono::system_clock::now() + seconds(3600);
    driver->set_utc_date(when);
    CHECK(std::abs(std::chrono::duration<double>(driver->get_utc_date() - when).count()) < 5.0);

    // A slew start flips Slewing at once, though the handset still answers "not slewing".
    CHECK_FALSE(driver->get_slewing());
    driver->slew_to_coordinates_async(2.0, 30.0);
    CHECK(driver->get_slewing());
    driver->abort_slew();
    CHECK_FALSE(driver->get_slewing());

    // A stop is read back at once: the frame still says the handset was slewing at the last poll.
    st->slewing = true;
    sc::poll_status_now_for_testing(*driver);
    CHECK(driver->get_slewing());
    driver->abort_slew();
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - DeviceState TimeStamp is when the poll measured, not when it was asked",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());

    auto stamp = parse_timestamp(time_stamp_of(*driver));
    CHECK(std::abs(std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count()) < 1.0);

    // The poll stops while the task clock moves on 5 s (inside the 30 s bound, no fault latched): the stamp keeps
    // the age of the frame.
    sc::set_status_poll_paused_for_testing(*driver, true);
    clock.advance(seconds(5));
    REQUIRE(driver->get_connected());
    stamp = parse_timestamp(time_stamp_of(*driver));
    const double age = std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count();
    CHECK(age > 4.5);
    CHECK(age < 5.5);
    sc::set_status_poll_paused_for_testing(*driver, false);
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - past the staleness bound DeviceState is empty with no TimeStamp",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    CHECK(has_entry(*driver, "TimeStamp"));

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_link_fault().empty());
    CHECK(driver->get_connected());  // a young fault keeps Connected

    clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    sc::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    CHECK_FALSE(has_entry(*driver, "TimeStamp"));
    try {
        (void)driver->get_right_ascension();
        FAIL("a lost link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    st->mute = false;

    // Decision 0009 point 4: Connected=true against a lost link relinks, on both entry points; it is not the
    // idempotent no-op.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_right_ascension() - 8.711749) < 1e-5);

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    sc::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_connected());
    st->mute = false;
    driver->connect();  // the async route
    CHECK(alpacacore::test::settle_connected(*driver, true, seconds(10)));
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_right_ascension() - 8.711749) < 1e-5);
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a pointing state that is neither E nor W reads as pierUnknown, not an error",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    st->pier = 'X';
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, milliseconds(2000)));

    const int before = st->transactions.load();
    CHECK(driver->get_side_of_pier() == -1);
    bool found = false;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "SideOfPier") {
            found = true;
            CHECK(std::get<int>(entry.value) == -1);
        }
    }
    CHECK(found);
    CHECK(st->transactions.load() == before);
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a tick that passed its first hold check makes no exchange once the hunt holds the line",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);

    // The hunt starts after the tick's first look at the hold, before the tick reaches the handset.
    sc::set_status_poll_before_sample_hook_for_testing(*driver,
                                                       [&] { sc::hold_status_poll_for_testing(*driver, true); });
    const int before = st->transactions.load();
    CHECK(sc::poll_status_now_for_testing(*driver));
    CHECK(st->transactions.load() == before);
    sc::set_status_poll_before_sample_hook_for_testing(*driver, nullptr);

    // With the hold dropped the same tick reads the handset again.
    sc::hold_status_poll_for_testing(*driver, false);
    CHECK(sc::poll_status_now_for_testing(*driver));
    CHECK(st->transactions.load() > before);
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a slewing re-read that finds no change does not drop the poll's frame",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    st->slewing = true;
    sc::poll_status_now_for_testing(*driver);  // the frame says: slewing
    REQUIRE(driver->get_slewing());

    // A slew loop or the Park hook re-reads L every 250 ms while the poll samples. Here the PulseGuide gate makes
    // that read between the tick's sample and its publish, and finds the slew still running: no change to the
    // frame, so the tick's measurement must still publish. Before the fix every such read dropped it, and during a
    // GOTO the position stayed at its pre-slew value.
    clock.advance(seconds(5));
    std::atomic<bool> once{false};
    sc::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        if (once.exchange(true)) return;
        try {
            driver->pulse_guide(2, 500);
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): refused mid-slew, as intended
        }
    });
    sc::poll_status_now_for_testing(*driver);
    sc::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    REQUIRE(once.load());
    const auto stamp = parse_timestamp(time_stamp_of(*driver));
    CHECK(std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count() < 1.0);
    st->slewing = false;
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a getter does not wait while the poll retries an unread site",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    // An unparseable `w`: the site stays unread and the poll's slow tick retries it. The timeout is above the 400 ms
    // `w` stall below, so the late reply is not left on the wire.
    auto driver = connected_driver_without_site(server, clock, 1000);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    clock.advance(seconds(3));  // past the site retry delay

    st->w_latency_ms = 400;
    std::thread tick([&] { (void)sc::poll_status_now_for_testing(*driver); });
    const auto entered_by = std::chrono::steady_clock::now() + seconds(5);
    while (!st->w_entered.load() && std::chrono::steady_clock::now() < entered_by) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    REQUIRE(st->w_entered.load());
    CHECK(timed([&] { (void)driver->get_slewing(); }) < milliseconds(200));  // the `w` exchange takes 400 ms
    tick.join();
    st->w_latency_ms = 0;
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a site getter does not hold the driver while the handset ignores w",
          "[synscan][telescope][snapshot]") {
    constexpr int kTimeoutMs = 600;
    for (int round = 0; round < 3; ++round) {
        FakeTaskClock clock;
        auto st = std::make_shared<Handset>();
        alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
        REQUIRE(server.ok());
        auto driver = connected_driver_without_site(server, clock, kTimeoutMs);
        REQUIRE(driver->get_connected());
        sc::set_status_poll_paused_for_testing(*driver, true);
        sc::poll_status_now_for_testing(*driver);
        st->w_silent = true;
        st->w_entered = false;
        clock.advance(seconds(3));  // past the site retry delay
        const int w_before = st->w_count.load();

        std::thread site([&] {
            try {
                (void)(round % 2 == 0 ? driver->get_site_latitude() : driver->get_site_longitude());
            } catch (const alpacacore::AlpacaException&) {  // the unread site refuses; only the wait is measured
            }
        });
        // Wait until the site read reached the handset, or finished without sending (the fixed driver).
        const auto by = std::chrono::steady_clock::now() + milliseconds(200);
        while (!st->w_entered.load() && std::chrono::steady_clock::now() < by) {
            std::this_thread::sleep_for(milliseconds(1));
        }
        CHECK(timed([&] { (void)driver->get_right_ascension(); }) < milliseconds(kTimeoutMs / 3));
        CHECK(timed([&] { (void)driver->get_device_state(); }) < milliseconds(kTimeoutMs / 3));
        site.join();
        CHECK(st->w_count.load() == w_before);  // no handset transaction from a getter
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan snapshot - an unread site refuses SiteLatitude and SiteLongitude as not known yet",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    st->w_silent = true;
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver_without_site(server, clock, 300);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    for (const auto& read : {std::function<void()>([&] { (void)driver->get_site_latitude(); }),
                             std::function<void()>([&] { (void)driver->get_site_longitude(); })}) {
        try {
            read();
            FAIL("an unread site must not read as 0.0");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::ValueNotSet);
            CHECK(std::string(e.what()).find("not known yet") != std::string::npos);
        }
    }
    driver->set_connected(false);
}

bool site_served(const alpacacore::TelescopeDriver& driver) {
    try {
        (void)driver.get_site_latitude();
        return true;
    } catch (const alpacacore::AlpacaException&) {
        return false;
    }
}

TEST_CASE("SynScan snapshot - a configured, handset-read or set site is served", "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    {
        auto driver = connected_driver(server, clock);  // configured site
        REQUIRE(driver->get_connected());
        CHECK(driver->get_site_latitude() == Catch::Approx(-33.856784));
        CHECK(driver->get_site_longitude() == Catch::Approx(151.215297));
        driver->set_connected(false);
    }
    st->w_site = true;
    {
        auto driver = connected_driver_without_site(server, clock, 300);  // read from the handset at connect
        REQUIRE(driver->get_connected());
        sc::set_status_poll_paused_for_testing(*driver, true);
        CHECK(driver->get_site_latitude() == Catch::Approx(-(33.0 + 51.0 / 60.0 + 24.0 / 3600.0)));
        CHECK(driver->get_site_longitude() == Catch::Approx(151.0 + 12.0 / 60.0 + 54.0 / 3600.0));
        driver->set_site_latitude(51.477833);
        driver->set_site_longitude(-0.0015);
        CHECK(driver->get_site_latitude() == Catch::Approx(51.477833));
        CHECK(driver->get_site_longitude() == Catch::Approx(-0.0015));
        driver->set_connected(false);
    }
    {
        // The poll retries an unread site and the getters then serve it.
        st->w_site = false;
        FakeTaskClock poll_clock;
        auto driver = connected_driver_without_site(server, poll_clock, 300);
        REQUIRE(driver->get_connected());
        sc::set_status_poll_paused_for_testing(*driver, true);
        CHECK_THROWS_AS(driver->get_site_latitude(), alpacacore::AlpacaException);
        st->w_site = true;
        poll_clock.advance(seconds(3));  // past the site retry delay
        for (int tick = 0; tick < 20 && !site_served(*driver); ++tick) {
            (void)sc::poll_status_now_for_testing(*driver);
        }
        CHECK(driver->get_site_latitude() == Catch::Approx(-(33.0 + 51.0 / 60.0 + 24.0 / 3600.0)));
        CHECK(driver->get_site_longitude() == Catch::Approx(151.0 + 12.0 / 60.0 + 54.0 / 3600.0));
        driver->set_connected(false);
        // Disconnected, the getters refuse before anything else, like the other reads.
        CHECK_THROWS_AS(driver->get_site_latitude(), alpacacore::AlpacaException);
    }
}

TEST_CASE("SynScan snapshot - DestinationSideOfPier with no site refuses a faulted link like the other reads",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver_without_site(server, clock, 300);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    CHECK(driver->get_destination_side_of_pier(6.0, 10.0) == 1);  // the frame's handset E: pierWest

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_link_fault().empty());
    try {
        (void)driver->get_destination_side_of_pier(6.0, 10.0);
        FAIL("a faulted link must refuse DestinationSideOfPier");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
    st->mute = false;
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - PulseGuide refuses while the handset slews even when the frame is older",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    sc::poll_status_now_for_testing(*driver);  // the frame says: not slewing
    CHECK_FALSE(driver->get_slewing());

    st->slewing = true;  // a keypad slew the frame has not seen yet
    try {
        driver->pulse_guide(2, 500);  // west
        FAIL("PulseGuide was accepted while the handset reported a slew");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
    driver->set_connected(false);
}

// A GOTO cancels and joins the slew slot before it starts. The status poll must outlive that: a poll joined by the
// first GOTO of a session and never restarted freezes every read at the slew-end frame, stops the link-fault latch
// and the site retry, and leaves the landing hunt waiting for a tick that never comes.
TEST_CASE("SynScan snapshot - the status poll keeps ticking after an async and a sync GOTO",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, milliseconds(2000)));  // the poll is parked on the task clock

    // The slew body waits on the task clock (the 2 s start grace, the landing hunt): step it until Slewing drops,
    // then let the slew-end publish and the thread finish (no transaction for 300 ms of wall time).
    const auto wait_slew_end = [&](const std::atomic<bool>& caller_done) {
        const auto deadline = std::chrono::steady_clock::now() + seconds(10);
        while ((driver->get_slewing() || !caller_done.load()) && std::chrono::steady_clock::now() < deadline) {
            clock.advance(milliseconds(250));
            std::this_thread::sleep_for(milliseconds(10));
        }
        if (driver->get_slewing() || !caller_done.load()) return false;
        int seen = st->transactions.load();
        auto quiet_since = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - quiet_since < milliseconds(300)) {
            std::this_thread::sleep_for(milliseconds(10));
            const int now = st->transactions.load();
            if (now != seen) {
                seen = now;
                quiet_since = std::chrono::steady_clock::now();
            }
            if (std::chrono::steady_clock::now() > deadline) return false;
        }
        return true;
    };
    const auto poll_ticks_once = [&] {
        const int before = st->transactions.load();
        clock.advance(seconds(1));
        const auto deadline = std::chrono::steady_clock::now() + seconds(2);
        while (st->transactions.load() == before && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(10));
        }
        return st->transactions.load() > before;
    };

    std::atomic<bool> returned{true};
    driver->slew_to_coordinates_async(2.0, 30.0);  // the handset lands on the aim: the body ends after the hunt
    REQUIRE(wait_slew_end(returned));
    CHECK(clock.wait_for_waiters(1, milliseconds(3000)));  // the poll is parked again, not joined for good
    CHECK(poll_ticks_once());

    // The sync form blocks its caller on the task clock: run it on a thread and step the clock from here.
    returned = false;
    std::thread sync_slew([&] {
        driver->slew_to_coordinates(3.0, 20.0);
        returned = true;
    });
    const bool sync_ended = wait_slew_end(returned);
    sync_slew.join();
    REQUIRE(sync_ended);
    CHECK(clock.wait_for_waiters(1, milliseconds(3000)));
    CHECK(poll_ticks_once());
    driver->set_connected(false);
}

#endif

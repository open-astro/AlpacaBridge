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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <future>
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
#include "pty_responder.h"

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
    std::atomic<bool> garbage{false};  // every command but the echo and `V` gets an unparseable "?#" at once
    std::atomic<bool> slewing{false};
    std::atomic<int> tracking_mode{1};
    std::atomic<char> pier{'E'};       // 0 = the handset does not answer 'p'
    std::atomic<int> w_latency_ms{0};  // a handset slow on `w` (site) only, answering it unparseably
    std::atomic<bool> w_entered{false};
    std::atomic<bool> w_silent{false};  // the handset ignores `w` (site) entirely
    std::atomic<int> w_count{0};
    std::atomic<FakeTaskClock*> w_advances{nullptr};  // `w` moves this clock 3 s on, as a slow site read does
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
    if (st.garbage.load() && chunk[0] != 'K' && chunk[0] != 'V') return "?#";
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
            if (FakeTaskClock* c = st.w_advances.load()) c->advance(std::chrono::seconds(3));
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

// A driver whose response timeout is longer than the latencies a case injects, so a slow reply is an answer.
std::unique_ptr<alpacacore::TelescopeDriver> slow_link_driver(alpacacore::test::FakeMountServer& server,
                                                              FakeTaskClock& clock) {
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 3000;
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

// The poll period and the real time a tick may take to settle. The poll waits its first period on the fake clock, so
// a driver whose clock is not advanced makes no tick of its own: one tick is one advance.
constexpr auto kPollPeriod = seconds(1);
constexpr auto kSettle = milliseconds(3000);

// One poll period, then wait until the woken poll is parked again.
void poll_tick(FakeTaskClock& clock) { alpacacore::test::tick(clock, kPollPeriod, kSettle); }

using alpacacore::test::parse_timestamp;
using alpacacore::test::require_alpaca_error;
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

namespace {

// Mutes the handset until the latched fault outlives the staleness bound: Connected false, the fault text kept.
void lose_link(alpacacore::TelescopeDriver& driver, Handset& st, FakeTaskClock& clock) {
    st.mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(driver);
    clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    sc::poll_status_now_for_testing(driver);
    REQUIRE_FALSE(driver.get_connected());
    REQUIRE_FALSE(driver.get_link_fault().empty());
}

}  // namespace

// open-astro#1786: the default per-axis setters read the guide rate first, and that read threw the kept fault
// (DriverException) on a lost link, ahead of both the range check and NotConnected (decision 0009 rule 3).
TEST_CASE("SynScan snapshot - a lost link refuses a guide-rate write by its value first, then as NotConnected",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    driver->set_guide_rate({0.001, 0.002});
    lose_link(*driver, *st, clock);

    for (const double bad : {2.0 * 15.0411 / 3600.0, -1.0}) {
        require_alpaca_error([&] { driver->set_guide_rate_right_ascension(bad); },
                             alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->set_guide_rate_declination(bad); }, alpacacore::AlpacaError::InvalidValue);
    }
    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(0.003); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_guide_rate_declination(0.003); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_guide_rate(); }, alpacacore::AlpacaError::NotConnected);

    st->mute = false;
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_guide_rate().ra == 0.001);
    CHECK(driver->get_guide_rate().dec == 0.002);
    driver->set_connected(false);
}

// Decision 0009 rule 3: the getters that serve no frame refuse a lost link as NotConnected, not with the fault text
// rule 5 keeps after the loss.
TEST_CASE("SynScan snapshot - a lost link refuses the getters that serve no frame as NotConnected",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    lose_link(*driver, *st, clock);

    require_alpaca_error([&] { (void)driver->get_sidereal_time(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_site_latitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_site_longitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_tracking_rate(); }, alpacacore::AlpacaError::NotConnected);
    st->mute = false;
    driver->set_connected(false);
}

// Decision 0009 rule 1: while Connected stays true on a young fault, a guide-rate read or write is refused with
// DriverException and stores nothing; a value out of range is still InvalidValue first; a good poll restores writes.
TEST_CASE("SynScan snapshot - a faulted link refuses guide-rate writes and reads and a good poll restores them",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    driver->set_guide_rate({0.001, 0.002});

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_link_fault().empty());
    REQUIRE(driver->get_connected());  // a young fault keeps Connected

    const auto fault = alpacacore::AlpacaError::DriverException;
    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(0.003); }, fault);
    require_alpaca_error([&] { driver->set_guide_rate_declination(0.003); }, fault);
    require_alpaca_error([&] { driver->set_guide_rate({0.003, 0.003}); }, fault);
    require_alpaca_error([&] { (void)driver->get_guide_rate(); }, fault);
    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(-1.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_guide_rate_declination(-1.0); }, alpacacore::AlpacaError::InvalidValue);

    st->mute = false;
    sc::poll_status_now_for_testing(*driver);
    REQUIRE(driver->get_link_fault().empty());
    CHECK(driver->get_guide_rate().ra == 0.001);  // the refused writes stored nothing
    CHECK(driver->get_guide_rate().dec == 0.002);
    driver->set_guide_rate_right_ascension(0.003);
    CHECK(driver->get_guide_rate().ra == 0.003);
    CHECK(driver->get_guide_rate().dec == 0.002);
    driver->set_connected(false);
}

// Decision 0009 rules 1 and 3 (open-astro#1786): the target pair is commanded state. A young fault refuses a read of
// a set target and a write with DriverException while Connected stays true; a lost link and an explicit disconnect are
// NotConnected; an out-of-range write is InvalidValue first; a refused write changes neither the value nor its flag.
TEST_CASE("SynScan snapshot - a faulted or lost link refuses the targets and the guide rate like the other reads",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    driver->set_target_right_ascension(5.0);  // declination stays unset
    driver->set_guide_rate({0.001, 0.002});

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_link_fault().empty());
    REQUIRE(driver->get_connected());  // a young fault keeps Connected

    const auto fault = alpacacore::AlpacaError::DriverException;
    const auto invalid = alpacacore::AlpacaError::InvalidValue;
    require_alpaca_error([&] { (void)driver->get_target_right_ascension(); }, fault);
    require_alpaca_error([&] { (void)driver->get_target_declination(); }, fault);
    require_alpaca_error([&] { driver->set_target_right_ascension(6.0); }, fault);
    require_alpaca_error([&] { driver->set_target_declination(10.0); }, fault);
    require_alpaca_error([&] { driver->set_target_right_ascension(25.0); }, invalid);
    require_alpaca_error([&] { driver->set_target_declination(95.0); }, invalid);

    st->mute = false;
    sc::poll_status_now_for_testing(*driver);
    REQUIRE(driver->get_link_fault().empty());
    CHECK(driver->get_target_right_ascension() == 5.0);  // the refused writes stored nothing
    require_alpaca_error([&] { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    driver->set_target_declination(10.0);
    CHECK(driver->get_target_declination() == 10.0);
    CHECK(driver->get_target_right_ascension() == 5.0);

    lose_link(*driver, *st, clock);
    const auto lost = alpacacore::AlpacaError::NotConnected;
    require_alpaca_error([&] { (void)driver->get_target_right_ascension(); }, lost);
    require_alpaca_error([&] { (void)driver->get_target_declination(); }, lost);
    require_alpaca_error([&] { driver->set_target_right_ascension(6.0); }, lost);
    require_alpaca_error([&] { driver->set_target_declination(11.0); }, lost);
    require_alpaca_error([&] { driver->set_target_right_ascension(25.0); }, invalid);
    st->mute = false;
    driver->set_connected(false);
}

// A connected driver answers; after an explicit disconnect the guide rate and the targets are NotConnected, and the
// rate written while connected is what the next connect serves.
TEST_CASE("SynScan snapshot - an explicit disconnect refuses the guide rate and the targets",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    driver->set_guide_rate({0.001, 0.002});
    driver->set_target_right_ascension(5.0);
    CHECK(driver->get_guide_rate().ra == 0.001);
    CHECK(driver->get_target_right_ascension() == 5.0);

    driver->set_connected(false);
    const auto lost = alpacacore::AlpacaError::NotConnected;
    require_alpaca_error([&] { (void)driver->get_guide_rate(); }, lost);
    require_alpaca_error([&] { (void)driver->get_target_right_ascension(); }, lost);
    require_alpaca_error([&] { driver->set_target_declination(10.0); }, lost);
    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(0.003); }, lost);
    require_alpaca_error([&] { driver->set_guide_rate_right_ascension(-1.0); }, alpacacore::AlpacaError::InvalidValue);
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

TEST_CASE("SynScan snapshot - a connect whose site read failed sends no second w in the same connect",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    st->w_silent = true;
    st->w_advances = &clock;  // the failed `w` itself takes longer than the retry delay
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver_without_site(server, clock, 300);
    REQUIRE(driver->get_connected());
    CHECK(st->w_count.load() == 1);  // a second `w` would cost the connect one more response timeout
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

TEST_CASE("SynScan snapshot - Tracking and GuideRate do not wait for a connect that is in flight",
          "[synscan][telescope][snapshot][connect]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 1000;
    auto driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                         false, sc::SynScanAlignmentSetting::Auto, clock);
    st->latency_ms = 200;  // every handshake exchange pays 200 ms: the connect takes seconds
    std::thread connecting([&] { driver->set_connected(true); });
    // The handshake is under way once the handset has seen a few of its commands.
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (st->transactions.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(st->transactions.load() >= 3);

    const auto swallow = [](const std::function<void()>& read) {
        try {
            read();
        } catch (const alpacacore::AlpacaException&) {  // NotConnected or "not known yet" is an answer too
        }
    };
    CHECK(timed([&] { swallow([&] { (void)driver->get_tracking(); }); }) < kGetterBudget);
    CHECK(timed([&] { swallow([&] { (void)driver->get_guide_rate(); }); }) < kGetterBudget);
    CHECK(timed([&] { swallow([&] { (void)driver->get_at_park(); }); }) < kGetterBudget);
    CHECK(timed([&] { swallow([&] { (void)driver->get_is_pulse_guiding(); }); }) < kGetterBudget);
    connecting.join();
    driver->set_connected(false);
}

// Each getter alone: the case above runs the four in a row, so the first one that waits for the connect hides whether
// the others would have waited too.
TEST_CASE("SynScan snapshot - each no-I/O getter alone is in budget while a connect is in flight",
          "[synscan][telescope][snapshot][connect]") {
    const int which = GENERATE(0, 1, 2, 3);
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 1000;
    auto driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                         false, sc::SynScanAlignmentSetting::Auto, clock);
    st->latency_ms = 200;
    std::thread connecting([&] { driver->set_connected(true); });
    const auto deadline = std::chrono::steady_clock::now() + seconds(5);
    while (st->transactions.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(st->transactions.load() >= 3);

    const auto elapsed = timed([&] {
        try {
            switch (which) {
                case 0:
                    (void)driver->get_tracking();
                    break;
                case 1:
                    (void)driver->get_guide_rate();
                    break;
                case 2:
                    (void)driver->get_at_park();
                    break;
                default:
                    (void)driver->get_is_pulse_guiding();
                    break;
            }
        } catch (const alpacacore::AlpacaException&) {  // NotConnected is an answer too
        }
    });
    CHECK(elapsed < kGetterBudget);
    connecting.join();
    driver->set_connected(false);
}

// A full status tick that is mid-exchange on a slow link: the disconnect ends it between exchanges instead of joining
// the whole tick (5 exchanges at 1.2 s = 6 s here), so it returns within 5 s. The ticks are stepped on the task clock
// so the full one is in flight whether the poll runs its first tick at once or after one period.
TEST_CASE("SynScan snapshot - set_connected(false) returns within 5 s with a status tick mid-exchange",
          "[synscan][telescope][snapshot][connect]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 3000;
    auto driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                         false, sc::SynScanAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    // Every fourth tick is a full one and the first tick of the session is full: three short ticks, then the full one.
    REQUIRE(clock.wait_for_waiters(1, milliseconds(3000)));  // the poll is parked on the task clock
    for (int short_tick = 1; short_tick < 4; ++short_tick) {
        alpacacore::test::tick(clock, seconds(1), milliseconds(3000));
    }
    st->latency_ms = 1200;
    const int before = st->transactions.load();
    clock.advance(seconds(1));
    const auto deadline = std::chrono::steady_clock::now() + seconds(2);
    while (st->transactions.load() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(st->transactions.load() > before);  // the full tick is inside its first exchange
    const auto elapsed = timed([&] { driver->set_connected(false); });
    CHECK(elapsed < seconds(5));
}

// A disconnect that aborts a full tick mid-exchange makes the probe throw, which the module counts as a failure: with
// two failed ticks just before, that is the third and latches a fault. The reset in the teardown clears it before
// Connected reads false, so a relink never starts with the old fault.
TEST_CASE("SynScan snapshot - a probe aborted by the disconnect leaves no fault behind",
          "[synscan][telescope][snapshot][connect]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = slow_link_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, kSettle));
    poll_tick(clock);  // tick 1 answers
    st->garbage = true;
    poll_tick(clock);  // ticks 2 and 3 fail: two in a row, below the threshold of three
    poll_tick(clock);
    REQUIRE(driver->get_link_fault().empty());
    st->garbage = false;
    st->latency_ms = 1200;
    const int before = st->transactions.load();
    clock.advance(kPollPeriod);  // tick 4 is a full one
    const auto deadline = std::chrono::steady_clock::now() + seconds(2);
    while (st->transactions.load() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(st->transactions.load() > before);  // the full tick is inside its first exchange
    driver->set_connected(false);
    CHECK(driver->get_link_fault().empty());
}

// A handset that answers the echo and the firmware query but nothing else still connects: the first probe returns an
// all-unknown frame, so the getters say "not known yet" instead of the connect failing.
TEST_CASE("SynScan snapshot - a handset that answers only the echo still connects with nothing known yet",
          "[synscan][telescope][snapshot][connect]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    st->mute = true;
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    for (const auto& read : {std::function<void()>([&] { (void)driver->get_tracking(); }),
                             std::function<void()>([&] { (void)driver->get_right_ascension(); })}) {
        try {
            read();
            FAIL("a read nothing answered must not return a value");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        }
    }
    CHECK(driver->get_link_fault().empty());  // the seeding probe is not a failure
    st->mute = false;
    driver->set_connected(false);
}

// Two pulse bodies and a full tick in flight at 1.2 s per reply: the bodies' stops are blind writes that do not wait
// for the slow replies, and the tick ends between exchanges, so the disconnect fits in 5 s.
TEST_CASE("SynScan snapshot - set_connected(false) returns within 5 s with two pulse bodies and a tick in flight",
          "[synscan][telescope][snapshot][connect]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = slow_link_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, kSettle));
    driver->pulse_guide(2, 600000);  // RA axis
    driver->pulse_guide(0, 600000);  // Dec axis
    for (int short_tick = 1; short_tick < 4; ++short_tick) poll_tick(clock);
    st->latency_ms = 1200;
    const int before = st->transactions.load();
    clock.advance(kPollPeriod);  // tick 4 is a full one
    const auto deadline = std::chrono::steady_clock::now() + seconds(2);
    while (st->transactions.load() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(st->transactions.load() > before);
    const auto elapsed = timed([&] { driver->set_connected(false); });
    CHECK(elapsed < seconds(5));
}

// Decision 0009 rule 2a (issue #1723): a pulled cable loses a serial link at once. The handset answers on a pty behind
// a by-id style link (ReplugPort), so a case pulls the cable mid-exchange (PtyResponder::unplug_on) or while idle
// (unplug) and plugs a new handset in under the same name. The driver runs on a FakeTaskClock, as above.
namespace {

namespace err = alpacacore::AlpacaError;
using alpacacore::test::require_alpaca_error;

// Long enough that a read which waited it out could not pass for a hangup (each such case checks the call returned in
// under half of it).
constexpr int kLinkReadTimeoutMs = 1000;
constexpr auto kAtOnce = milliseconds(kLinkReadTimeoutMs / 2);

struct SerialLinkRig {
    FakeTaskClock clock;
    std::shared_ptr<Handset> st = std::make_shared<Handset>();
    std::unique_ptr<alpacacore::test::PtyResponder> handset = plug();
    alpacacore::test::ReplugPort port{*handset};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit SerialLinkRig(bool sync_time = false) {
        sc::ConnectionInfo info;
        info.type = sc::ConnectionType::Serial;
        info.port_path = port.path.string();
        info.response_timeout_ms = kLinkReadTimeoutMs;
        driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                        sync_time, sc::SynScanAlignmentSetting::Auto, clock);
    }
    ~SerialLinkRig() {
        if (driver) driver->set_connected(false);  // the wrapper is a process-wide singleton
    }
    SerialLinkRig(const SerialLinkRig&) = delete;
    SerialLinkRig& operator=(const SerialLinkRig&) = delete;

    std::unique_ptr<alpacacore::test::PtyResponder> plug() const {
        const auto state = st;
        return std::make_unique<alpacacore::test::PtyResponder>(
            [state](const std::string& c) { return reply(*state, c); }, "SynScan handset");
    }
    // A new handset on a new pty, under the configured name.
    void replug() {
        handset = plug();
        port.replug(*handset);
    }
    // Connects and parks the poll body on the clock, so only the case's own calls reach the handset.
    void connect() {
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        sc::set_status_poll_paused_for_testing(*driver, true);
        REQUIRE(clock.wait_for_waiters(1, kSettle));
    }
    // Moves the task clock and waits until every woken body has parked on it again or ended.
    void advance(std::chrono::milliseconds by) {
        clock.advance(by);
        REQUIRE(clock.wait_for_woken_settled(kSettle));
    }
};

std::function<bool(const std::string&)> on_command(char command) {
    return [command](const std::string& chunk) { return !chunk.empty() && chunk[0] == command; };
}

// The first exchange after a GOTO (`r`) that is not itself one: the motion body's first completion read.
std::function<bool(const std::string&)> after_goto() {
    auto seen = std::make_shared<bool>(false);
    return [seen](const std::string& chunk) {
        if (!chunk.empty() && chunk[0] == 'r') {
            *seen = true;
            return false;
        }
        return *seen;
    };
}

// Rule 3: Connected false, operations NotConnected, DeviceState empty, the port released.
void check_lost(alpacacore::TelescopeDriver& driver) {
    CHECK_FALSE(driver.get_connected());
    CHECK_FALSE(sc::SynScanProtocolWrapper::instance().is_connected());
    CHECK(driver.get_device_state().empty());
    require_alpaca_error([&] { (void)driver.get_right_ascension(); }, err::NotConnected);
    require_alpaca_error([&] { (void)driver.get_slewing(); }, err::NotConnected);
    require_alpaca_error([&] { (void)driver.get_at_park(); }, err::NotConnected);
    require_alpaca_error([&] { (void)driver.get_tracking(); }, err::NotConnected);
}

}  // namespace

TEST_CASE("SynScan Link - a cable pulled during a command loses the link at once and Connect reopens it",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    const double ra_at_connect = driver.get_right_ascension();

    rig.handset->unplug_on(on_command('T'));  // the tracking write's '#' never comes: the cable is pulled first
    const auto took = timed([&] { require_alpaca_error([&] { driver.set_tracking(false); }, err::NotConnected); });
    CHECK(took < kAtOnce);
    check_lost(driver);
    const std::string reason = driver.get_link_fault();
    CHECK(reason.find("serial link lost (hangup)") != std::string::npos);

    // Rule 4: nothing relinks on its own. The poll body ends at its next tick and time reopens nothing.
    rig.replug();
    sc::set_status_poll_paused_for_testing(driver, false);
    rig.advance(seconds(10));
    CHECK(rig.clock.waiter_count() == 0);  // the poll body has ended
    CHECK(rig.handset->received().empty());
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_link_fault() == reason);  // rule 5: kept until the next Connect or Disconnect

    // The client's Connect reopens the replugged port without a Disconnect first, and clears the reason.
    driver.set_connected(true);
    CHECK(driver.get_connected());
    CHECK(driver.get_link_fault().empty());
    CHECK(std::abs(driver.get_right_ascension() - ra_at_connect) < 1e-9);
}

TEST_CASE("SynScan Link - a write to a pulled mount loses the link at once and Disconnect clears the reason",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    rig.handset->unplug();  // idle: the next command's write meets the dead port (EIO)

    SECTION("a command that waits for its reply") {
        require_alpaca_error([&] { driver.set_tracking(false); }, err::NotConnected);
    }
    SECTION("a fire-and-forget command") {
        require_alpaca_error([&] { (void)driver.command_blind("M", true); }, err::NotConnected);
    }
    check_lost(driver);
    CHECK(driver.get_link_fault().find("serial link lost (write)") != std::string::npos);
    driver.set_connected(false);
    CHECK(driver.get_link_fault().empty());
}

TEST_CASE("SynScan Link - a hangup in a fire-and-forget command's reply drain fails that call and Connect reopens it",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    rig.handset->unplug_on(on_command('M'));  // the drain after the write hangs up instead of going quiet
    require_alpaca_error([&] { (void)driver.command_blind("M", true); }, err::NotConnected);
    check_lost(driver);
    // The paused poll has not run since, so only the wrapper knows the link is lost: Connect still relinks.
    rig.replug();
    driver.set_connected(true);
    CHECK(driver.get_connected());
}

TEST_CASE("SynScan Link - a cable pulled during the status poll ends the poll in that tick",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    sc::set_status_poll_paused_for_testing(driver, false);
    SECTION("the position read") { rig.handset->unplug_on(on_command('e')); }
    SECTION("the pier-side read of a full tick, whose failure the sample does without") {
        for (int short_tick = 1; short_tick < 4; ++short_tick) rig.advance(kPollPeriod);
        rig.handset->unplug_on(on_command('p'));  // tick 4 is a full one
    }
    rig.advance(kPollPeriod);
    REQUIRE(rig.handset->unplugged());
    CHECK(rig.clock.waiter_count() == 0);  // the poll body ended in the tick that met the pull
    check_lost(driver);
    CHECK(driver.get_link_fault().find("serial link lost (hangup)") != std::string::npos);
}

TEST_CASE("SynScan Link - a cable pulled during the connect fails the Connect with NotConnected",
          "[synscan][telescope][unit][fake][link-lost]") {
    const bool sync_time = GENERATE(false, true);  // with the time write, set_time() would meet the released port
    CAPTURE(sync_time);
    SerialLinkRig rig(sync_time);
    auto& driver = *rig.driver;
    rig.handset->unplug_on(on_command('V'));  // the first handshake query; the connect swallows its failure
    require_alpaca_error([&] { driver.set_connected(true); }, err::NotConnected);
    CHECK_FALSE(driver.get_connected());
    CHECK_FALSE(sc::SynScanProtocolWrapper::instance().is_connected());
    rig.replug();
    driver.set_connected(true);
    CHECK(driver.get_connected());
}

TEST_CASE("SynScan Link - a cable pulled mid-slew loses the link and the slew body ends",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    const auto idle = rig.clock.waiter_count();  // the paused poll body
    rig.handset->unplug_on(after_goto());
    driver.slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, kSettle));  // the GOTO is out, the body waits
    rig.advance(milliseconds(8250));  // past the slew's forced-Slewing window: its completion read meets the pull
    REQUIRE(rig.handset->unplugged());
    check_lost(driver);
    CHECK(rig.clock.waiter_count() == idle);  // the body ended on its connection check
}

TEST_CASE("SynScan Link - a cable pulled mid-park loses the link and the park body ends on its own",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    const auto idle = rig.clock.waiter_count();
    rig.handset->unplug_on(after_goto());
    driver.park();
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, kSettle));
    rig.advance(milliseconds(8250));  // past the park's forced-Slewing window: it reads the handset
    REQUIRE(rig.handset->unplugged());
    check_lost(driver);
    // Each further completion read fails at once on the released port; the third in a row ends the body.
    for (int i = 0; i < 4 && rig.clock.waiter_count() > idle; ++i) rig.advance(milliseconds(250));
    CHECK(rig.clock.waiter_count() == idle);
    CHECK_FALSE(driver.get_connected());
}

TEST_CASE("SynScan Link - a cable pulled at a pulse's end loses the link and the pulse body ends",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();
    const auto idle = rig.clock.waiter_count();
    rig.handset->unplug_on(on_command('T'));  // an RA pulse on a tracking mount ends with the tracking write
    driver.pulse_guide(2, 5000);
    REQUIRE(rig.clock.wait_for_waiters(idle + 1, kSettle));
    rig.advance(milliseconds(5000));
    REQUIRE(rig.handset->unplugged());
    check_lost(driver);
    require_alpaca_error([&] { (void)driver.get_is_pulse_guiding(); }, err::NotConnected);
    // The two retries fail at once on the released port, 100 ms apart; then the body ends.
    for (int i = 0; i < 4 && rig.clock.waiter_count() > idle; ++i) rig.advance(milliseconds(100));
    CHECK(rig.clock.waiter_count() == idle);
}

// A Tracking write queued behind the exchange that meets the pull: its turn comes after the wrapper has latched the
// loss and disconnected, so it must read NotConnected, not send and surface the wrapper's "Not connected" as a
// DriverException. No sleeps: the holder's pull waits on a latch until the second writer reports (the wire-turn seam)
// that it has taken its ticket.
TEST_CASE("SynScan Link - a Tracking write waiting its turn when the cable is pulled gets NotConnected",
          "[synscan][telescope][unit][fake][link-lost]") {
    SerialLinkRig rig;
    auto& driver = *rig.driver;
    rig.connect();

    std::promise<void> on_wire;
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto fired = std::make_shared<std::atomic<bool>>(false);
    rig.handset->unplug_on([&on_wire, release_future, fired](const std::string& chunk) {
        if (chunk.empty() || chunk[0] != 'T' || fired->exchange(true)) return false;
        on_wire.set_value();  // the holder's tracking write is on the wire
        release_future.wait();
        return true;
    });

    std::promise<void> queued;
    std::atomic<bool> queued_set{false};
    std::atomic<int> holder_code{-1};
    std::atomic<int> waiter_code{-1};
    const auto run = [&](std::atomic<int>& code) {
        try {
            driver.set_tracking(false);
            code = 0;
        } catch (const alpacacore::AlpacaException& e) {
            code = e.error_code();
        }
    };
    std::thread holder([&] { run(holder_code); });
    std::thread waiter;
    // A failed REQUIRE unwinds the case: free the latch and join, so no joinable thread reaches std::terminate.
    struct Cleanup {
        std::promise<void>& release;
        std::atomic<bool>& released;
        std::thread& holder;
        std::thread& waiter;
        ~Cleanup() {
            if (!released.exchange(true)) release.set_value();
            if (holder.joinable()) holder.join();
            if (waiter.joinable()) waiter.join();
        }
    };
    std::atomic<bool> released{false};
    Cleanup cleanup{release, released, holder, waiter};

    REQUIRE(on_wire.get_future().wait_for(kSettle) == std::future_status::ready);
    sc::set_wire_turn_wait_hook_for_testing(driver, [&] {
        if (!queued_set.exchange(true)) queued.set_value();
    });
    waiter = std::thread([&] { run(waiter_code); });
    REQUIRE(queued.get_future().wait_for(kSettle) == std::future_status::ready);
    sc::set_wire_turn_wait_hook_for_testing(driver, nullptr);

    released = true;
    release.set_value();
    holder.join();
    waiter.join();
    REQUIRE(rig.handset->unplugged());
    CHECK(holder_code == err::NotConnected);
    CHECK(waiter_code == err::NotConnected);
    CHECK_FALSE(driver.get_connected());
}

// Over TCP (SynScan Wi-Fi) a hangup is not a pulled cable: it stays a DriverException and the fault latch and the
// staleness bound decide, as before.
TEST_CASE("SynScan Link - over TCP a hangup is a failed exchange, not a lost link",
          "[synscan][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = slow_link_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);

    st->mute = true;
    const int before = st->transactions.load();
    int code = 0;
    std::thread call([&] {
        try {
            driver->set_tracking(false);
        } catch (const alpacacore::AlpacaException& e) {
            code = e.error_code();
        }
    });
    // A failed REQUIRE below unwinds the case; joining here keeps a joinable thread from reaching std::terminate.
    struct JoinOnExit {
        std::thread& t;
        ~JoinOnExit() {
            if (t.joinable()) t.join();
        }
    } join_call{call};
    const auto deadline = std::chrono::steady_clock::now() + seconds(2);
    while (st->transactions.load() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(st->transactions.load() > before);
    REQUIRE(server.drop_connections());  // the read in flight meets the reset
    call.join();
    CHECK(code == err::DriverException);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

#endif

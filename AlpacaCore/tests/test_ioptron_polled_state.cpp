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

// The iOptron telescope serves position, Alt/Az and status from one polled frame (issue #2491, decision 0010 rule 7):
// one poll per connection reads :GLS#, :GEP# and :GAC# every 250 ms, a getter reads the frame and sends nothing, a
// client write goes through the frame, three failed polls latch a fault, a fault standing for
// util::kLinkStalenessBound loses the link, and the poll alone ends a driver-started FindHome or Park. The mount fake
// answers over TCP (or a pty for the cable pull); every driver wait runs on a FakeTaskClock, so a tick is an
// advance() of one period. The wire timing the cases measure (the held reply, the getter budget) is real time:
// decision 0005 leaves the I/O timeouts real.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_ioptron_mount.h"
#include "fake_task_clock.h"

namespace {

namespace ioptron = alpacacore::vendor::ioptron;
namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::TelescopeDriver;
using alpacacore::test::FakeIoptronMount;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::PtyIoptronMount;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

// Decision 0010 and issue #2491 rule 9: the poll runs every 250 ms, start to start.
constexpr milliseconds kPollPeriod{250};
// Real-time bound a rig waits for the poll to park on the clock (and for a woken poll to settle).
constexpr milliseconds kRigBound{3000};
// Issue #2491 test 1: a getter answers within this while the mount holds every reply for kReplyDelay.
constexpr milliseconds kGetterBudget{20};
constexpr milliseconds kReplyDelay{200};

ioptron::ConnectionInfo tcp_endpoint(int port) {
    ioptron::ConnectionInfo info;
    info.type = ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    return info;
}

ioptron::ConnectionInfo serial_endpoint(const std::string& path) {
    ioptron::ConnectionInfo info;
    info.type = ioptron::ConnectionType::Serial;
    info.port_path = path;
    return info;
}

// No site or time sync on connect, so the clock-sync body sends nothing and the poll is the only reader that wakes
// when a case advances the clock.
std::unique_ptr<TelescopeDriver> make_driver(const ioptron::ConnectionInfo& info, FakeTaskClock& clock) {
    return ioptron::create_ioptron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt,
                                                       /*sync_time_on_connect=*/false, clock);
}

using TcpRig = alpacacore::test::PolledRig<FakeIoptronMount, TelescopeDriver>;
using PtyRig = alpacacore::test::PolledRig<PtyIoptronMount, TelescopeDriver>;

TcpRig make_tcp_rig() {
    return TcpRig({kPollPeriod, kRigBound}, [](FakeIoptronMount& mount, FakeTaskClock& clock) {
        return make_driver(tcp_endpoint(mount.port()), clock);
    });
}

// Moves the task clock by `d` and waits for whatever it woke (the poll) to park on the clock again. No assertion: a
// driver with no poll wakes nothing, and the case then fails on what it checks.
void step(FakeTaskClock& clock, std::chrono::nanoseconds d) {
    clock.advance(d);
    static_cast<void>(clock.wait_for_woken_settled(kRigBound));
}

// One poll period: exactly one poll cycle runs and finishes.
void tick(FakeTaskClock& clock) { step(clock, kPollPeriod); }

// Past the 30 s connect grace and every TTL of today's caches, in steps well under util::kLinkStalenessBound, so a
// polled frame is refreshed at each step (one 31 s jump would let a getter read a frame older than the bound).
void age_past_connect_grace(FakeTaskClock& clock) {
    for (int i = 0; i < 7; ++i) {
        step(clock, seconds(5));
    }
}

// Every exchange the mount received, as a count: a getter that sends one changes it.
std::size_t sent(FakeIoptronMount& mount) { return mount.commands().size(); }

// The reads a poll sends, in any order: a tick that stops at its first failed exchange still adds at least one.
int poll_reads(FakeIoptronMount& mount) { return mount.count(":GLS#") + mount.count(":GEP#") + mount.count(":GAC#"); }

// One tick, and the poll behind it read the mount: a driver with no poll reads nothing, and the case fails here.
void tick_and_require_a_poll(FakeTaskClock& clock, FakeIoptronMount& mount) {
    const int before = poll_reads(mount);
    tick(clock);
    CHECK(poll_reads(mount) > before);
}

struct Getter {
    const char* label;
    std::function<void(TelescopeDriver&)> read;
};

// The getters of the issue's table: position, Alt/Az and status, one per row.
std::vector<Getter> served_getters() {
    return {
        {"Altitude", [](TelescopeDriver& d) { static_cast<void>(d.get_altitude()); }},
        {"Azimuth", [](TelescopeDriver& d) { static_cast<void>(d.get_azimuth()); }},
        {"RightAscension", [](TelescopeDriver& d) { static_cast<void>(d.get_right_ascension()); }},
        {"Declination", [](TelescopeDriver& d) { static_cast<void>(d.get_declination()); }},
        {"SideOfPier", [](TelescopeDriver& d) { static_cast<void>(d.get_side_of_pier()); }},
        {"AtHome", [](TelescopeDriver& d) { static_cast<void>(d.get_at_home()); }},
        {"AtPark", [](TelescopeDriver& d) { static_cast<void>(d.get_at_park()); }},
        {"Slewing", [](TelescopeDriver& d) { static_cast<void>(d.get_slewing()); }},
        {"TrackingRate", [](TelescopeDriver& d) { static_cast<void>(d.get_tracking_rate()); }},
        {"Tracking", [](TelescopeDriver& d) { static_cast<void>(d.get_tracking()); }},
        {"DeviceState", [](TelescopeDriver& d) { static_cast<void>(d.get_device_state()); }},
    };
}

// Each getter answers in under kGetterBudget and sends nothing to the mount. A getter that throws fails the case.
void check_getters_answer_from_the_frame(TelescopeDriver& driver, FakeIoptronMount& mount) {
    for (const auto& getter : served_getters()) {
        INFO(getter.label);
        const auto before = sent(mount);
        const auto elapsed = alpacacore::test::timed([&] { CHECK_NOTHROW(getter.read(driver)); });
        CHECK(elapsed < kGetterBudget);
        CHECK(sent(mount) == before);
    }
}

// Blocks until the async slew body has sent one more :MS1# than `sent_before`, or 5 s of real time pass.
bool wait_for_goto_dispatch(FakeIoptronMount& mount, int sent_before) {
    const auto by = steady_clock::now() + seconds(5);
    while (mount.count(":MS1#") <= sent_before) {
        if (steady_clock::now() >= by) return false;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return true;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The members every case below expects to refuse with NotConnected once the link is lost.
void require_all_members_not_connected(TelescopeDriver& driver) {
    using alpacacore::test::require_alpaca_error;
    for (const auto& getter : served_getters()) {
        if (std::string(getter.label) == "DeviceState") continue;  // the empty list, checked on its own
        INFO(getter.label);
        require_alpaca_error([&] { getter.read(driver); }, AlpacaError::NotConnected);
    }
    require_alpaca_error([&] { static_cast<void>(driver.get_is_pulse_guiding()); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver.set_tracking(true); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver.abort_slew(); }, AlpacaError::NotConnected);
    CHECK(driver.get_device_state().empty());
}

}  // namespace

// Issue #2491 test group 1, the reproduction: with the mount holding every reply, a getter that has to ask the mount
// pays for it. Today every cache lapses past the 30 s connect grace and its 1 s TTL, so each getter of the table sends
// an exchange and waits 200 ms for it; served from the polled frame it answers from memory (decision 0010 rule 1).
TEST_CASE(
    "iOptron polled state - every getter answers in under 20 ms and sends nothing while the mount holds each reply",
    "[ioptron][telescope][polled][getter-locks]") {
    auto rig = make_tcp_rig();
    rig.fake().set_alt_az(45.0, 180.0);
    REQUIRE(rig.driver().get_connected() == false);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    age_past_connect_grace(rig.clock());

    rig.fake().set_reply_delay(kReplyDelay);
    check_getters_answer_from_the_frame(rig.driver(), rig.fake());
    rig.fake().set_reply_delay(milliseconds(0));
}

// Ticket ALP-3351, "Added scope": a raw CommandString holds the operation lock for its exchange and nothing else; a
// getter must not wait for it. (command_blind, command_bool and command_string already run under the I/O lock with
// mutex_ released since #1829/#2036; what keeps a getter behind them today is a lapsed cache.)
TEST_CASE("iOptron polled state - a getter answers in under 20 ms while a raw CommandString exchange is held",
          "[ioptron][telescope][polled][getter-locks]") {
    auto rig = make_tcp_rig();
    rig.fake().set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    age_past_connect_grace(rig.clock());

    rig.fake().hold_next_reply(milliseconds(1500));
    std::atomic<bool> command_ok{false};
    std::thread raw([&] {
        try {
            static_cast<void>(rig.driver().command_string(":GLS#", true));
            command_ok = true;
        } catch (const std::exception&) {  // the case checks command_ok below
        }
    });
    REQUIRE(rig.fake().wait_until_holding(seconds(5)));
    const auto held_from = steady_clock::now();

    const auto before = sent(rig.fake());
    check_getters_answer_from_the_frame(rig.driver(), rig.fake());
    CHECK(rig.driver().get_connected());
    CHECK(rig.driver().get_name() == "iOptron HAE29C EQ");
    CHECK(sent(rig.fake()) == before);
    // Every sample above landed while the held reply still kept the raw exchange on the wire.
    CHECK(steady_clock::now() - held_from < milliseconds(1500));

    raw.join();
    CHECK(command_ok);
}

// Issue #2491 rule 1 and rule 9: one poll per connection reads the status, the position and the Alt/Az, every 250 ms
// start to start. Half a period is no poll; a period is one of each read.
TEST_CASE("iOptron polled state - one poll reads status, position and Alt/Az every 250 ms",
          "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    rig.fake().set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& mount = rig.fake();

    const int status = mount.count(":GLS#");
    const int position = mount.count(":GEP#");
    const int altaz = mount.count(":GAC#");

    step(rig.clock(), kPollPeriod / 2);
    CHECK(mount.count(":GLS#") == status);
    CHECK(mount.count(":GEP#") == position);
    CHECK(mount.count(":GAC#") == altaz);

    step(rig.clock(), kPollPeriod / 2);
    CHECK(mount.count(":GLS#") == status + 1);
    CHECK(mount.count(":GEP#") == position + 1);
    CHECK(mount.count(":GAC#") == altaz + 1);

    tick(rig.clock());
    CHECK(mount.count(":GLS#") == status + 2);
    CHECK(mount.count(":GEP#") == position + 2);
    CHECK(mount.count(":GAC#") == altaz + 2);
}

// Issue #2491 test group 2: a read after a client write reports the written state, from the frame, before the poll
// that comes after the write could say otherwise. The fake does not model the change (its status and position stay
// as they were), so a poll sample taken after the write is the pre-write state: the written value must still hold
// inside its window, and the read sends nothing.
TEST_CASE("iOptron polled state - a read after Tracking, Park, Unpark, a slew, AbortSlew and a Sync reports the write",
          "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    mount.set_position(12.0, 20.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    tick(clock);

    SECTION("Tracking off") {
        REQUIRE(driver.get_tracking());
        driver.set_tracking(false);
        const auto before = sent(mount);
        CHECK_FALSE(driver.get_tracking());
        CHECK(sent(mount) == before);
        tick_and_require_a_poll(clock, mount);  // the poll after the write reads the mount's old status
        CHECK_FALSE(driver.get_tracking());
    }

    SECTION("Unpark") {
        driver.set_connected(false);
        mount.set_system_status(6);  // parked, from the connect on
        REQUIRE(rig.connect());
        REQUIRE(driver.get_at_park());
        driver.unpark();
        const auto before = sent(mount);
        CHECK_FALSE(driver.get_at_park());
        CHECK(sent(mount) == before);
        tick_and_require_a_poll(clock, mount);  // still reads parked at the mount: the Unpark override holds
        CHECK_FALSE(driver.get_at_park());
    }

    SECTION("Park") {
        driver.park();
        const auto before = sent(mount);
        CHECK(driver.get_slewing());
        CHECK_FALSE(driver.get_at_park());
        CHECK_FALSE(driver.get_at_home());
        CHECK(sent(mount) == before);
        tick_and_require_a_poll(clock, mount);  // it reads a mount that is not moving: the dispatch window holds
        CHECK(driver.get_slewing());
    }

    SECTION("SlewToCoordinatesAsync") {
        const int goto_before = mount.count(":MS1#");
        driver.slew_to_coordinates_async(13.0, 25.0);
        REQUIRE(wait_for_goto_dispatch(mount, goto_before));
        const auto before = sent(mount);
        CHECK(driver.get_slewing());
        CHECK(sent(mount) == before);
        tick_and_require_a_poll(clock, mount);
        CHECK(driver.get_slewing());
        driver.abort_slew();
    }

    SECTION("AbortSlew") {
        const int goto_before = mount.count(":MS1#");
        mount.set_status_on_command(":Q#", 1);  // the firmware leaves the slew when it is stopped
        driver.slew_to_coordinates_async(13.0, 25.0);
        REQUIRE(wait_for_goto_dispatch(mount, goto_before));
        mount.set_system_status(2);  // slewing
        tick_and_require_a_poll(clock, mount);
        REQUIRE(driver.get_slewing());
        driver.abort_slew();
        const auto before = sent(mount);
        CHECK_FALSE(driver.get_slewing());  // the frame sampled before the stop said slewing
        CHECK(sent(mount) == before);
        tick_and_require_a_poll(clock, mount);  // the stop took: the mount reads stopped again
        CHECK_FALSE(driver.get_slewing());
    }

    SECTION("SyncToCoordinates") {
        driver.sync_to_coordinates(5.0, 10.0);
        const auto before = sent(mount);
        CHECK(driver.get_right_ascension() == Catch::Approx(5.0).margin(1e-6));
        CHECK(driver.get_declination() == Catch::Approx(10.0).margin(1e-6));
        CHECK(sent(mount) == before);
        tick_and_require_a_poll(clock, mount);  // it measures the same mount position; the Sync offset still applies
        CHECK(driver.get_right_ascension() == Catch::Approx(5.0).margin(1e-6));
        CHECK(driver.get_declination() == Catch::Approx(10.0).margin(1e-6));
    }
}

// Issue #2491 rule 5: a pulse on one axis holds the other axis's reported position at its last read for the pulse, and
// a poll that measures meanwhile does not replace the hold. The hold and its prediction are driver state, not frame
// fields.
TEST_CASE("iOptron polled state - a poll does not replace the position a pulse guide holds",
          "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    mount.set_position(12.0, 20.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    tick(rig.clock());
    const double dec_before = driver.get_declination();
    REQUIRE(dec_before == Catch::Approx(20.0).margin(1e-6));

    driver.pulse_guide(2, 1000);     // RA pulse: Declination is held at its last read
    mount.set_position(12.0, 20.5);  // the poll below measures a different Dec
    tick_and_require_a_poll(rig.clock(), mount);
    const auto before = sent(mount);
    CHECK(driver.get_declination() == Catch::Approx(dec_before).margin(1e-6));
    CHECK(sent(mount) == before);
}

// Issue #2491 test group 3: three failed polls latch the fault and every getter answers DriverException; one good poll
// clears it with no getter call in between (decision 0009 rule 1). Two failed polls are not a latch: the frame ages
// and is served as it stands (decision 0010 point 6).
TEST_CASE("iOptron polled state - three failed polls latch a fault and one good poll clears it",
          "[ioptron][telescope][polled][link-fault]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    age_past_connect_grace(clock);

    mount.set_fail_reads(true);
    tick_and_require_a_poll(clock, mount);
    tick_and_require_a_poll(clock, mount);  // the poll tried twice
    CHECK(driver.get_link_fault().empty());
    check_getters_answer_from_the_frame(driver, mount);  // two failures: the frame is served

    tick_and_require_a_poll(clock, mount);
    CHECK(driver.get_connected());  // faulted, not lost
    CHECK_FALSE(driver.get_link_fault().empty());
    for (const auto& getter : served_getters()) {
        if (std::string(getter.label) == "DeviceState") continue;
        INFO(getter.label);
        std::string message;
        const auto before = sent(mount);
        CHECK(alpacacore::test::error_code_of([&] { getter.read(driver); }, &message) == AlpacaError::DriverException);
        CHECK(contains(message, "communications compromised"));
        CHECK(sent(mount) == before);  // a faulted getter refuses; it does not probe
    }

    mount.set_fail_reads(false);
    tick_and_require_a_poll(clock, mount);  // the poll alone clears it: no getter ran since the latch
    CHECK(driver.get_link_fault().empty());
    check_getters_answer_from_the_frame(driver, mount);
}

// #728 with the poll's latch: AbortSlew is the one member a latched fault does not refuse, so the stop still goes
// out, with no status read in front of it. The sent stop is not a poll and does not clear the latch; the next good
// poll does, with no client call (decision 0009 rule 1).
TEST_CASE("iOptron polled state - AbortSlew sends the stop on a latch the poll set, and the next good poll clears it",
          "[ioptron][telescope][polled][link-fault]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    age_past_connect_grace(clock);

    mount.set_fail_reads(true);
    for (int i = 0; i < 3; ++i) tick_and_require_a_poll(clock, mount);
    REQUIRE_FALSE(driver.get_link_fault().empty());

    const int stops = mount.count(":Q#");
    const int status_reads = mount.count(":GLS#");
    REQUIRE_NOTHROW(driver.abort_slew());
    CHECK(mount.count(":Q#") == stops + 1);
    CHECK(mount.count(":GLS#") == status_reads);
    CHECK_FALSE(driver.get_link_fault().empty());  // the stop is not a good poll
    alpacacore::test::require_alpaca_error([&] { static_cast<void>(driver.get_tracking()); },
                                           AlpacaError::DriverException);

    mount.set_fail_reads(false);
    tick_and_require_a_poll(clock, mount);
    CHECK(driver.get_link_fault().empty());
    CHECK_NOTHROW(driver.get_tracking());
}

// Issue #2491 rule 2: a frame older than max_age (1 s) is served as it is, never refused: one failed poll that
// leaves the frame 2 s old is a stale reading, not a fault.
TEST_CASE("iOptron polled state - a stale frame is served, never refused", "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    mount.set_position(12.0, 20.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    age_past_connect_grace(rig.clock());
    const double ra = driver.get_right_ascension();
    const double altitude = driver.get_altitude();

    mount.set_fail_reads(true);
    step(rig.clock(), seconds(2));  // one poll, which fails; the frame is now 2 s old
    CHECK(driver.get_link_fault().empty());
    const auto before = sent(mount);
    CHECK(driver.get_right_ascension() == Catch::Approx(ra).margin(1e-9));
    CHECK(driver.get_altitude() == Catch::Approx(altitude).margin(1e-9));
    CHECK_NOTHROW(driver.get_slewing());
    CHECK_FALSE(driver.get_device_state().empty());
    CHECK(sent(mount) == before);
}

// Issue #2491 test group 4 and rule 6: a fault that stands for util::kLinkStalenessBound is a lost link. Connected
// drops once, every member answers NotConnected, the port is released and the poll ends: nothing more is sent.
TEST_CASE("iOptron polled state - a fault that stands for the staleness bound drops Connected once",
          "[ioptron][telescope][polled][link-lost]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    age_past_connect_grace(clock);

    mount.set_fail_reads(true);
    for (int i = 0; i < 3; ++i) tick(clock);
    REQUIRE_FALSE(driver.get_link_fault().empty());
    REQUIRE(driver.get_connected());  // faulted for under the bound: still connected

    const auto bound_steps = static_cast<int>(alpacacore::util::kLinkStalenessBound / seconds(5)) + 1;
    for (int i = 0; i < bound_steps; ++i)
        step(clock, seconds(5));  // the fault now stands past the bound, no client call
    CHECK_FALSE(driver.get_connected());
    CHECK_FALSE(ioptron::iOptronProtocolWrapper::instance().is_connected());  // the port is released
    require_all_members_not_connected(driver);
    CHECK_FALSE(driver.get_link_fault().empty());  // the reason is kept until the next Connect

    const auto reads = poll_reads(mount);
    step(clock, seconds(30));  // the poll ended with the loss: no autonomous reconnect, nothing sent
    CHECK(poll_reads(mount) == reads);
    CHECK_FALSE(driver.get_connected());
}

// Decision 0009 rule 2a with the poll: a cable pulled while no client calls is evidence the transport is gone, found
// by the next poll's exchange; Connected drops at once and no getter had to meet the pull.
TEST_CASE("iOptron polled state - a cable pulled between polls loses the link at the next poll",
          "[ioptron][telescope][polled][link-lost]") {
    PtyRig rig({kPollPeriod, kRigBound}, [](PtyIoptronMount& mount, FakeTaskClock& clock) {
        return make_driver(serial_endpoint(mount.slave_path()), clock);
    });
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());

    rig.fake().unplug();
    tick(rig.clock());
    CHECK_FALSE(rig.driver().get_connected());
    CHECK_FALSE(ioptron::iOptronProtocolWrapper::instance().is_connected());
    require_all_members_not_connected(rig.driver());
}

// Issue #2491 test group 5 and rule 3: a driver-started FindHome ends from the poll alone. HEM27 (0025) goes home with
// :MH#; the mount says it is home (status 7) and no client call follows the command until the case reads the result.
TEST_CASE("iOptron polled state - a driver-started FindHome ends from the poll alone", "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_model_code("0025");  // HEM27: :MH# go-to-zero FindHome
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    age_past_connect_grace(clock);

    driver.find_home();
    mount.set_system_status(7);  // arrived at home
    const int reads = poll_reads(mount);
    for (int i = 0; i < 24; ++i) tick(clock);  // past the 5 s dispatch window, polls only
    CHECK(poll_reads(mount) >= reads + 24);

    const auto before = sent(mount);
    CHECK(driver.get_at_home());
    CHECK_FALSE(driver.get_slewing());
    CHECK(sent(mount) == before);
    // The motion the driver started is over: a pulse is no longer refused for it (#2504).
    CHECK_NOTHROW(driver.pulse_guide(2, 100));
}

// Issue #2491 test group 5 and rule 3: the HAE29C wedged-park finalizer runs from the poll. The park slew is over, the
// firmware keeps saying "slewing", and the mount stands on the park position: the poll sees that, sends :ST0#, and the
// mount then reports parked, with no client call after Park.
TEST_CASE("iOptron polled state - a wedged Park is finalized from the poll alone", "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();  // HAE29C (0036)
    mount.set_alt_az(30.0, 100.0);
    mount.set_park_position_reply("00000000000000000#");  // park position: Alt 0, Az 0
    mount.set_status_on_command(":ST0#", 6);              // :ST0# ends the wedge: parked
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    age_past_connect_grace(clock);

    REQUIRE_NOTHROW(driver.park());
    mount.set_alt_az(0.0, 0.0);  // the park slew arrived
    mount.set_system_status(2);  // and the firmware still reports slewing
    const int stops = mount.count(":ST0#");
    const int reads = poll_reads(mount);
    for (int i = 0; i < 24; ++i) tick(clock);  // past the 5 s dispatch window, polls only
    CHECK(poll_reads(mount) >= reads + 24);

    CHECK(mount.count(":ST0#") == stops + 1);
    const auto before = sent(mount);
    CHECK(driver.get_at_park());
    CHECK_FALSE(driver.get_slewing());
    CHECK(sent(mount) == before);
}

// The async landing gate runs in the poll (FINDINGS 1 of the plan). A GOTO at rest far from its target is a failed
// operation, not a failed poll: the poll stores the error for Slewing to raise (the ASCOM async "Completion
// property"), publishes the tick's frame (DeviceState's TimeStamp is that tick) and latches no fault however many
// ticks follow. HEM27 (0025), so no GOTO trim reads run.
TEST_CASE("iOptron polled state - a far miss the poll decides is raised by Slewing and is not a link fault",
          "[ioptron][telescope][polled][goto-safety]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_model_code("0025");
    mount.set_landing_ra_error(3600.0);  // the GOTO lands 1 degree of RA east, and the status reads tracking
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();
    auto& clock = rig.clock();
    age_past_connect_grace(clock);

    const int goto_before = mount.count(":MS1#");
    driver.slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount, goto_before));
    const int reads = poll_reads(mount);
    for (int i = 0; i < 24; ++i) tick(clock);  // past the 5 s dispatch window and the landing samples, polls only
    CHECK(poll_reads(mount) >= reads + 24);

    const auto before = sent(mount);
    std::string message;
    CHECK(alpacacore::test::error_code_of([&] { static_cast<void>(driver.get_slewing()); }, &message) ==
          AlpacaError::DriverException);
    CHECK(contains(message, "from the target"));
    CHECK(driver.get_connected());
    CHECK(driver.get_link_fault().empty());
    const auto state = driver.get_device_state();
    REQUIRE_FALSE(state.empty());
    const auto stamped = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(state));
    CHECK(std::chrono::system_clock::now() - stamped < kPollPeriod);
    CHECK(sent(mount) == before);
    CHECK(mount.count(":Q#") == 0);  // at rest: no stop
}

// Issue #2491 rule 8 and decision 0010 rule 5: DeviceState's TimeStamp is when the frame was measured. A poll that
// fails leaves the frame as it was, so after 600 ms of failed polls (two) the stamp is 600 ms in the past, not now.
TEST_CASE("iOptron polled state - DeviceState TimeStamp is the measurement time", "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());
    auto& driver = rig.driver();

    mount.set_fail_reads(true);
    tick(rig.clock());
    tick(rig.clock());
    step(rig.clock(), milliseconds(100));  // the frame is 600 ms old on the task clock; two failed polls: no latch
    REQUIRE(driver.get_link_fault().empty());

    const auto state = driver.get_device_state();
    REQUIRE_FALSE(state.empty());
    const auto stamped = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(state));
    const auto behind = std::chrono::system_clock::now() - stamped;
    CHECK(behind >= milliseconds(550));
    CHECK(behind < seconds(5));
}

// Issue #2491 acceptance 5 and the ticket's "Tests first": Disconnect returns within 5 s while the poll is in the
// middle of an exchange. The poll holds the operation lock for its reads; Disconnect cancels and joins it before it
// closes the port, so it waits for that one exchange (held 1.5 s here) and no longer. Every reply after the held one
// takes 4 s: a poll that went on to its next read, or to an effect's exchange, once Disconnect asked it to stop would
// hold Disconnect past 5 s (the probe gets no cancel context, so the driver's own stop flag ends it).
TEST_CASE("iOptron polled state - Disconnect returns within 5 s while the poll is mid-exchange",
          "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());

    mount.hold_next_reply(milliseconds(1500));
    rig.clock().advance(kPollPeriod);  // wakes the poll: its first read is the held one
    REQUIRE(mount.wait_until_holding(seconds(5)));
    mount.set_reply_delay(seconds(4));

    const auto elapsed = alpacacore::test::timed([&] { rig.driver().set_connected(false); });
    mount.set_reply_delay(milliseconds(0));
    CHECK(elapsed < seconds(5));
    CHECK_FALSE(rig.driver().get_connected());
}

// The same bound with a client's exchange in flight (no poll needed): Disconnect waits for that one exchange.
TEST_CASE("iOptron polled state - Disconnect returns within 5 s while a raw CommandString exchange is held",
          "[ioptron][telescope][polled]") {
    auto rig = make_tcp_rig();
    auto& mount = rig.fake();
    mount.set_alt_az(45.0, 180.0);
    static_cast<void>(rig.connect());
    REQUIRE(rig.driver().get_connected());

    mount.hold_next_reply(milliseconds(1500));
    std::thread raw([&] {
        try {
            static_cast<void>(rig.driver().command_string(":GLS#", true));
        } catch (const std::exception&) {  // a command the Disconnect cut short may throw
        }
    });
    REQUIRE(mount.wait_until_holding(seconds(5)));
    const auto elapsed = alpacacore::test::timed([&] { rig.driver().set_connected(false); });
    raw.join();
    CHECK(elapsed < seconds(5));
    CHECK_FALSE(rig.driver().get_connected());
}

#endif  // _WIN32

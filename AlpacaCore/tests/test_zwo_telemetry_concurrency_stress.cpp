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

#ifndef _WIN32
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#include "fake_zwo_mount.h"
#include "pty_responder.h"

namespace {
using namespace std::chrono_literals;
using namespace alpacacore;
using alpacacore::test::time_stamp_of;

bool eventually(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::yield();
    }
    return predicate();
}

void error(const std::function<void()>& call, int code, const std::string& text = {}) {
    try {
        call();
        FAIL("Expected AlpacaException");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == code);
        if (!text.empty()) CHECK(std::string(ex.what()).find(text) != std::string::npos);
    }
}

// One AM5N on the clock the driver reads. The knobs below translate a test's intent into FakeZwoMount calls; the
// mount derives every other reply from its own state.
struct Rig {
    test::FakeTaskClock clock;
    test::FakeZwoMount mount{clock};
    std::unique_ptr<TelescopeDriver> driver;
    bool silent_ = false;

    explicit Rig(int response_timeout_ms = 500, int device_number = 0, bool sync_time = false) {
        mount.override_reply(":Gps", "e1");  // AM5N: optional query unsupported.
        mount.set_slew_duration(0ns);        // a GOTO or park is over at once; a test that needs motion set_moving()s
        set_mount_time(0);
        vendor::zwo::ConnectionInfo info;
        info.type = vendor::zwo::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = mount.port();
        info.response_timeout_ms = response_timeout_ms;
        driver = vendor::zwo::create_zwo_telescope_with_site(device_number, info, 30.0, -100.0, {}, sync_time, clock);
    }
    ~Rig() {
        mount.release();
        driver.reset();
    }
    void connect() {
        REQUIRE(mount.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        REQUIRE(clock.wait_for_waiters(1, 5s));
    }
    void tick(std::chrono::nanoseconds duration = 250ms) {
        REQUIRE(clock.wait_for_waiters(1, 5s));
        clock.advance(duration);
        REQUIRE(clock.wait_for_woken_settled(5s));
    }
    void check_measurement_age(std::chrono::nanoseconds age) {
        const auto before = std::chrono::system_clock::now();
        const auto state = driver->get_device_state();
        const auto after = std::chrono::system_clock::now();
        const auto entry =
            std::find_if(state.begin(), state.end(), [](const auto& item) { return item.name == "TimeStamp"; });
        REQUIRE(entry != state.end());
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
        REQUIRE(std::sscanf(std::get<std::string>(entry->value).c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi,
                            &s, &ms) == 7);
        REQUIRE((ms >= 0 && ms < 1000));
        const auto stamp = std::chrono::sys_days{std::chrono::year{y} / mo / d} + std::chrono::hours(h) +
                           std::chrono::minutes(mi) + std::chrono::seconds(s) + std::chrono::milliseconds(ms);
        // UTC advances in real time, not with the fake clock; allow only timestamp millisecond truncation.
        CHECK(stamp >= before - age - 1ms);
        CHECK(stamp <= after - age);
    }
    // Commands received, written with the terminating '#'.
    int count(const std::string& command) const {
        const auto all = mount.commands();
        return static_cast<int>(std::count(all.begin(), all.end(), command.substr(0, command.size() - 1)));
    }
    int stops() const { return count(":Q#") + count(":Qe#") + count(":Qw#"); }
    int writes() const { return mount.count(":S") + mount.count(":MS"); }

    // The mount answers the command that starts with @p prefix with an error or an unparseable reply, or not.
    void fail(const std::string& prefix, bool on) {
        std::string reply = "bad";
        if (prefix == ":GVP") reply = "OnStep";
        if (prefix == ":GU" || prefix == ":GA" || prefix == ":GZ") reply = "e1";
        mount.override_reply(prefix, reply, on ? -1 : 0);
    }
    // The mount stops answering status and coordinate reads.
    void silence(bool on) {
        if (!on && !silent_) return;  // never clear a fail() rule on the same prefix
        silent_ = on;
        for (const char* prefix : {":GU", ":GR", ":GD"}) {
            if (on)
                mount.go_silent(prefix);
            else
                mount.override_reply(prefix, "", 0);
        }
    }
    // The ":GMTI" reply: a two-digit year (0: unparseable) and the west-positive zone hour.
    void set_mount_time(int year, int timezone_hours = 0) {
        mount.override_reply(":GMTI", year == 0 ? "bad"
                                                : "01/02/" + std::to_string(year) + "&03:04:05&+0" +
                                                      std::to_string(timezone_hours) + ":00");
    }
};
}  // namespace

TEST_CASE("ZWO disconnecting an idle entry leaves its connected sibling untouched", "[zwo][telescope][isolation]") {
    const bool asynchronous = GENERATE(false, true);
    Rig live(500, 3), idle(100, 7);
    live.connect();
    if (asynchronous)
        idle.driver->disconnect();
    else
        idle.driver->set_connected(false);
    idle.driver.reset();  // Join even a teardown issued before the entry ever connected.
    CHECK(live.driver->command_string(":GVP", false) == "AM5N");
    live.tick();
    CHECK(live.driver->get_connected());
    CHECK(live.driver->get_right_ascension() == 12.0);
    CHECK(live.driver->get_link_fault().empty());
}

TEST_CASE("ZWO sibling telescope reads writes and safety stops use their own endpoints",
          "[zwo][telescope][isolation]") {
    Rig a(500, 3), b(500, 7);
    b.mount.set_position(18.0, 45.0);
    a.connect();
    b.connect();
    a.tick();
    CHECK(a.driver->get_right_ascension() == 12.0);
    CHECK(b.driver->get_right_ascension() == 18.0);
    a.driver->set_target_right_ascension(6.0);
    b.driver->set_target_right_ascension(9.0);
    a.driver->set_tracking_rate(1);
    b.driver->set_tracking_rate(2);
    a.driver->abort_slew();
    b.driver->move_axis(0, 0.0);
    CHECK(a.count(":Sr06:00:00#") == 1);
    CHECK(a.count(":Sr09:00:00#") == 0);
    CHECK(b.count(":Sr09:00:00#") == 1);
    CHECK(b.count(":Sr06:00:00#") == 0);
    CHECK(a.count(":TL#") == 1);
    CHECK(a.count(":TS#") == 0);
    CHECK(b.count(":TS#") == 1);
    CHECK(b.count(":TL#") == 0);
    CHECK(a.count(":Q#") == 1);
    CHECK(b.count(":Q#") == 1);
    CHECK(b.count(":Qe#") == 1);
    CHECK(b.count(":Qw#") == 1);
    CHECK(a.count(":Qe#") == 0);
    CHECK(a.count(":Qw#") == 0);
    b.driver->set_connected(false);
    b.driver.reset();
    CHECK(a.driver->command_string(":GVP", false) == "AM5N");
    a.tick();
    CHECK(a.driver->get_connected());
    CHECK(a.driver->get_link_fault().empty());
}

TEST_CASE("ZWO a failed sibling connect and destruction cannot close a healthy link", "[zwo][telescope][isolation]") {
    Rig live(500, 3);
    live.connect();
    vendor::zwo::ConnectionInfo info;
    info.port_path = "/dev/does-not-exist-issue-1524";
    auto failed = vendor::zwo::create_zwo_telescope(7, info);
    CHECK_THROWS_AS(failed->set_connected(true), AlpacaException);
    failed.reset();
    CHECK(live.driver->command_string(":GVP", false) == "AM5N");
    live.tick();
    CHECK(live.driver->get_connected());
    CHECK(live.driver->get_link_fault().empty());
}

TEST_CASE("ZWO pulse and GOTO workers keep their endpoint after a sibling connects", "[zwo][telescope][isolation]") {
    Rig a(500, 3), b(500, 7);
    a.connect();
    b.connect();
    a.driver->pulse_guide(0, 100);
    REQUIRE(eventually([&] { return a.mount.count(":Mg") == 1; }));
    REQUIRE(a.clock.wait_for_waiters(2, 5s));
    a.clock.advance(100ms);
    REQUIRE(a.clock.wait_for_woken_settled(5s));
    CHECK(b.mount.count(":Mg") == 0);
    a.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(eventually([&] { return a.mount.count(":MS") == 1; }));
    CHECK(b.mount.count(":MS") == 0);
    a.driver->abort_slew();
    CHECK(a.stops() == 1);
    CHECK(b.stops() == 0);
    b.driver.reset();
    CHECK(a.driver->command_string(":GVP", false) == "AM5N");
}

TEST_CASE("ZWO one-sided fault loss and explicit reconnect leave a sibling healthy", "[zwo][telescope][isolation]") {
    const bool transport_loss = GENERATE(false, true);
    Rig a(500, 3), b(500, 7);
    b.mount.set_position(18.0, 45.0);
    a.connect();
    b.connect();
    if (transport_loss) {
        REQUIRE(b.mount.drop_link());
        b.tick();
    } else {
        b.silence(true);
        b.tick();
        b.tick();
        b.tick();
        CHECK(b.driver->get_connected());
        CHECK_FALSE(b.driver->get_link_fault().empty());
        CHECK(a.driver->get_link_fault().empty());
        b.tick(30s);
        b.silence(false);
    }
    CHECK_FALSE(b.driver->get_connected());
    CHECK(b.driver->get_device_state().empty());
    CHECK(a.driver->command_string(":GVP", false) == "AM5N");
    a.tick();
    CHECK(a.driver->get_connected());
    CHECK(a.driver->get_right_ascension() == 12.0);
    CHECK(a.driver->get_link_fault().empty());
    b.connect();
    CHECK(b.driver->get_right_ascension() == 18.0);
    a.tick();
    CHECK(a.driver->get_right_ascension() == 12.0);
    CHECK(a.count(":MS#") == 0);
    CHECK(b.count(":MS#") == 0);
}

TEST_CASE("ZWO a blocked sibling exchange cannot stall another telescope",
          "[zwo][telescope][isolation][getter-stall]") {
    Rig a(500, 3), b(5000, 7);
    a.connect();
    b.connect();
    b.mount.hold_reply(":GU");
    b.clock.advance(250ms);
    REQUIRE(b.mount.wait_until_held(5s));
    const auto begin = std::chrono::steady_clock::now();
    CHECK(a.driver->get_right_ascension() == 12.0);
    CHECK(std::chrono::steady_clock::now() - begin < 100ms);
    auto command = std::async(std::launch::async, [&] { return a.driver->command_string(":GVP", false); });
    const auto ready = command.wait_for(250ms);
    const auto disconnect_start = std::chrono::steady_clock::now();
    b.driver->set_connected(false);
    b.driver.reset();
    b.mount.release();  // Teardown must cancel the read before the fake releases its reply.
    CHECK(std::chrono::steady_clock::now() - disconnect_start < 2s);
    CHECK(ready == std::future_status::ready);
    CHECK(command.get() == "AM5N");
    CHECK(a.driver->command_string(":GVP", false) == "AM5N");
}

TEST_CASE("ZWO initial unavailable telemetry never performs getter I/O", "[zwo][telescope][telemetry][getter-stall]") {
    const int failure = GENERATE(0, 1, 2);
    Rig rig;
    rig.fail(":GU", failure == 0);
    rig.fail(":GR", failure == 1);
    rig.fail(":GD", failure == 2);
    rig.connect();
    const int gu = rig.mount.count(":GU"), gr = rig.mount.count(":GR"), gd = rig.mount.count(":GD");
    const auto begin = std::chrono::steady_clock::now();
    error([&] { rig.driver->get_alignment_mode(); }, AlpacaError::DriverException, "not available");
    error([&] { rig.driver->get_at_home(); }, AlpacaError::DriverException, "not available");
    error([&] { rig.driver->get_slewing(); }, AlpacaError::DriverException, "not available");
    rig.driver->get_device_state();
    CHECK(std::chrono::steady_clock::now() - begin < 100ms);
    CHECK(rig.mount.count(":GU") == gu);
    CHECK(rig.mount.count(":GR") == gr);
    CHECK(rig.mount.count(":GD") == gd);
    rig.fail(":GU", false);
    rig.fail(":GR", false);
    rig.fail(":GD", false);
    rig.tick();
    CHECK(rig.driver->get_alignment_mode() == AlignmentMode::GermanPolar);
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO partial status publication retains mandatory measurement age",
          "[zwo][telescope][telemetry][timestamp]") {
    const bool moving = GENERATE(false, true);
    const bool fail_dec = GENERATE(false, true);
    Rig rig;
    rig.connect();
    rig.check_measurement_age(0ns);
    rig.mount.set_moving(moving);
    rig.mount.set_tracking(false);
    rig.fail(fail_dec ? ":GD" : ":GR", true);
    rig.tick(2s);
    CHECK_FALSE(rig.driver->get_tracking());  // GU published, even though the pair failed.
    CHECK(rig.driver->get_link_fault().empty());
    rig.check_measurement_age(2s);
    rig.tick();
    rig.tick();
    REQUIRE_FALSE(rig.driver->get_link_fault().empty());
    rig.fail(fail_dec ? ":GD" : ":GR", false);
    rig.tick();
    CHECK(rig.driver->get_link_fault().empty());
    rig.check_measurement_age(0ns);
}

TEST_CASE("ZWO optional publication cannot renew mandatory measurement age", "[zwo][telescope][telemetry][timestamp]") {
    Rig rig(5000);
    rig.connect();
    rig.mount.hold_reply(":GA");
    rig.clock.advance(2s);
    REQUIRE(rig.mount.wait_until_held(5s));
    rig.check_measurement_age(0ns);  // The complete pair publishes before optional I/O.
    rig.clock.advance(1500ms);
    rig.check_measurement_age(1500ms);
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    rig.check_measurement_age(1500ms);
    CHECK(rig.driver->get_altitude() == 45.0);
    rig.tick();
    rig.check_measurement_age(0ns);
}

TEST_CASE("ZWO moving warmup timestamp uses its partial frame before the first mandatory pair",
          "[zwo][telescope][telemetry][timestamp]") {
    Rig rig(5000);
    rig.mount.set_moving(true);
    rig.fail(":GR", true);
    rig.connect();
    REQUIRE(rig.driver->get_slewing());
    error([&] { rig.driver->get_right_ascension(); }, AlpacaError::DriverException, "not available");
    rig.mount.hold_reply(":GR");
    rig.clock.advance(2s);
    REQUIRE(rig.mount.wait_until_held(5s));
    rig.check_measurement_age(0ns);
    rig.clock.advance(1s);
    rig.check_measurement_age(1s);
    rig.fail(":GR", false);
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    rig.check_measurement_age(0ns);
    CHECK(rig.driver->get_right_ascension() == 12.0);
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("ZWO mandatory telemetry alone controls fault recovery and loss", "[zwo][telescope][telemetry]") {
    const int failure = GENERATE(0, 1, 2, 3);
    CAPTURE(failure);
    Rig rig;
    rig.mount.set_moving(true);
    rig.connect();
    REQUIRE(rig.driver->get_slewing());
    rig.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(eventually([&] { return rig.mount.count(":MS") > 0; }));
    rig.driver->command_string(":GVP", false);  // Drain the MS exchange before advancing virtual time.
    // A completed wire exchange does not prove the GOTO worker has resumed the poll loop.
    const int paused_polls = rig.mount.count(":GU");
    for (int step = 0; step < 2000 && !rig.mount.wait_for_count(":GU", paused_polls + 1, 1ms); ++step) rig.tick(50ms);
    REQUIRE(rig.mount.count(":GU") > paused_polls);
    rig.mount.set_moving(false);
    rig.fail(":GU", failure == 0);
    rig.fail(":GR", failure == 1);
    rig.fail(":GD", failure == 2);
    rig.silence(failure == 3);
    const int polls_before_fault = rig.mount.count(":GU");
    rig.tick();
    rig.tick();
    REQUIRE(rig.mount.count(":GU") == polls_before_fault + 2);
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_slewing());
    rig.tick();
    REQUIRE(rig.mount.count(":GU") == polls_before_fault + 3);
    REQUIRE_FALSE(rig.driver->get_link_fault().empty());
    const auto fault = rig.driver->get_link_fault();
    CHECK(rig.driver->get_connected());
    error([&] { rig.driver->get_slewing(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_right_ascension(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_declination(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_altitude(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_azimuth(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_alignment_mode(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_at_home(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_at_park(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_side_of_pier(); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->get_tracking(); }, AlpacaError::DriverException, fault);
    const int writes_at_fault = rig.writes();
    error([&] { rig.driver->set_tracking(true); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->slew_to_coordinates_async(6.0, 20.0); }, AlpacaError::DriverException, fault);
    CHECK(rig.writes() == writes_at_fault);
    CHECK_FALSE(rig.driver->get_name().empty());
    CHECK(rig.driver->get_can_slew_async());
    REQUIRE_NOTHROW(rig.driver->abort_slew());
    REQUIRE_NOTHROW(rig.driver->move_axis(0, 0.0));
    CHECK(rig.stops() >= 3);
    SECTION("A complete sample recovers without reconnect") {
        rig.fail(":GU", false);
        rig.fail(":GR", false);
        rig.fail(":GD", false);
        rig.silence(false);
        rig.tick();
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_connected());
        CHECK_FALSE(rig.driver->get_slewing());
        CHECK(rig.driver->get_right_ascension() == 6.0);  // where the mount landed the GOTO
    }
    SECTION("Persistent failure loses the connection at the fault deadline") {
        rig.tick(29999ms);
        CHECK(rig.driver->get_connected());
        rig.clock.advance(1ms);
        error([&] { rig.driver->get_slewing(); }, AlpacaError::NotConnected);
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_device_state().empty());
        CHECK(rig.driver->get_link_fault() == fault);
        rig.fail(":GU", false);
        rig.fail(":GR", false);
        rig.fail(":GD", false);
        rig.silence(false);
        rig.driver->set_connected(true);
        CHECK(rig.driver->get_connected());
        CHECK(rig.driver->get_link_fault().empty());
        CHECK_FALSE(rig.driver->get_slewing());
    }
}

TEST_CASE("ZWO stalled poll keeps getters responsive but cannot preserve a frame forever",
          "[zwo][telescope][telemetry][getter-stall]") {
    Rig rig;
    rig.connect();
    rig.mount.hold_reply(":GU");
    rig.clock.advance(250ms);
    REQUIRE(rig.mount.wait_until_held(5s));
    const auto begin = std::chrono::steady_clock::now();
    CHECK(rig.driver->get_alignment_mode() == AlignmentMode::GermanPolar);
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->get_device_state();
    CHECK(std::chrono::steady_clock::now() - begin < 100ms);
    rig.clock.advance(29749ms);
    CHECK(rig.driver->get_connected());
    rig.clock.advance(1ms);
    error([&] { rig.driver->get_slewing(); }, AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    rig.mount.release();
}

TEST_CASE("ZWO optional telemetry failure invalidates only its own measurement", "[zwo][telescope][telemetry]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.driver->get_altitude() == 45.0);
    rig.fail(":GA", true);
    rig.fail(":GZ", true);
    rig.tick();
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_right_ascension() == 12.0);
    error([&] { rig.driver->get_altitude(); }, AlpacaError::DriverException, "not available");
    error([&] { rig.driver->get_azimuth(); }, AlpacaError::DriverException, "not available");
    rig.fail(":GA", false);
    rig.fail(":GZ", false);
    rig.tick();
    CHECK(rig.driver->get_altitude() == 45.0);
    CHECK(rig.driver->get_azimuth() == 180.0);
}

TEST_CASE("ZWO TCP reset is lost immediately and needs an explicit reconnect", "[zwo][telescope][telemetry]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.mount.drop_link());
    rig.tick();
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    error([&] { rig.driver->set_tracking(true); }, AlpacaError::NotConnected);
    const int queries = rig.mount.count(":GU");
    rig.clock.advance(1s);
    CHECK(rig.mount.count(":GU") == queries);
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO Park inference cannot complete without its measured final pair",
          "[zwo][telescope][telemetry][arrival]") {
    const bool reject_ra = GENERATE(false, true);
    Rig rig;
    rig.connect();
    rig.driver->park();
    rig.fail(":GR", reject_ra);
    rig.fail(":GD", !reject_ra);
    rig.tick(2500ms);
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());
    rig.tick();
    rig.tick();
    error([&] { rig.driver->get_at_park(); }, AlpacaError::DriverException);
    rig.fail(":GR", false);
    rig.fail(":GD", false);
    rig.tick(2500ms);
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO a long pulse pause preserves the link without renewing measurement age",
          "[zwo][telescope][telemetry][pulseguide]") {
    const bool moving = GENERATE(false, true);
    const bool between_status_and_pair = GENERATE(false, true);
    const bool queued = GENERATE(false, true);
    Rig rig;
    rig.mount.set_moving(moving);
    rig.connect();
    const int pairs = rig.mount.count(":GR");
    const auto before = time_stamp_of(*rig.driver);
    REQUIRE_FALSE(before.empty());
    if (between_status_and_pair) {
        rig.mount.hold_reply(":GU");
        rig.clock.advance(250ms);
        REQUIRE(rig.mount.wait_until_held(5s));
    }
    if (queued) {
        for (int pulse = 0; pulse < 10; ++pulse) rig.driver->pulse_guide(0, 5000);
    } else {
        rig.driver->pulse_guide(0, 60000);
    }
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    rig.tick(1s);
    CHECK(rig.mount.count(":GR") == pairs);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_slewing() == moving);
    rig.tick(40000ms - rig.clock.now().time_since_epoch());
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_slewing() == moving);
    CHECK(rig.mount.count(":GR") == pairs);
    // Wall UTC did not advance with the fake clock: an older stamp proves the age was not renewed.
    CHECK(time_stamp_of(*rig.driver) < before);
    REQUIRE(rig.mount.drop_link());
    // Even during the pause, a failed guide write or an explicit exchange observes transport loss.
    CHECK_THROWS_AS(rig.driver->command_string(":GVP", false), AlpacaException);
    error([&] { rig.driver->get_slewing(); }, AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE("ZWO a polled Park error remains on both completion properties", "[zwo][telescope][telemetry][arrival]") {
    Rig rig;
    rig.connect();
    rig.driver->park();
    rig.mount.override_reply(":Gps", "3");
    rig.tick();
    for (int read = 0; read < 2; ++read) {
        error([&] { rig.driver->get_at_park(); }, AlpacaError::DriverException, "Park failed");
        error([&] { rig.driver->get_slewing(); }, AlpacaError::DriverException, "Park failed");
    }
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());  // Abort clears the error; a new poll must confirm the stopped state.
    rig.tick();
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO a stopped poll cannot overwrite a newer command or disconnect", "[zwo][telescope][telemetry]") {
    const bool disconnect = GENERATE(false, true);
    Rig rig;
    rig.connect();
    rig.mount.hold_reply(":GU");
    rig.clock.advance(250ms);
    REQUIRE(rig.mount.wait_until_held(5s));
    if (disconnect) {
        rig.driver->set_connected(false);
    } else {
        // GOTO claims motion before its setup can acquire the held transport.
        rig.driver->slew_to_coordinates_async(6.0, 20.0);
    }
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    if (disconnect) {
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_device_state().empty());
        error([&] { rig.driver->get_slewing(); }, AlpacaError::NotConnected);
    } else {
        CHECK(rig.driver->get_slewing());
        error([&] { rig.driver->get_right_ascension(); }, AlpacaError::DriverException, "not available");
    }
}

TEST_CASE("ZWO serial transport removal is immediate loss rather than a timeout fault",
          "[zwo][telescope][telemetry][serial]") {
    test::FakeTaskClock clock;
    auto serial = std::make_unique<test::PtyResponder>([](const std::string& command) -> std::string {
        if (command == ":GVP#") return "AM5#";
        if (command == ":GMA#") return "012345ABCDEF#";
        if (command.find(":GU#") != std::string::npos) return "NG#";
        if (command.find(":GR#") != std::string::npos) return "12:00:00#";
        if (command.find(":GD#") != std::string::npos) return "+45*00:00#";
        if (command.find(":Ggr#") != std::string::npos) return "0.5#";
        if (command.starts_with(":S")) return "1#";
        return "0#";
    });
    vendor::zwo::ConnectionInfo info;
    info.port_path = serial->slave_path();
    info.response_timeout_ms = 500;
    auto driver = vendor::zwo::create_zwo_telescope_with_site(0, info, 30.0, -100.0, {}, false, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, 5s));
    serial.reset();
    clock.advance(250ms);
    REQUIRE(clock.wait_for_woken_settled(5s));
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_link_fault().empty());
    CHECK(driver->get_device_state().empty());
    error([&] { driver->get_slewing(); }, AlpacaError::NotConnected);
}

TEST_CASE("ZWO disconnect and destruction stay bounded under a silent telemetry exchange",
          "[zwo][telescope][telemetry]") {
    const bool queued_goto = GENERATE(false, true);
    Rig rig(5000);
    rig.connect();
    const int polls = rig.mount.count(":GU");
    rig.silence(true);
    rig.clock.advance(250ms);
    REQUIRE(eventually([&] { return rig.mount.count(":GU") > polls; }));
    if (queued_goto) rig.driver->slew_to_coordinates_async(6.0, 20.0);
    const auto begin = std::chrono::steady_clock::now();
    rig.driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - begin < (queued_goto ? 1s : 100ms));
    const int writes_at_return = rig.writes();
    rig.driver.reset();
    CHECK(std::chrono::steady_clock::now() - begin < 1s);
    CHECK(rig.writes() == writes_at_return);
}

TEST_CASE("ZWO fault recovery races disconnect without reviving a frame", "[zwo][telescope][telemetry][stress]") {
    for (int iteration = 0; iteration < 5; ++iteration) {
        Rig rig;
        rig.connect();
        rig.fail(":GR", true);
        rig.tick();
        rig.tick();
        rig.tick();
        REQUIRE_FALSE(rig.driver->get_link_fault().empty());
        rig.fail(":GR", false);
        std::atomic<bool> stop{false};
        test::StressCallGuard guard{AlpacaError::NotConnected, AlpacaError::DriverException};
        std::thread reader([&] {
            while (!stop.load()) {
                guard([&] { rig.driver->get_slewing(); });
                guard([&] { rig.driver->get_right_ascension(); });
                guard([&] { rig.driver->get_device_state(); });
                std::this_thread::yield();
            }
        });
        rig.clock.advance(250ms);
        rig.driver->set_connected(false);
        stop.store(true);
        reader.join();
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_device_state().empty());
        // Guarantee a call even if the disconnect won before the reader was scheduled.
        guard([&] { rig.driver->get_slewing(); });
        INFO(guard.report());
        CHECK(guard.unexpected_count() == 0);
        CHECK(guard.total_calls() > 0);
    }
}

TEST_CASE("ZWO a silent pulse reconciliation cannot delay the next guide command",
          "[zwo][telescope][telemetry][pulseguide]") {
    Rig rig(5000);
    rig.connect();
    rig.driver->pulse_guide(0, 100);
    REQUIRE(eventually([&] { return rig.mount.count(":Mg") == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(2, 5s));
    rig.driver->pulse_guide(1, 100);
    rig.mount.go_silent(":GR");
    const int pairs = rig.mount.count(":GR");
    rig.clock.advance(100ms);
    REQUIRE(eventually([&] { return rig.mount.count(":GR") > pairs; }));
    const auto begin = std::chrono::steady_clock::now();
    while (rig.mount.count(":Mg") < 2 && std::chrono::steady_clock::now() - begin < 1s) {
        std::this_thread::yield();
    }
    CHECK(rig.mount.count(":Mg") == 2);
}

TEST_CASE("ZWO a late pulse reconciliation reply cannot poison the next poll",
          "[zwo][telescope][telemetry][pulseguide]") {
    const bool dec = GENERATE(false, true);
    Rig rig(5000);
    rig.connect();
    rig.driver->pulse_guide(0, 100);
    const auto expected_dec = rig.driver->get_declination();
    REQUIRE(eventually([&] { return rig.mount.count(":Mg") == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(2, 5s));
    rig.mount.delay_next(dec ? ":GD" : ":GR", 650ms);  // Transport latency, not a driver timer.
    rig.clock.advance(100ms);
    // The late reply is on the socket before the next exchange starts.
    REQUIRE(rig.mount.wait_for_delayed_reply(5s));
    // Fence the transport: its next response must belong to this command, not the late coordinate.
    CHECK(rig.driver->command_string(":GVP", false) == "AM5N");
    rig.tick(1s);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_right_ascension() == 12.0);
    CHECK(rig.driver->get_declination() == expected_dec);
    CHECK(rig.driver->get_link_fault().empty());
}

TEST_CASE("ZWO TrackingRate is cached and a failed connect measurement stays unavailable",
          "[zwo][telescope][telemetry][getter-stall]") {
    const bool failed = GENERATE(false, true);
    Rig rig;
    rig.fail(":GT", failed);
    rig.connect();
    const int queries = rig.mount.count(":GT");
    REQUIRE(queries > 0);
    for (int read = 0; read < 3; ++read) {
        if (failed)
            error([&] { rig.driver->get_tracking_rate(); }, AlpacaError::DriverException, "not available");
        else
            CHECK(rig.driver->get_tracking_rate() == 0);
        CHECK(rig.mount.count(":GT") == queries);
    }
    rig.driver->set_tracking_rate(2);
    CHECK(rig.driver->get_tracking_rate() == 2);
    CHECK(rig.mount.count(":GT") == queries);
}

TEST_CASE("ZWO cached operational getters require a connection while offline setters remain available",
          "[zwo][telescope][unit][utcdate]") {
    Rig rig;
    const int property = GENERATE(0, 1, 2, 3);
    const std::function<void()> reads[] = {
        [&] { (void)rig.driver->get_target_right_ascension(); }, [&] { (void)rig.driver->get_target_declination(); },
        [&] { (void)rig.driver->get_utc_date(); }, [&] { (void)rig.driver->get_guide_rate(); }};
    CAPTURE(property);
    error(reads[property], AlpacaError::NotConnected);
    const auto configure = [&] {
        rig.driver->set_target_right_ascension(6.0);
        rig.driver->set_target_declination(20.0);
        rig.driver->set_utc_date(std::chrono::sys_days{std::chrono::year{2001} / 1 / 2});
        rig.driver->set_guide_rate({0.002, 0.002});
    };
    REQUIRE_NOTHROW(configure());
    error(reads[property], AlpacaError::NotConnected);
    rig.connect();
    configure();
    REQUIRE_NOTHROW(reads[property]());
    rig.driver->set_connected(false);
    REQUIRE_FALSE(rig.driver->get_connected());
    error(reads[property], AlpacaError::NotConnected);
    REQUIRE_NOTHROW(configure());
    error(reads[property], AlpacaError::NotConnected);
}

TEST_CASE("ZWO individual guide-rate writes update the shared rate without a getter",
          "[zwo][telescope][unit][guide-rate]") {
    Rig rig;
    const bool ra = GENERATE(false, true);
    const auto write = [&](double value) {
        if (ra)
            rig.driver->set_guide_rate_right_ascension(value);
        else
            rig.driver->set_guide_rate_declination(value);
    };
    error([&] { write(50.0); }, AlpacaError::InvalidValue);
    error([&] { write(std::numeric_limits<double>::quiet_NaN()); }, AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(write(0.002));
    error([&] { rig.driver->get_guide_rate(); }, AlpacaError::NotConnected);
    rig.connect();
    write(0.003);
    const auto before = rig.driver->get_guide_rate();
    write(0.001);
    const auto after = rig.driver->get_guide_rate();
    CHECK(after.ra == after.dec);
    CHECK(after.ra < before.ra);
}

TEST_CASE("ZWO UTCDate getters never read the link and the poll refreshes mount time",
          "[zwo][telescope][telemetry][getter-stall]") {
    Rig rig(1000);  // The wire reader paces each character; a complete GMTI reply exceeds 100 ms.
    rig.set_mount_time(20);
    rig.connect();
    const int queries = rig.mount.count(":GMTI");
    REQUIRE(queries == 1);
    const auto mount_time = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 3h + 4min + 5s;
    CHECK(rig.driver->get_utc_date() == mount_time);
    rig.driver->get_device_state();
    CHECK(rig.mount.count(":GMTI") == queries);
    const auto client_time = mount_time - 24h;
    rig.driver->set_utc_date(client_time);
    rig.tick(1s);
    CHECK(rig.driver->get_utc_date() == client_time + 1s);
    CHECK(rig.mount.count(":GMTI") == queries);
    rig.set_mount_time(20);
    rig.tick(28s);
    CHECK(rig.mount.count(":GMTI") == queries);
    rig.tick(1s);
    REQUIRE(rig.mount.count(":GMTI") == queries + 1);
    CHECK(rig.driver->get_utc_date() == mount_time);
    rig.set_mount_time(21);
    rig.tick(29s);
    CHECK(rig.driver->get_utc_date() == mount_time + 29s);
    CHECK(rig.mount.count(":GMTI") == queries + 1);
    rig.tick(1s);
    CHECK(rig.driver->get_utc_date() == mount_time + 366 * 24h);
    CHECK(rig.mount.count(":GMTI") == queries + 2);
}

TEST_CASE("ZWO connect samples mount UTC when configured sync fails", "[zwo][telescope][unit][utcdate]") {
    Rig rig(1000, 0, true);
    rig.set_mount_time(20, 3);
    rig.mount.override_reply(":SMTI", "e1");
    rig.connect();
    const auto mount_utc = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 6h + 4min + 5s;
    CHECK(rig.mount.count(":SMTI") == 1);
    CHECK(rig.mount.count(":GMTI") == 1);
    CHECK(rig.driver->get_utc_date() == mount_utc);
    CHECK(rig.driver->get_utc_date() == mount_utc);
    CHECK(rig.mount.count(":GMTI") == 1);
}

TEST_CASE("ZWO unavailable connect time is not invented or retried by getters", "[zwo][telescope][unit][utcdate]") {
    const auto reply = GENERATE("bad", "02/30/20&03:04:05&+00:00", "01/02/20&25:04:05&+00:00");
    Rig rig(1000);
    rig.mount.override_reply(":GMTI", reply);
    for (int session = 0; session < 2; ++session) {
        rig.connect();
        for (int read = 0; read < 2; ++read) {
            error([&] { rig.driver->get_utc_date(); }, AlpacaError::InvalidOperation);
            rig.driver->get_device_state();
        }
        CHECK(rig.mount.count(":GMTI") == session + 1);
        const auto client = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
        rig.driver->set_utc_date(client);
        CHECK(rig.driver->get_utc_date() == client);
        rig.driver->set_connected(false);
    }
}

TEST_CASE("ZWO redundant connect retries an unavailable mount clock only while UTC is unset",
          "[zwo][telescope][unit][utcdate]") {
    Rig rig(1000);
    rig.connect();
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::InvalidOperation);
    REQUIRE(rig.mount.count(":GMTI") == 1);
    rig.set_mount_time(20);
    rig.driver->set_connected(true);
    const auto mount_utc = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 3h + 4min + 5s;
    CHECK(rig.driver->get_utc_date() == mount_utc);
    CHECK(rig.mount.count(":GMTI") == 2);
    rig.set_mount_time(21);
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_utc_date() == mount_utc);
    CHECK(rig.mount.count(":GMTI") == 2);
}

TEST_CASE("ZWO UTC anchor preserves fractional elapsed time and earlier or later client writes",
          "[zwo][telescope][utc][issue1522]") {
    Rig rig;
    const auto first = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 123456789ns;
    // Offline writes remain supported, but a fresh connection starts a new lifetime.
    rig.driver->set_utc_date(first);
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK(vendor::zwo::utc_anchor_for_testing(*rig.driver) == first);
    rig.clock.advance(123456789ns);
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK(vendor::zwo::utc_anchor_for_testing(*rig.driver) == first + 123456789ns);
    rig.connect();
    for (const auto value : {first, first - 365 * 24h, first + 365 * 24h}) {
        rig.driver->set_utc_date(value);
        CHECK(rig.driver->get_utc_date() == value);
        rig.tick(987654321ns);
        CHECK(rig.driver->get_utc_date() == value + 987654321ns);
    }
    CHECK(rig.count(":SMTI01/02/20&00:00:00&+00:00#") == 1);
}

TEST_CASE("ZWO UTC anchor clears on disconnect fresh connect and failed initialization",
          "[zwo][telescope][utc][issue1522]") {
    const bool fail_identity = GENERATE(false, true);
    Rig rig;
    const auto old = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    const auto unavailable = [&] {
        error([&] { rig.driver->get_utc_date(); },
              rig.driver->get_connected() ? AlpacaError::InvalidOperation : AlpacaError::NotConnected);
    };
    rig.driver->set_utc_date(old);
    rig.connect();  // Sync disabled: the offline anchor must not leak into this session.
    unavailable();
    rig.driver->set_utc_date(old);
    rig.driver->set_connected(true);  // Redundant Connected=true is not a new session.
    CHECK(rig.driver->get_utc_date() == old);
    rig.driver->set_connected(false);
    unavailable();
    CHECK_FALSE(vendor::zwo::utc_anchor_for_testing(*rig.driver));
    // Offline configuration writes remain accepted, but do not make getters readable.
    rig.driver->set_utc_date(old);
    unavailable();
    CHECK(vendor::zwo::utc_anchor_for_testing(*rig.driver) == old);
    rig.fail(":GVP", fail_identity);
    if (fail_identity) {
        CHECK_THROWS_AS(rig.driver->set_connected(true), AlpacaException);
        CHECK_FALSE(rig.driver->get_connected());
        unavailable();
        CHECK_FALSE(vendor::zwo::utc_anchor_for_testing(*rig.driver));
        rig.fail(":GVP", false);
    }
    rig.connect();
    unavailable();
}

TEST_CASE("ZWO failed periodic time read keeps the last anchor and successful reads convert timezone",
          "[zwo][telescope][utc][issue1522]") {
    Rig rig(1000);
    rig.connect();
    const auto client = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(client);
    rig.tick(29s);
    rig.tick(1s);
    CHECK(rig.mount.count(":GMTI") > 0);
    CHECK(rig.driver->get_utc_date() == client + 30s);
    rig.set_mount_time(20, 3);
    rig.tick(29s);
    rig.tick(1s);
    // The wire timezone is west-positive: +03:00 means local time is UTC-3.
    const auto mount = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 6h + 4min + 5s;
    CHECK(rig.driver->get_utc_date() == mount);
}

TEST_CASE("ZWO a failed transport open clears an offline UTC anchor", "[zwo][telescope][utc][issue1522]") {
    test::FakeTaskClock clock;
    vendor::zwo::ConnectionInfo info;
    info.port_path = "/dev/does-not-exist-issue-1522";
    auto driver = vendor::zwo::create_zwo_telescope(0, info, clock);
    driver->set_utc_date(std::chrono::sys_days{std::chrono::year{2001} / 1 / 2});
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    error([&] { driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK_FALSE(vendor::zwo::utc_anchor_for_testing(*driver));
}

TEST_CASE("ZWO failed initialization clears a UTC write made during the handshake",
          "[zwo][telescope][utc][issue1522]") {
    Rig rig;
    rig.mount.hold_reply(":GVP");
    rig.fail(":GVP", true);
    rig.driver->connect();
    REQUIRE(rig.mount.wait_until_held(5s));
    rig.driver->set_utc_date(std::chrono::sys_days{std::chrono::year{2001} / 1 / 2});
    rig.mount.release();
    REQUIRE(eventually([&] { return !rig.driver->get_connecting(); }));
    CHECK_FALSE(rig.driver->get_connected());
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK_FALSE(vendor::zwo::utc_anchor_for_testing(*rig.driver));
}

TEST_CASE("ZWO disconnect cannot republish a periodic UTC read from the old session",
          "[zwo][telescope][utc][issue1522][stress]") {
    Rig rig(1000);
    rig.connect();
    rig.set_mount_time(20);
    rig.tick(29s);
    rig.mount.hold_reply(":GMTI");
    rig.clock.advance(1s);
    REQUIRE(rig.mount.wait_until_held(5s));
    rig.driver->set_connected(false);
    const auto offline = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(offline);
    rig.mount.release();
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK(vendor::zwo::utc_anchor_for_testing(*rig.driver) == offline);
    rig.driver->set_connected(true);  // Joins teardown and starts with sync disabled.
    const auto mount_utc = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 3h + 4min + 5s;
    CHECK(rig.driver->get_utc_date() == mount_utc);  // A fresh sample, neither the old read nor the offline write.
}

TEST_CASE("ZWO an old UTC writer cannot replace an offline write after disconnect",
          "[zwo][telescope][utc][issue1522][stress]") {
    Rig rig(1000);
    rig.connect();
    rig.mount.hold_reply(":SMTI");
    const auto old = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    auto writer = std::async(std::launch::async, [&] {
        try {
            rig.driver->set_utc_date(old);
        } catch (const AlpacaException&) {
            // Disconnect may cancel the exchange rather than let its acknowledgement complete.
        }
    });
    const bool entered = rig.mount.wait_until_held(5s);
    if (!entered) rig.mount.release();
    REQUIRE(entered);
    rig.driver->set_connected(false);
    const auto offline = old + 24h;
    rig.driver->set_utc_date(offline);
    rig.mount.release();
    writer.get();
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK(vendor::zwo::utc_anchor_for_testing(*rig.driver) == offline);
}

TEST_CASE("ZWO UTC refuses a lost link and starts fresh on explicit reconnect",
          "[zwo][telescope][utc][issue1522][stress]") {
    const bool transport_loss = GENERATE(false, true);
    Rig rig;
    rig.connect();
    const auto old = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(old);
    REQUIRE(rig.driver->get_utc_date() == old);
    if (transport_loss) {
        REQUIRE(rig.mount.drop_link());
        rig.tick();
    } else {
        rig.silence(true);
        rig.tick();
        rig.tick();
        rig.tick();
        REQUIRE(rig.driver->get_connected());
        error([&] { rig.driver->get_utc_date(); }, AlpacaError::DriverException);
        rig.tick(30s);
        rig.silence(false);
    }
    REQUIRE_FALSE(rig.driver->get_connected());
    // Loss keeps its reason and refuses even offline configuration writes.
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    error([&] { rig.driver->set_utc_date(old + 24h); }, AlpacaError::NotConnected);
    CHECK(rig.driver->get_device_state().empty());
    rig.connect();  // No intervening Disconnect that could hide a stale session.
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::InvalidOperation);
    CHECK(rig.mount.count(":MS") == 0);
}

TEST_CASE("ZWO disconnect during GOTO time synchronization leaves UTC unanchored",
          "[zwo][telescope][utc][issue1522][stress]") {
    Rig rig(1000, 0, true);
    rig.connect();
    rig.mount.hold_reply(":SMTI");
    rig.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(rig.mount.wait_until_held(5s));
    rig.driver->set_connected(false);  // Cancels the exchange and joins the GOTO worker.
    rig.mount.release();
    rig.clock.advance(10s);
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK_FALSE(vendor::zwo::utc_anchor_for_testing(*rig.driver));
    const auto offline = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(offline);
    rig.clock.advance(1234ms);
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    CHECK(vendor::zwo::utc_anchor_for_testing(*rig.driver) == offline + 1234ms);
    CHECK(rig.mount.count(":MS") == 0);
}

TEST_CASE("ZWO host time synchronization seeds UTC at connect refresh and before GOTO",
          "[zwo][telescope][utc][issue1522]") {
    const bool sync_time = GENERATE(false, true);
    Rig rig(1000, 0, sync_time);
    const auto before_connect = std::chrono::system_clock::now();
    rig.connect();
    CHECK(rig.writes() >= 1);  // Configured site is applied regardless of time policy.
    const auto time_writes = [&] { return rig.mount.count(":SMTI"); };
    CHECK(time_writes() == (sync_time ? 1 : 0));
    if (sync_time) {
        const auto after_connect = std::chrono::system_clock::now();
        CHECK(rig.driver->get_utc_date() >= before_connect);
        CHECK(rig.driver->get_utc_date() <= after_connect);
        rig.tick(1234ms);
        CHECK(rig.driver->get_utc_date() >= before_connect + 1234ms);
        CHECK(rig.driver->get_utc_date() <= after_connect + 1234ms);
    } else
        error([&] { rig.driver->get_utc_date(); }, AlpacaError::InvalidOperation);
    const auto client = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(client);
    const auto before_refresh = std::chrono::system_clock::now();
    rig.driver->set_connected(true);
    if (sync_time) {
        CHECK(rig.driver->get_utc_date() >= before_refresh);
        const auto after_refresh = std::chrono::system_clock::now();
        REQUIRE(rig.clock.wait_for_waiters(1, 5s));
        rig.tick(1234ms);
        CHECK(rig.driver->get_utc_date() >= before_refresh + 1234ms);
        CHECK(rig.driver->get_utc_date() <= after_refresh + 1234ms);
    } else
        CHECK(rig.driver->get_utc_date() == client);
    const auto before_goto = std::chrono::system_clock::now();
    rig.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(eventually([&] { return rig.mount.count(":MS") == 1; }));
    REQUIRE_NOTHROW(rig.driver->command_string(":GVP", false));  // Drain the setup exchange.
    const auto goto_utc = rig.driver->get_utc_date();
    const auto goto_after = std::chrono::system_clock::now();
    CHECK(goto_utc >= before_goto);
    CHECK(goto_utc <= goto_after);
    const auto after_goto = std::chrono::system_clock::now();
    rig.tick(1234ms);
    CHECK(rig.driver->get_utc_date() >= before_goto + 1234ms);
    CHECK(rig.driver->get_utc_date() <= after_goto + 1234ms);
    CHECK(time_writes() == (sync_time ? 4 : 2));
}

TEST_CASE("ZWO a management health read cannot expire the connect warmup", "[zwo][telescope][telemetry]") {
    const bool sever_transport = GENERATE(false, true);
    Rig rig;
    rig.mount.hold_reply(":GVP");
    rig.driver->connect();
    REQUIRE(rig.mount.wait_until_held(5s));
    rig.clock.advance(31s);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    if (sever_transport) {
        // shutdown takes effect immediately; the held reply cannot leave until released below.
        (void)rig.mount.drop_link();
        // An unverified link is still private initialization; the connect error reports its failure below.
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_device_state().empty());
    }
    rig.mount.release();
    // Also release the failed-handshake retry timer on the red path before joining the async connect.
    if (rig.clock.wait_for_waiters(1, 1s)) rig.clock.advance(1s);
    REQUIRE(eventually([&] { return !rig.driver->get_connecting(); }));
    CHECK(rig.driver->get_connected() == !sever_transport);
    CHECK(rig.driver->get_link_fault().empty());
    if (sever_transport) {
        CHECK_FALSE(rig.driver->get_last_connect_error().empty());
        CHECK_NOTHROW(rig.driver->set_site_elevation(123.0));
        CHECK_NOTHROW(rig.driver->set_target_right_ascension(6.0));
        CHECK_NOTHROW(rig.driver->set_utc_date(std::chrono::system_clock::now()));
    }
}
#endif

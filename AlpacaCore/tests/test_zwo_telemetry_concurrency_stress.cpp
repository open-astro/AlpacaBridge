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
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#include "pty_responder.h"

namespace {
using namespace std::chrono_literals;
using namespace alpacacore;

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

struct Rig {
    test::FakeTaskClock clock;
    std::atomic<int> ra_hours{12};
    std::mutex commands_mutex;
    std::vector<std::string> commands;
    std::atomic<bool> bad_status{false}, bad_ra{false}, bad_dec{false}, moving{false};
    std::atomic<bool> silent{false}, hold_status{false}, status_entered{false}, release_status{false};
    std::atomic<bool> park_error{false};
    std::atomic<bool> horizontal_error{false};
    std::atomic<bool> withhold_ra{false};
    std::atomic<bool> delay_ra{false}, delay_dec{false};
    std::atomic<bool> late_reply_ready{false};
    std::atomic<bool> bad_tracking_rate{false};
    std::atomic<bool> hold_identity{false}, identity_entered{false}, release_identity{false};
    std::atomic<bool> bad_identity{false};
    std::atomic<bool> hold_time{false}, time_entered{false}, release_time{false};
    std::atomic<bool> hold_time_write{false}, time_write_entered{false};
    std::atomic<int> timezone_hours{0};
    std::atomic<int> tracking_queries{0}, time_queries{0}, time_year{0};
    std::atomic<int> pulse_commands{0};
    std::atomic<int> gu{0}, gr{0}, gd{0}, stops{0}, writes{0}, gotos{0};
    test::FakeMountServer server{[this, pending = std::string{}](const std::string& chunk) mutable {
        pending += chunk;
        std::string response;
        for (auto end = pending.find('#'); end != std::string::npos; end = pending.find('#')) {
            const auto command = pending.substr(0, end + 1);
            pending.erase(0, end + 1);
            {
                std::lock_guard<std::mutex> lock(commands_mutex);
                commands.push_back(command);
            }
            if (command == ":GU#") {
                ++gu;
                const bool was_moving = moving.load();
                if (hold_status.load()) {
                    status_entered.store(true);
                    while (!release_status.load()) std::this_thread::yield();
                }
                if (!silent.load()) response += bad_status.load() ? "e1#" : (was_moving ? "G#" : "NG#");
            } else if (command == ":GR#") {
                ++gr;
                if (delay_ra.exchange(false)) {
                    std::this_thread::sleep_for(650ms);  // Transport latency, not a driver timer.
                    late_reply_ready.store(true);
                }
                if (!silent.load() && !withhold_ra.load())
                    response += bad_ra.load() ? "bad#" : std::to_string(ra_hours.load()) + ":00:00#";
            } else if (command == ":GD#") {
                ++gd;
                if (delay_dec.exchange(false)) {
                    std::this_thread::sleep_for(650ms);  // Transport latency, not a driver timer.
                    late_reply_ready.store(true);
                }
                if (!silent.load()) response += bad_dec.load() ? "bad#" : "+45*00:00#";
            } else if (command.starts_with(":Mg")) {
                ++pulse_commands;
            } else if (command == ":Q#" || command == ":Qe#" || command == ":Qw#") {
                ++stops;
            } else if (command.starts_with(":S") || command == ":MS#") {
                ++writes;
                if (command.starts_with(":SMTI") && hold_time_write.load()) {
                    time_write_entered.store(true);
                    while (!release_time.load()) std::this_thread::yield();
                }
                if (command == ":MS#") ++gotos;
                response += command == ":MS#" ? "0#" : "1#";
            } else if (command == ":Ggr#")
                response += "0.5#";
            else if (command == ":GVP#") {
                if (hold_identity.load()) {
                    identity_entered.store(true);
                    while (!release_identity.load()) std::this_thread::yield();
                }
                response += bad_identity.load() ? "OnStep#" : "AM5N#";
            } else if (command == ":GMA#") {
                response += "012345ABCDEF#";
            } else if (command == ":GT#") {
                ++tracking_queries;
                response += bad_tracking_rate.load() ? "bad#" : "0#";
            } else if (command == ":GMTI#" || command == ":GL#" || command == ":GC#") {
                ++time_queries;
                const int year = time_year.load();
                if (hold_time.load()) {
                    time_entered.store(true);
                    while (!release_time.load()) std::this_thread::yield();
                }
                response += year == 0 ? "bad#"
                                      : "01/02/" + std::to_string(year) + "&03:04:05&+0" +
                                            std::to_string(timezone_hours.load()) + ":00#";
            } else if (command == ":GA#")
                response += horizontal_error.load() ? "e1#" : "+45*00:00#";
            else if (command == ":GZ#")
                response += horizontal_error.load() ? "e1#" : "180*00:00#";
            else if (command == ":GMGE#")
                response += "+30*00:00&+100*00:00#";
            else if (command == ":Gps#")
                response += park_error.load() ? "3#" : "e1#";  // AM5N: optional query unsupported.
            else
                response += "0#";
        }
        return response;
    }};
    std::unique_ptr<TelescopeDriver> driver;

    explicit Rig(int response_timeout_ms = 500, int device_number = 0, bool sync_time = false) {
        vendor::zwo::ConnectionInfo info;
        info.type = vendor::zwo::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = response_timeout_ms;
        driver = vendor::zwo::create_zwo_telescope_with_site(device_number, info, 30.0, -100.0, {}, sync_time, clock);
    }
    ~Rig() {
        release_status.store(true);
        release_identity.store(true);
        release_time.store(true);
        driver.reset();
    }
    void connect() {
        REQUIRE(server.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        REQUIRE(clock.wait_for_waiters(1, 5s));
    }
    void tick(std::chrono::nanoseconds duration = 250ms) {
        REQUIRE(clock.wait_for_waiters(1, 5s));
        clock.advance(duration);
        REQUIRE(clock.wait_for_woken_settled(5s));
    }
    int count(const std::string& command) {
        std::lock_guard<std::mutex> lock(commands_mutex);
        return static_cast<int>(std::count(commands.begin(), commands.end(), command));
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
    b.ra_hours.store(18);
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
    REQUIRE(eventually([&] { return a.pulse_commands.load() == 1; }));
    REQUIRE(a.clock.wait_for_waiters(2, 5s));
    a.clock.advance(100ms);
    REQUIRE(a.clock.wait_for_woken_settled(5s));
    CHECK(b.pulse_commands.load() == 0);
    a.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(eventually([&] { return a.gotos.load() == 1; }));
    CHECK(b.gotos.load() == 0);
    a.driver->abort_slew();
    CHECK(a.stops.load() == 1);
    CHECK(b.stops.load() == 0);
    b.driver.reset();
    CHECK(a.driver->command_string(":GVP", false) == "AM5N");
}

TEST_CASE("ZWO one-sided fault loss and explicit reconnect leave a sibling healthy", "[zwo][telescope][isolation]") {
    const bool transport_loss = GENERATE(false, true);
    Rig a(500, 3), b(500, 7);
    b.ra_hours.store(18);
    a.connect();
    b.connect();
    if (transport_loss) {
        REQUIRE(b.server.drop_connections());
        b.tick();
    } else {
        b.silent.store(true);
        b.tick();
        b.tick();
        b.tick();
        CHECK(b.driver->get_connected());
        CHECK_FALSE(b.driver->get_link_fault().empty());
        CHECK(a.driver->get_link_fault().empty());
        b.tick(30s);
        b.silent.store(false);
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
    b.hold_status.store(true);
    b.clock.advance(250ms);
    REQUIRE(eventually([&] { return b.status_entered.load(); }));
    const auto begin = std::chrono::steady_clock::now();
    CHECK(a.driver->get_right_ascension() == 12.0);
    CHECK(std::chrono::steady_clock::now() - begin < 100ms);
    auto command = std::async(std::launch::async, [&] { return a.driver->command_string(":GVP", false); });
    const auto ready = command.wait_for(250ms);
    const auto disconnect_start = std::chrono::steady_clock::now();
    b.driver->set_connected(false);
    b.driver.reset();
    b.release_status.store(true);  // Teardown must cancel the read before the fake releases its reply.
    CHECK(std::chrono::steady_clock::now() - disconnect_start < 2s);
    CHECK(ready == std::future_status::ready);
    CHECK(command.get() == "AM5N");
    CHECK(a.driver->command_string(":GVP", false) == "AM5N");
}

TEST_CASE("ZWO initial unavailable telemetry never performs getter I/O", "[zwo][telescope][telemetry][getter-stall]") {
    const int failure = GENERATE(0, 1, 2);
    Rig rig;
    rig.bad_status.store(failure == 0);
    rig.bad_ra.store(failure == 1);
    rig.bad_dec.store(failure == 2);
    rig.connect();
    const int gu = rig.gu.load(), gr = rig.gr.load(), gd = rig.gd.load();
    const auto begin = std::chrono::steady_clock::now();
    error([&] { rig.driver->get_alignment_mode(); }, AlpacaError::DriverException, "not available");
    error([&] { rig.driver->get_at_home(); }, AlpacaError::DriverException, "not available");
    error([&] { rig.driver->get_slewing(); }, AlpacaError::DriverException, "not available");
    rig.driver->get_device_state();
    CHECK(std::chrono::steady_clock::now() - begin < 100ms);
    CHECK(rig.gu.load() == gu);
    CHECK(rig.gr.load() == gr);
    CHECK(rig.gd.load() == gd);
    rig.bad_status.store(false);
    rig.bad_ra.store(false);
    rig.bad_dec.store(false);
    rig.tick();
    CHECK(rig.driver->get_alignment_mode() == AlignmentMode::GermanPolar);
    CHECK_FALSE(rig.driver->get_at_home());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO mandatory telemetry alone controls fault recovery and loss", "[zwo][telescope][telemetry]") {
    const int failure = GENERATE(0, 1, 2, 3);
    Rig rig;
    rig.moving.store(true);
    rig.connect();
    REQUIRE(rig.driver->get_slewing());
    rig.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(eventually([&] { return rig.gotos.load() > 0; }));
    rig.driver->command_string(":GVP", false);  // fence the setup exchange before advancing the poll clock
    rig.moving.store(false);
    rig.bad_status.store(failure == 0);
    rig.bad_ra.store(failure == 1);
    rig.bad_dec.store(failure == 2);
    rig.silent.store(failure == 3);
    rig.tick();
    rig.tick();
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_slewing());
    rig.tick();
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
    const int writes_at_fault = rig.writes.load();
    error([&] { rig.driver->set_tracking(true); }, AlpacaError::DriverException, fault);
    error([&] { rig.driver->slew_to_coordinates_async(6.0, 20.0); }, AlpacaError::DriverException, fault);
    CHECK(rig.writes.load() == writes_at_fault);
    CHECK_FALSE(rig.driver->get_name().empty());
    CHECK(rig.driver->get_can_slew_async());
    REQUIRE_NOTHROW(rig.driver->abort_slew());
    REQUIRE_NOTHROW(rig.driver->move_axis(0, 0.0));
    CHECK(rig.stops.load() >= 3);
    SECTION("A complete sample recovers without reconnect") {
        rig.bad_status.store(false);
        rig.bad_ra.store(false);
        rig.bad_dec.store(false);
        rig.silent.store(false);
        rig.tick();
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_connected());
        CHECK_FALSE(rig.driver->get_slewing());
        CHECK(rig.driver->get_right_ascension() == 12.0);
    }
    SECTION("Persistent failure loses the connection at the fault deadline") {
        rig.tick(29999ms);
        CHECK(rig.driver->get_connected());
        rig.clock.advance(1ms);
        error([&] { rig.driver->get_slewing(); }, AlpacaError::NotConnected);
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_device_state().empty());
        CHECK(rig.driver->get_link_fault() == fault);
        rig.bad_status.store(false);
        rig.bad_ra.store(false);
        rig.bad_dec.store(false);
        rig.silent.store(false);
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
    rig.hold_status.store(true);
    rig.clock.advance(250ms);
    REQUIRE(eventually([&] { return rig.status_entered.load(); }));
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
    rig.release_status.store(true);
}

TEST_CASE("ZWO optional telemetry failure invalidates only its own measurement", "[zwo][telescope][telemetry]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.driver->get_altitude() == 45.0);
    rig.horizontal_error.store(true);
    rig.tick();
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_right_ascension() == 12.0);
    error([&] { rig.driver->get_altitude(); }, AlpacaError::DriverException, "not available");
    error([&] { rig.driver->get_azimuth(); }, AlpacaError::DriverException, "not available");
    rig.horizontal_error.store(false);
    rig.tick();
    CHECK(rig.driver->get_altitude() == 45.0);
    CHECK(rig.driver->get_azimuth() == 180.0);
}

TEST_CASE("ZWO TCP reset is lost immediately and needs an explicit reconnect", "[zwo][telescope][telemetry]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.server.drop_connections());
    rig.tick();
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    error([&] { rig.driver->set_tracking(true); }, AlpacaError::NotConnected);
    const int queries = rig.gu.load();
    rig.clock.advance(1s);
    CHECK(rig.gu.load() == queries);
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
    rig.bad_ra.store(reject_ra);
    rig.bad_dec.store(!reject_ra);
    rig.tick(2500ms);
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK(rig.driver->get_slewing());
    rig.tick();
    rig.tick();
    error([&] { rig.driver->get_at_park(); }, AlpacaError::DriverException);
    rig.bad_ra.store(false);
    rig.bad_dec.store(false);
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
    rig.moving.store(moving);
    rig.connect();
    const int pairs = rig.gr.load();
    const auto measured_stamp = [&] {
        for (const auto& item : rig.driver->get_device_state())
            if (item.name == "TimeStamp") return std::get<std::string>(item.value);
        return std::string{};
    };
    const auto before = measured_stamp();
    REQUIRE_FALSE(before.empty());
    if (between_status_and_pair) {
        rig.hold_status.store(true);
        rig.clock.advance(250ms);
        REQUIRE(eventually([&] { return rig.status_entered.load(); }));
    }
    if (queued) {
        for (int pulse = 0; pulse < 10; ++pulse) rig.driver->pulse_guide(0, 5000);
    } else {
        rig.driver->pulse_guide(0, 60000);
    }
    rig.release_status.store(true);
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    rig.tick(1s);
    CHECK(rig.gr.load() == pairs);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_slewing() == moving);
    rig.tick(40000ms - rig.clock.now().time_since_epoch());
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_slewing() == moving);
    CHECK(rig.gr.load() == pairs);
    // Wall UTC did not advance with the fake clock: an older stamp proves the age was not renewed.
    CHECK(measured_stamp() < before);
    REQUIRE(rig.server.drop_connections());
    // Even during the pause, a failed guide write or an explicit exchange observes transport loss.
    CHECK_THROWS_AS(rig.driver->command_string(":GVP", false), AlpacaException);
    error([&] { rig.driver->get_slewing(); }, AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE("ZWO a polled Park error remains on both completion properties", "[zwo][telescope][telemetry][arrival]") {
    Rig rig;
    rig.connect();
    rig.driver->park();
    rig.park_error.store(true);
    rig.tick();
    for (int read = 0; read < 2; ++read) {
        error([&] { rig.driver->get_at_park(); }, AlpacaError::DriverException, "Park failed");
        error([&] { rig.driver->get_slewing(); }, AlpacaError::DriverException, "Park failed");
    }
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("ZWO a stopped poll cannot overwrite a newer command or disconnect", "[zwo][telescope][telemetry]") {
    const bool disconnect = GENERATE(false, true);
    Rig rig;
    rig.connect();
    rig.hold_status.store(true);
    rig.clock.advance(250ms);
    REQUIRE(eventually([&] { return rig.status_entered.load(); }));
    if (disconnect) {
        rig.driver->set_connected(false);
    } else {
        // GOTO claims motion before its setup can acquire the held transport.
        rig.driver->slew_to_coordinates_async(6.0, 20.0);
    }
    rig.release_status.store(true);
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
    const int polls = rig.gu.load();
    rig.silent.store(true);
    rig.clock.advance(250ms);
    REQUIRE(eventually([&] { return rig.gu.load() > polls; }));
    if (queued_goto) rig.driver->slew_to_coordinates_async(6.0, 20.0);
    const auto begin = std::chrono::steady_clock::now();
    rig.driver->set_connected(false);
    CHECK(std::chrono::steady_clock::now() - begin < (queued_goto ? 1s : 100ms));
    const int writes_at_return = rig.writes.load();
    rig.driver.reset();
    CHECK(std::chrono::steady_clock::now() - begin < 1s);
    CHECK(rig.writes.load() == writes_at_return);
}

TEST_CASE("ZWO fault recovery races disconnect without reviving a frame", "[zwo][telescope][telemetry][stress]") {
    for (int iteration = 0; iteration < 5; ++iteration) {
        Rig rig;
        rig.connect();
        rig.bad_ra.store(true);
        rig.tick();
        rig.tick();
        rig.tick();
        REQUIRE_FALSE(rig.driver->get_link_fault().empty());
        rig.bad_ra.store(false);
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
    REQUIRE(eventually([&] { return rig.pulse_commands.load() == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(2, 5s));
    rig.driver->pulse_guide(1, 100);
    rig.withhold_ra.store(true);
    const int pairs = rig.gr.load();
    rig.clock.advance(100ms);
    REQUIRE(eventually([&] { return rig.gr.load() > pairs; }));
    const auto begin = std::chrono::steady_clock::now();
    while (rig.pulse_commands.load() < 2 && std::chrono::steady_clock::now() - begin < 1s) {
        std::this_thread::yield();
    }
    CHECK(rig.pulse_commands.load() == 2);
}

TEST_CASE("ZWO a late pulse reconciliation reply cannot poison the next poll",
          "[zwo][telescope][telemetry][pulseguide]") {
    const bool dec = GENERATE(false, true);
    Rig rig(5000);
    rig.connect();
    rig.driver->pulse_guide(0, 100);
    const auto expected_dec = rig.driver->get_declination();
    REQUIRE(eventually([&] { return rig.pulse_commands.load() == 1; }));
    REQUIRE(rig.clock.wait_for_waiters(2, 5s));
    rig.delay_ra.store(!dec);
    rig.delay_dec.store(dec);
    rig.clock.advance(100ms);
    REQUIRE(eventually([&] { return rig.late_reply_ready.load(); }));
    std::this_thread::sleep_for(50ms);  // Let the late transport reply reach the socket before the next exchange.
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
    rig.bad_tracking_rate.store(failed);
    rig.connect();
    const int queries = rig.tracking_queries.load();
    REQUIRE(queries > 0);
    for (int read = 0; read < 3; ++read) {
        if (failed)
            error([&] { rig.driver->get_tracking_rate(); }, AlpacaError::DriverException, "not available");
        else
            CHECK(rig.driver->get_tracking_rate() == 0);
        CHECK(rig.tracking_queries.load() == queries);
    }
    rig.driver->set_tracking_rate(2);
    CHECK(rig.driver->get_tracking_rate() == 2);
    CHECK(rig.tracking_queries.load() == queries);
}

TEST_CASE("ZWO UTCDate getters never read the link and the poll refreshes mount time",
          "[zwo][telescope][telemetry][getter-stall]") {
    Rig rig(1000);  // The wire reader paces each character; a complete GMTI reply exceeds 100 ms.
    rig.connect();
    const int queries = rig.time_queries.load();
    const auto before = std::chrono::system_clock::now();
    const auto host = rig.driver->get_utc_date();
    CHECK(host >= before);
    CHECK(host <= std::chrono::system_clock::now());
    rig.driver->get_device_state();
    CHECK(rig.time_queries.load() == queries);
    const auto mount_time = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 3h + 4min + 5s;
    const auto client_time = mount_time - 24h;
    rig.driver->set_utc_date(client_time);
    rig.tick(1s);
    CHECK(rig.driver->get_utc_date() == client_time + 1s);
    CHECK(rig.time_queries.load() == queries);
    rig.time_year.store(20);
    rig.tick(28s);
    CHECK(rig.time_queries.load() == queries);
    rig.tick(1s);
    REQUIRE(rig.time_queries.load() == queries + 1);
    CHECK(rig.driver->get_utc_date() == mount_time);
    rig.time_year.store(21);
    rig.tick(29s);
    CHECK(rig.driver->get_utc_date() == mount_time + 29s);
    CHECK(rig.time_queries.load() == queries + 1);
    rig.tick(1s);
    CHECK(rig.driver->get_utc_date() == mount_time + 366 * 24h);
    CHECK(rig.time_queries.load() == queries + 2);
}

TEST_CASE("ZWO UTC anchor preserves fractional elapsed time and earlier or later client writes",
          "[zwo][telescope][utc][issue1522]") {
    Rig rig;
    const auto first = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + 123456789ns;
    // Offline writes remain supported, but a fresh connection starts a new lifetime.
    rig.driver->set_utc_date(first);
    CHECK(rig.driver->get_utc_date() == first);
    rig.clock.advance(123456789ns);
    CHECK(rig.driver->get_utc_date() == first + 123456789ns);
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
    const auto host_fallback = [&] {
        const auto before = std::chrono::system_clock::now();
        const auto value = rig.driver->get_utc_date();
        CHECK(value >= before);
        CHECK(value <= std::chrono::system_clock::now());
    };
    rig.driver->set_utc_date(old);
    rig.connect();  // Sync disabled: the offline anchor must not leak into this session.
    host_fallback();
    rig.driver->set_utc_date(old);
    rig.driver->set_connected(true);  // Redundant Connected=true is not a new session.
    CHECK(rig.driver->get_utc_date() == old);
    rig.driver->set_connected(false);
    host_fallback();
    // A write made after disconnect remains usable offline, including during teardown.
    rig.driver->set_utc_date(old);
    CHECK(rig.driver->get_utc_date() == old);
    rig.bad_identity.store(fail_identity);
    if (fail_identity) {
        CHECK_THROWS_AS(rig.driver->set_connected(true), AlpacaException);
        CHECK_FALSE(rig.driver->get_connected());
        host_fallback();
        rig.bad_identity.store(false);
    }
    rig.connect();
    host_fallback();
}

TEST_CASE("ZWO failed periodic time read keeps the last anchor and successful reads convert timezone",
          "[zwo][telescope][utc][issue1522]") {
    Rig rig(1000);
    rig.connect();
    const auto client = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(client);
    rig.tick(29s);
    rig.tick(1s);
    CHECK(rig.time_queries.load() > 0);
    CHECK(rig.driver->get_utc_date() == client + 30s);
    rig.time_year.store(20);
    rig.timezone_hours.store(3);
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
    const auto before = std::chrono::system_clock::now();
    CHECK(driver->get_utc_date() >= before);
    CHECK(driver->get_utc_date() <= std::chrono::system_clock::now());
}

TEST_CASE("ZWO failed initialization clears a UTC write made during the handshake",
          "[zwo][telescope][utc][issue1522]") {
    Rig rig;
    rig.hold_identity.store(true);
    rig.bad_identity.store(true);
    rig.driver->connect();
    REQUIRE(eventually([&] { return rig.identity_entered.load(); }));
    rig.driver->set_utc_date(std::chrono::sys_days{std::chrono::year{2001} / 1 / 2});
    rig.release_identity.store(true);
    REQUIRE(eventually([&] { return !rig.driver->get_connecting(); }));
    CHECK_FALSE(rig.driver->get_connected());
    const auto before = std::chrono::system_clock::now();
    CHECK(rig.driver->get_utc_date() >= before);
    CHECK(rig.driver->get_utc_date() <= std::chrono::system_clock::now());
}

TEST_CASE("ZWO disconnect cannot republish a periodic UTC read from the old session",
          "[zwo][telescope][utc][issue1522][stress]") {
    Rig rig(1000);
    rig.connect();
    rig.time_year.store(20);
    rig.tick(29s);
    rig.hold_time.store(true);
    rig.clock.advance(1s);
    REQUIRE(eventually([&] { return rig.time_entered.load(); }));
    rig.driver->set_connected(false);
    const auto offline = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(offline);
    rig.release_time.store(true);
    REQUIRE(rig.clock.wait_for_woken_settled(5s));
    CHECK(rig.driver->get_utc_date() == offline);
    rig.driver->set_connected(true);  // Joins teardown and starts with sync disabled.
    const auto before = std::chrono::system_clock::now();
    CHECK(rig.driver->get_utc_date() >= before);
}

TEST_CASE("ZWO an old UTC writer cannot replace an offline write after disconnect",
          "[zwo][telescope][utc][issue1522][stress]") {
    Rig rig(1000);
    rig.connect();
    rig.hold_time_write.store(true);
    const auto old = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    auto writer = std::async(std::launch::async, [&] {
        try {
            rig.driver->set_utc_date(old);
        } catch (const AlpacaException&) {
            // Disconnect may cancel the exchange rather than let its acknowledgement complete.
        }
    });
    const bool entered = eventually([&] { return rig.time_write_entered.load(); });
    if (!entered) rig.release_time.store(true);
    REQUIRE(entered);
    rig.driver->set_connected(false);
    const auto offline = old + 24h;
    rig.driver->set_utc_date(offline);
    rig.release_time.store(true);
    writer.get();
    CHECK(rig.driver->get_utc_date() == offline);
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
        REQUIRE(rig.server.drop_connections());
        rig.tick();
    } else {
        rig.silent.store(true);
        rig.tick();
        rig.tick();
        rig.tick();
        REQUIRE(rig.driver->get_connected());
        error([&] { rig.driver->get_utc_date(); }, AlpacaError::DriverException);
        rig.tick(30s);
        rig.silent.store(false);
    }
    REQUIRE_FALSE(rig.driver->get_connected());
    // Loss keeps its reason: unlike a deliberate disconnect, it refuses offline UTC access.
    error([&] { rig.driver->get_utc_date(); }, AlpacaError::NotConnected);
    error([&] { rig.driver->set_utc_date(old + 24h); }, AlpacaError::NotConnected);
    CHECK(rig.driver->get_device_state().empty());
    rig.connect();  // No intervening Disconnect that could hide a stale session.
    const auto before = std::chrono::system_clock::now();
    CHECK(rig.driver->get_utc_date() >= before);
    CHECK(rig.driver->get_utc_date() <= std::chrono::system_clock::now());
    CHECK(rig.gotos.load() == 0);
}

TEST_CASE("ZWO disconnect during GOTO time synchronization leaves UTC unanchored",
          "[zwo][telescope][utc][issue1522][stress]") {
    Rig rig(1000, 0, true);
    rig.connect();
    rig.hold_time_write.store(true);
    rig.driver->slew_to_coordinates_async(6.0, 20.0);
    REQUIRE(eventually([&] { return rig.time_write_entered.load(); }));
    rig.driver->set_connected(false);  // Cancels the exchange and joins the GOTO worker.
    rig.release_time.store(true);
    rig.clock.advance(10s);
    const auto before = std::chrono::system_clock::now();
    CHECK(rig.driver->get_utc_date() >= before);
    CHECK(rig.driver->get_utc_date() <= std::chrono::system_clock::now());
    const auto offline = std::chrono::sys_days{std::chrono::year{2001} / 1 / 2};
    rig.driver->set_utc_date(offline);
    rig.clock.advance(1234ms);
    CHECK(rig.driver->get_utc_date() == offline + 1234ms);
    CHECK(rig.gotos.load() == 0);
}

TEST_CASE("ZWO host time synchronization seeds UTC at connect refresh and before GOTO",
          "[zwo][telescope][utc][issue1522]") {
    const bool sync_time = GENERATE(false, true);
    Rig rig(1000, 0, sync_time);
    const auto before_connect = std::chrono::system_clock::now();
    rig.connect();
    CHECK(rig.writes.load() >= 1);  // Configured site is applied regardless of time policy.
    const auto time_writes = [&] {
        std::lock_guard<std::mutex> lock(rig.commands_mutex);
        return std::count_if(rig.commands.begin(), rig.commands.end(),
                             [](const auto& command) { return command.starts_with(":SMTI"); });
    };
    CHECK(time_writes() == (sync_time ? 1 : 0));
    CHECK(rig.driver->get_utc_date() >= before_connect);
    CHECK(rig.driver->get_utc_date() <= std::chrono::system_clock::now());
    if (sync_time) {
        const auto after_connect = std::chrono::system_clock::now();
        rig.tick(1234ms);
        CHECK(rig.driver->get_utc_date() >= before_connect + 1234ms);
        CHECK(rig.driver->get_utc_date() <= after_connect + 1234ms);
    }
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
    REQUIRE(eventually([&] { return rig.gotos.load() == 1; }));
    REQUIRE_NOTHROW(rig.driver->command_string(":GVP", false));  // Drain the setup exchange.
    CHECK(rig.driver->get_utc_date() >= before_goto);
    CHECK(rig.driver->get_utc_date() <= std::chrono::system_clock::now());
    const auto after_goto = std::chrono::system_clock::now();
    rig.tick(1234ms);
    CHECK(rig.driver->get_utc_date() >= before_goto + 1234ms);
    CHECK(rig.driver->get_utc_date() <= after_goto + 1234ms);
    CHECK(time_writes() == (sync_time ? 4 : 2));
}

TEST_CASE("ZWO a management health read cannot expire the connect warmup", "[zwo][telescope][telemetry]") {
    const bool sever_transport = GENERATE(false, true);
    Rig rig;
    rig.hold_identity.store(true);
    rig.driver->connect();
    REQUIRE(eventually([&] { return rig.identity_entered.load(); }));
    rig.clock.advance(31s);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    if (sever_transport) {
        // shutdown takes effect immediately; the held responder cannot leave until released below.
        (void)rig.server.drop_connections();
        // An unverified link is still private initialization; the connect error reports its failure below.
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_device_state().empty());
    }
    rig.release_identity.store(true);
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
